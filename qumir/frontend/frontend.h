#pragma once

#include <qumir/error.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/debug_options.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <expected>
#include <istream>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQumir {
namespace NFrontend {

struct TFrontendOptions {
    bool PrintAst = false;
    bool PrintTransformedAst = false;
    bool PrintIr = false;
    bool CoreInput = false;
    // Resolves core input in Parse(), before the AST print; the transforms resolve it anyway.
    bool ResolveCoreInput = true;
    bool RunDefiniteAssignment = true;
    // Compile() runs the Qumir IR passes at 1 and above.
    int OptLevel = 0;
    // Directories searched for `.oz` source modules referenced by `use`.
    std::vector<std::string> ModuleSearchPaths;
    // Explicitly registered `.oz` module files (bound by file stem).
    std::vector<std::string> ModuleFiles;
    // Composes the `debug` source module into the program (browser debug points).
    bool IncludeDebugRuntime = false;
    std::string SourceFilePath;
    NIR::TDebugOptions DebugOptions;
};

// Source to Qumir IR: parsing, composition with the `.oz` source modules,
// semantic transforms and lowering into Module. The host registers registry
// modules in the resolver. The state accumulates across calls (a REPL
// session), so a frontend is not thread-safe.
class TFrontend {
public:
    explicit TFrontend(TFrontendOptions options);

    // Parses the program and composes it with the source modules it uses.
    std::expected<NAst::TExprPtr, TError> Parse(std::istream& input);
    // Resolves names, annotates types and runs the semantic rewrites.
    std::expected<void, TError> Transform(NAst::TExprPtr& ast);
    // Lowers a transformed program into the IR module.
    std::expected<void, TError> Lower(const NAst::TExprPtr& ast);
    // Runs the Qumir IR passes over the IR module.
    void Optimize();

    // All stages with the requested prints; the IR passes run at OptLevel > 0.
    std::expected<void, TError> Compile(std::istream& input);

    NIR::TModule& GetModule();
    NSemantics::TNameResolver& GetResolver();

private:
    TFrontendOptions Options_;

protected:
    NIR::TModule Module;
    NIR::TBuilder Builder;
    NIR::TAstLowerer Lowerer;

    NSemantics::TNameResolver Resolver;

    // IR functions already printed: a REPL session prints each one once.
    std::unordered_set<int> PrintedChunks;
};

} // namespace NFrontend
} // namespace NQumir
