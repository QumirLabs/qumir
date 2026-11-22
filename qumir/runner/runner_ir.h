#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/modules/module.h>

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/eval.h>

#include <expected>
#include <istream>
#include <optional>

#include <unordered_set>

#include <iostream>

namespace NQumir {

struct TIRRunnerOptions {
    bool PrintAst = false;
    bool PrintTransformedAst = false;
    bool PrintIr = false;
    bool PrintByteCode = false;
    bool CoreInput = false;
    bool ResolveCoreInput = true;
    int OptLevel = 0;
    // Core frontend host prelude: modules imported on behalf of the host.
    // Empty means pure core-lang imports nothing. Ignored for the Kumir
    // frontend, which imports its own prelude.
    std::vector<std::string> Prelude;
    // Directories searched for `.oz` source modules referenced by `use`.
    std::vector<std::string> ModuleSearchPaths;
    // Explicitly registered `.oz` module files (bound by file stem).
    std::vector<std::string> ModuleFiles;
    NIR::IDebugger* Debugger = nullptr;
    std::string SourceFilePath;
};

class TIRRunner {
public:
    TIRRunner(
        std::ostream& out,
        std::istream& in,
        TIRRunnerOptions options);

    // Parses, resolves, lowers to IR and interprets the code from input.
    // Returns numeric result (if any) produced by the compiled chunk.
    std::expected<std::optional<std::string>, TError> Run(std::istream& input);

private:
    NIR::TModule Module;
    NIR::TVMCompiler Compiler;
    NIR::TRuntime Runtime;
    NIR::TBuilder Builder;
    NIR::TAstLowerer Lowerer;
    TIRRunnerOptions Options;
    NIR::TInterpreter Interpreter;
    std::unordered_set<int> PrintedChunks;

    NAst::TTypeFactory Types;
    NSemantics::TNameResolver Resolver{Types};

    std::vector<std::shared_ptr<NRegistry::IModule>> RegisteredModules;
    std::vector<std::shared_ptr<NRegistry::IModule>> AvailableModules;
};

} // namespace NQumir
