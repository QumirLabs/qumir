#pragma once

#include <qumir/runner/backend.h>

#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/modules/module.h>

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>

#include <expected>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQumir {

struct TJitRunnerOptions {
    bool PrintAst = false;
    bool PrintTransformedAst = false;
    bool PrintIr = false;
    bool CoreInput = false;
    bool ResolveCoreInput = true;
    bool AllowOverloads = false; // enable function overloads / generics (pragma language overloads)
    bool RunDefiniteAssignment = true;
    int OptLevel = 0; // 0-3, Qumir IR passes run at 1 and above
    // Core frontend host prelude: modules imported on behalf of the host.
    // Empty means pure core-lang imports nothing. Ignored for the Kumir
    // frontend, which imports its own prelude.
    std::vector<std::string> Prelude;
    // Directories searched for `.oz` source modules referenced by `use`.
    std::vector<std::string> ModuleSearchPaths;
    // Explicitly registered `.oz` module files (bound by file stem).
    std::vector<std::string> ModuleFiles;
    // Target triple override; the IR takes its pointer size from it.
    std::string TargetTriple;
    std::string SourceFilePath;
    bool EmitDebugInfo = false;
};

// A compilation session: the frontend lowers every input into one IR module
// that persists across Run() calls, so it is not thread-safe. The backend
// turns the module into machine code and runs it in this process.
class TJitRunner {
public:
    TJitRunner(TJitRunnerOptions options, std::unique_ptr<ICodegenBackend> backend);

    void RegisterModule(std::shared_ptr<NRegistry::IModule> module, bool import = false);

    // Parses, resolves, lowers to IR and runs the entry point via the backend.
    // Returns the printed result of the entry point, if any.
    std::expected<std::optional<std::string>, TError> Run(std::istream& input);

protected:
    // Persistent compiler state across Run() calls (REPL-style session)
    NIR::TModule Module;
    NIR::TBuilder Builder;
    NIR::TAstLowerer Lowerer;

    NSemantics::TNameResolver Resolver;

    std::unordered_set<int> PrintedChunks;

private:
    TJitRunnerOptions Options_;
    std::unordered_set<std::string> RegisteredModuleNames_;
    std::vector<std::shared_ptr<NRegistry::IModule>> RegisteredModules_;
    std::vector<std::shared_ptr<NRegistry::IModule>> AvailableModules_;

    std::unique_ptr<ICodegenBackend> Backend_;
};

} // namespace NQumir
