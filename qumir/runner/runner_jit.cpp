#include "runner_jit.h"

#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/turtle/turtle.h>
#include <qumir/modules/robot/robot.h>
#include <qumir/modules/drawer/drawer.h>
#include <qumir/modules/painter/painter.h>
#include <qumir/modules/colors/colors.h>
#include <qumir/modules/keyboard/keyboard.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>

#include <iostream>
#include <sstream>

namespace NQumir {

namespace {

bool IsKnown32BitTarget(const std::string& targetTriple) {
    return targetTriple.starts_with("wasm32-");
}

} // namespace

TJitRunner::TJitRunner(TJitRunnerOptions options, std::unique_ptr<ICodegenBackend> backend)
    : Module{.SourceFilePath = options.SourceFilePath}
    , Builder(Module)
    , Lowerer(Module, Builder, Resolver, {.EmitDebugInfo = options.EmitDebugInfo && options.OptLevel == 0})
    , Options_(std::move(options))
    , Backend_(std::move(backend))
{
    if (IsKnown32BitTarget(Options_.TargetTriple)) {
        Module.Types.SetPointerSize(4);
    }
    if (Options_.AllowOverloads) {
        Resolver.ApplyPragmas({NAst::TPragma{"language", {"overloads"}, {}}});
    }

    RegisterModule(std::make_shared<NRegistry::SystemModule>(), /*import=*/false);
    // Byte primitives (memcpy/memmove/memcmp) every host needs; always
    // imported, independent of CoreInput/Prelude (qumir/modules/builtins).
    RegisterModule(std::make_shared<NRegistry::BuiltinsModule>(), /*import=*/true);
    // TODO: register other modules

    AvailableModules_.push_back(std::make_shared<NRegistry::TurtleModule>());
    AvailableModules_.push_back(std::make_shared<NRegistry::RobotModule>());
    AvailableModules_.push_back(std::make_shared<NRegistry::DrawerModule>());
    AvailableModules_.push_back(std::make_shared<NRegistry::PainterModule>());
    AvailableModules_.push_back(std::make_shared<NRegistry::ColorsModule>());
    AvailableModules_.push_back(std::make_shared<NRegistry::KeyboardModule>());

    for (const auto& mod : AvailableModules_) {
        Resolver.RegisterModule(mod.get());
    }

    if (!Options_.CoreInput) {
        // Kumir prelude: standard runtime modules plus legacy module aliases.
        for (const auto& mod : RegisteredModules_) {
            (void)Resolver.ImportModule(mod->Name());
        }
        for (const auto& [alias, canonical] : NSemantics::NKumir::ModuleAliases()) {
            Resolver.RegisterModuleAlias(alias, canonical);
        }
    } else {
        // Core has no implicit prelude; the host imports what it needs.
        for (const auto& name : Options_.Prelude) {
            (void)Resolver.ImportModule(name);
        }
    }
}

void TJitRunner::RegisterModule(std::shared_ptr<NRegistry::IModule> module, bool import) {
    if (!module) {
        return;
    }
    auto name = module->Name();
    if (RegisteredModuleNames_.count(name)) {
        if (import) {
            (void)Resolver.ImportModule(name);
        }
        return;
    }
    Resolver.RegisterModule(module.get());
    RegisteredModuleNames_.insert(name);
    RegisteredModules_.push_back(std::move(module));
    if (import) {
        (void)Resolver.ImportModule(name);
    }
}

std::expected<std::optional<std::string>, TError> TJitRunner::Run(std::istream& input) {
    NFrontend::TSourceModuleLoader loader;
    for (const auto& dir : Options_.ModuleSearchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : Options_.ModuleFiles) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            return std::unexpected(reg.error());
        }
    }

    // Parse source into AST
    std::expected<NAst::TExprPtr, TError> parsed;
    std::vector<NAst::TPragma> mainPragmas;
    if (Options_.CoreInput) {
        NAst::NCore::TTokenStream ts(input);
        NAst::NCore::TParser p;
        parsed = p.Parse(ts);
        if (parsed) {
            mainPragmas = std::move(p.Pragmas);
        }
    } else {
        NAst::TTokenStream ts(input);
        NAst::TParser p;
        parsed = p.parse(ts, &Resolver, &loader);
        if (parsed) {
            mainPragmas = ts.GetContext()->GetPragmas();
        }
    }
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto ast = std::move(parsed.value());

    {
        auto composed = NFrontend::LoadAndCompose(loader, ast, mainPragmas);
        if (!composed) {
            return std::unexpected(composed.error());
        }
        ast = std::move(composed->Ast);
        Resolver.ApplyPragmas(composed->Pragmas);
    }

    // Name resolution
    auto scope = Resolver.GetOrCreateRootScope();
    // scope->AllowsRedeclare = true; // TODO: move to options?
    scope->RootLevel = false;

    if (Options_.CoreInput && Options_.ResolveCoreInput) {
        if (auto err = Resolver.Resolve(ast)) {
            return std::unexpected(*err);
        }
    }

    if (Options_.PrintAst) {
        std::cerr << "=========== AST: ===========\n";
        std::cerr << ast << std::endl;
        std::cerr << "============================\n\n";
    }

    NTransform::TPipelineExtensions extensions;
    if (Options_.CoreInput) {
        extensions.AfterTypeAnnotation.push_back(
            NTransform::CoroutineAnnotationTransform);
    } else {
        extensions = NSemantics::NKumir::PipelineExtensions();
    }
    auto pipelineOptions = NTransform::TPipelineOptions{
        .Extensions = std::move(extensions),
        .RunDefiniteAssignment = Options_.RunDefiniteAssignment,
    };
    auto error = NTransform::Pipeline(ast, Resolver, std::move(pipelineOptions));
    if (!error) {
        return std::unexpected(error.error());
    }

    if (Options_.PrintTransformedAst) {
        std::cerr << "===== TRANSFORMED AST: =====\n";
        std::cerr << ast << std::endl;
        std::cerr << "============================\n\n";
    }

    // Lower to IR: we build a fresh __repl function chunk per call
    auto lowerRes = Lowerer.LowerTop(ast);
    if (!lowerRes) {
        return std::unexpected(lowerRes.error());
    }

    if (Options_.OptLevel > 0) {
        NIR::NPasses::Pipeline(Module);
    }

    if (Options_.PrintIr) {
        std::cerr << "=========== IR: ============\n";
        for (const auto& function : Module.Functions) {
            if (PrintedChunks.insert(function.UniqueId).second) {
                std::ostringstream oss;
                function.Print(oss, Module);
                std::cerr << oss.str() << std::endl;
            }
        }
        std::cerr << "============================\n\n";
    }

    auto compiled = Backend_->Compile(Module);
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    return (*compiled)->Run();
}

} // namespace NQumir
