#pragma once

#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

namespace NQumir {
namespace NRegistry {

struct TExternalFunction {
    std::string Name;
    std::string MangledName;
    using TPacked = uint64_t(*)(const uint64_t* args, size_t argCount);
    TPacked Packed = nullptr; // packed thunk
    std::vector<NAst::TTypePtr> ArgTypes;
    NAst::TTypePtr ReturnType = nullptr;
    bool RequireArgsMaterialization = false; // if true, arguments must be materialized before calling, used for strings
    bool IsOp = false; // if true, Name is an operator symbol; no name conflict check on import
    // The compiler calls builtin::record_locator(line, byte, column of the call)
    // right before each call, so that a runtime error of this command can point at it.
    bool NeedsLocator = false;
    // Optional inline factory: receives annotated argument ASTs, returns replacement AST.
    // If set, the IR interpreter replaces the call with the returned AST.
    // Other backends (LLVM, WASM) use the packed thunk / mangled symbol.
    using TInlineFactory = std::function<NAst::TExprPtr(std::vector<NAst::TExprPtr>)>;
    std::optional<TInlineFactory> Inline;

    mutable std::vector<uint32_t> NameCodePoints;
};

struct TExternalType {
    std::string Name;
    NAst::TTypePtr Type = nullptr; // must be unwrapped type, not named type
    bool IsAlias = false;
};

class IModule {
public:
    virtual ~IModule() = default;
    virtual const std::string& Name() const = 0;
    virtual const std::vector<TExternalFunction>& ExternalFunctions() const = 0;
    virtual const std::vector<TExternalType>& ExternalTypes() const = 0;
    virtual const std::vector<TLiteralSuffix>& LiteralSuffixes() const = 0;
    virtual const std::vector<std::string>& Dependencies() const = 0;
    // Imported by the host itself or as a dependency of another module;
    // programs never name it in a use statement, so it is not offered to them.
    virtual bool IsImplicit() const { return false; }
};

} // namespace NRegistry
} // namespace NQumir
