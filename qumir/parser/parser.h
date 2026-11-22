#pragma once

#include <qumir/error.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/lexer.h>
#include <qumir/module_manager.h>

#include <expected>

namespace NQumir {

namespace NFrontend {
class TSourceModuleLoader;
}

namespace NAst {

class TParser {
public:
    explicit TParser(TTypeFactory& types)
        : Types(types)
    { }

    // Shared by the whole compilation; all types are created through it.
    TTypeFactory& Types;

    // moduleManager: if non-null, `использовать X` imports X at parse time;
    // type names from imported modules are registered in the stream's TLexerContext.
    // loader: if non-null, `использовать X` where X resolves to an `.oz` source
    // module emits a plain use node (the module is composed in after parsing)
    // instead of a runtime import.
    std::expected<TExprPtr, TError> parse(
        TTokenStream& stream,
        IModuleManager* moduleManager = nullptr,
        NFrontend::TSourceModuleLoader* loader = nullptr);
};

} // namespace NAst
} // namespace NQumir