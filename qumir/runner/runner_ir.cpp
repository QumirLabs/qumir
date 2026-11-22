#include "runner_ir.h"

#include <exception>
#include <qumir/parser/lexer.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
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
#include <qumir/frontend/source_module_loader.h>
#include <qumir/frontend/compose.h>

#include <iostream>
#include <sstream>

namespace NQumir {

using namespace NIR;
using namespace NAst;

TIRRunner::TIRRunner(
    std::ostream& out,
    std::istream& in,
    TIRRunnerOptions options)
    : Module{.SourceFilePath = options.SourceFilePath}
    , Compiler(Module)
    , Builder(Module)
    , Lowerer(Module, Builder, Resolver, {.EmitDebugInfo = options.Debugger != nullptr})
    , Options(std::move(options))
    , Interpreter(Module, Compiler, out, in, Options.Debugger)
{
    RegisteredModules.push_back(std::make_shared<NRegistry::SystemModule>(Types));
    // TODO: register other modules

    AvailableModules.push_back(std::make_shared<NRegistry::TurtleModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::RobotModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::DrawerModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::PainterModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::ColorsModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::KeyboardModule>(Types));

    for (const auto& mod : RegisteredModules) {
        Resolver.RegisterModule(mod.get());
    }
    for (const auto& mod : AvailableModules) {
        Resolver.RegisterModule(mod.get());
    }

    // Byte primitives (memcpy/memmove/memcmp) every host needs; always
    // imported, independent of CoreInput/Prelude (qumir/modules/builtins).
    auto builtins = std::make_shared<NRegistry::BuiltinsModule>(Types);
    Resolver.RegisterModule(builtins.get());
    (void)Resolver.ImportModule(builtins->Name());
    RegisteredModules.push_back(std::move(builtins));

    if (!Options.CoreInput) {
        // Kumir prelude: standard runtime modules plus legacy module aliases.
        for (const auto& mod : RegisteredModules) {
            (void)Resolver.ImportModule(mod->Name());
        }
        for (const auto& [alias, canonical] : NSemantics::NKumir::ModuleAliases()) {
            Resolver.RegisterModuleAlias(alias, canonical);
        }
    } else {
        // Core has no implicit prelude; the host imports what it needs.
        for (const auto& name : Options.Prelude) {
            (void)Resolver.ImportModule(name);
        }
    }
}

std::expected<std::optional<std::string>, TError> TIRRunner::Run(std::istream& input) {
    NFrontend::TSourceModuleLoader loader(Types);
    for (const auto& dir : Options.ModuleSearchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : Options.ModuleFiles) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            return std::unexpected(reg.error());
        }
    }

    std::expected<TExprPtr, TError> parsed;
    std::vector<NAst::TPragma> mainPragmas;
    if (Options.CoreInput) {
        NCore::TTokenStream ts(input);
        NCore::TParser p(Types);
        parsed = p.Parse(ts);
        if (parsed) {
            mainPragmas = std::move(p.Pragmas);
        }
    } else {
        TTokenStream ts(input);
        TParser p(Types);
        parsed = p.parse(ts, &Resolver, &loader);
        if (parsed) {
            mainPragmas = ts.GetContext()->GetPragmas();
            Resolver.ApplyPragmas(mainPragmas);
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

    auto scope = Resolver.GetOrCreateRootScope();
    // scope->AllowsRedeclare = true; // TODO: move to options?
    scope->RootLevel = false;

    if (Options.CoreInput && Options.ResolveCoreInput) {
        if (auto err = Resolver.Resolve(ast)) {
            return std::unexpected(*err);
        }
    }

    if (Options.PrintAst) {
        std::cerr << "=========== AST: ===========\n";
        std::cerr << ast << std::endl;
        std::cerr << "============================\n\n";
    }

    NTransform::TPipelineExtensions extensions;
    if (Options.CoreInput) {
        extensions.AfterTypeAnnotation.push_back(
            NTransform::CoroutineAnnotationTransform);
    } else {
        extensions = NSemantics::NKumir::PipelineExtensions();
    }
    auto pipelineOptions = NTransform::TPipelineOptions{
        .Extensions = std::move(extensions),
    };
    auto error = NTransform::Pipeline(ast, Resolver, std::move(pipelineOptions));
    if (!error) {
        return std::unexpected(error.error());
    }

    if (Options.PrintTransformedAst) {
        std::cerr << "===== TRANSFORMED AST: =====\n";
        std::cerr << ast << std::endl;
        std::cerr << "============================\n\n";
    }

    auto lowerRes = Lowerer.LowerTop(ast);
    if (!lowerRes) {
        return std::unexpected(lowerRes.error());
    }
    if (Options.OptLevel > 0) {
        NIR::NPasses::Pipeline(Module);
    }

    auto* mainFun = Module.GetEntryPoint();

    if (Options.PrintIr) {
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

    if (!mainFun) {
        return std::unexpected(TError(TLocation(), "no <main> function found"));
    }

    // Interpret
    try {
        auto debugger = Options.Debugger;
        if (debugger) {
            debugger->SetModule(Module);
            for (auto& f : Module.Functions) {
                if (!f.Exec) {
                    f.Exec = &Compiler.Compile(f, Options.PrintByteCode);
                    debugger->OnFunctionCompilationFinished(f);
                }
            }
            debugger->OnModuleCompilationFinished();
        }
        auto res = Interpreter.Eval(*mainFun, {}, TInterpreter::TOptions{.PrintByteCode = Options.PrintByteCode});
        return res;
    } catch (const std::exception& e) {
        // TODO: free resources?
        return std::unexpected(TError(std::string("runtime error: ") + e.what()));
    }
}

} // namespace NQumir
