#include "frontend.h"

#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/semantics/transform/transform.h>

#include <iostream>
#include <sstream>

namespace NQumir {
namespace NFrontend {

TFrontend::TFrontend(TFrontendOptions options)
    : Options_(std::move(options))
    , Module{.SourceFilePath = Options_.SourceFilePath}
    , Builder(Module)
    , Lowerer(Module, Builder, Resolver, Options_.DebugOptions)
{}

std::expected<NAst::TExprPtr, TError> TFrontend::Parse(std::istream& input) {
    TSourceModuleLoader loader(Types);
    for (const auto& dir : Options_.ModuleSearchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : Options_.ModuleFiles) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            return std::unexpected(reg.error());
        }
    }

    std::expected<NAst::TExprPtr, TError> parsed;
    std::vector<NAst::TPragma> mainPragmas;
    if (Options_.CoreInput) {
        NAst::NCore::TTokenStream ts(input);
        NAst::NCore::TParser p(Types);
        parsed = p.Parse(ts);
        if (parsed) {
            mainPragmas = std::move(p.Pragmas);
        }
    } else {
        NAst::TTokenStream ts(input);
        NAst::TParser p(Types);
        parsed = p.parse(ts, &Resolver, &loader);
        if (parsed) {
            mainPragmas = ts.GetContext()->GetPragmas();
        }
    }
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto ast = std::move(parsed.value());

    if (Options_.IncludeDebugRuntime) {
        auto runtime = loader.Load("debug");
        if (!runtime) {
            return std::unexpected(runtime.error());
        }
        for (auto& stmt : NAst::TMaybeNode<NAst::TBlockExpr>((*runtime)->Ast).Cast()->Stmts) {
            if (auto function = NAst::TMaybeNode<NAst::TFunDecl>(stmt)) {
                function.Cast()->Used = true;
            }
        }
    }

    auto composed = LoadAndCompose(loader, ast, mainPragmas);
    if (!composed) {
        return std::unexpected(composed.error());
    }
    ast = std::move(composed->Ast);
    Resolver.ApplyPragmas(composed->Pragmas);

    // Name resolution
    auto scope = Resolver.GetOrCreateRootScope();
    // scope->AllowsRedeclare = true; // TODO: move to options?
    scope->RootLevel = false;

    if (Options_.CoreInput && Options_.ResolveCoreInput) {
        if (auto err = Resolver.Resolve(ast)) {
            return std::unexpected(*err);
        }
    }
    return ast;
}

std::expected<void, TError> TFrontend::Transform(NAst::TExprPtr& ast) {
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
    auto result = NTransform::Pipeline(ast, Resolver, std::move(pipelineOptions));
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

std::expected<void, TError> TFrontend::Lower(const NAst::TExprPtr& ast) {
    auto result = Lowerer.LowerTop(ast);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

void TFrontend::Optimize() {
    NIR::NPasses::Pipeline(Module);
}

std::expected<void, TError> TFrontend::Compile(std::istream& input) {
    auto ast = Parse(input);
    if (!ast) {
        return std::unexpected(ast.error());
    }

    if (Options_.PrintAst) {
        std::cerr << "=========== AST: ===========\n";
        std::cerr << *ast << std::endl;
        std::cerr << "============================\n\n";
    }

    if (auto transformed = Transform(*ast); !transformed) {
        return std::unexpected(transformed.error());
    }

    if (Options_.PrintTransformedAst) {
        std::cerr << "===== TRANSFORMED AST: =====\n";
        std::cerr << *ast << std::endl;
        std::cerr << "============================\n\n";
    }

    // Lower to IR: we build a fresh __repl function chunk per call
    if (auto lowered = Lower(*ast); !lowered) {
        return std::unexpected(lowered.error());
    }

    if (Options_.OptLevel > 0) {
        Optimize();
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
    return {};
}

NIR::TModule& TFrontend::GetModule() {
    return Module;
}

NSemantics::TNameResolver& TFrontend::GetResolver() {
    return Resolver;
}

} // namespace NFrontend
} // namespace NQumir
