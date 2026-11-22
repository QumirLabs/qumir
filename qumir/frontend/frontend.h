#pragma once

#include <qumir/error.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/debug_options.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <expected>
#include <istream>
#include <optional>
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

// The state accumulates across calls (a REPL session), so a frontend is not
// thread-safe; the host registers the registry modules.
class TFrontend {
public:
    // The factory every type of this session comes from. An AST handed to this frontend
    // must be built with it (types are interned per factory, equal types are one object).
    NAst::TTypeFactory& TypeFactory() { return Types; }

    // `types` is the host's factory, which must outlive the frontend. Without one the
    // frontend owns a factory (qumirc, tests).
    explicit TFrontend(TFrontendOptions options, NAst::TTypeFactory* types = nullptr);

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
    // Set only when no host factory is passed; declared first, so it outlives the rest.
    std::optional<NAst::TTypeFactory> OwnedTypes_;

protected:
    // Every type of this session comes from it, so it outlives the IR module, the
    // lowerer, the resolver and a derived runner's codegen.
    NAst::TTypeFactory& Types;
    NIR::TModule Module;
    NIR::TBuilder Builder;
    NIR::TAstLowerer Lowerer;

    NSemantics::TNameResolver Resolver{Types};

    // IR functions already printed: a REPL session prints each one once.
    std::unordered_set<int> PrintedChunks;
};

} // namespace NFrontend
} // namespace NQumir
