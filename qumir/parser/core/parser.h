#pragma once

#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>

#include <qumir/error.h>
#include <qumir/optional.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/pragma.h>

namespace NQumir {
namespace NAst {
namespace NCore {

using TAstTask = TExpectedTask<TExprPtr, TError, TLocation>;

struct IParseHandle {
    virtual ~IParseHandle() = default;

    virtual TAstTask Expr() = 0;
    virtual TExpectedTask<TTypePtr, TError, TLocation> Type() = 0;
    virtual TToken Next() = 0;
    virtual void Unget(TToken) = 0;
    virtual TExpectedTask<std::monostate, TError, TLocation> Take(char op) = 0;

    static bool IsOp(const TToken& tok, char op) {
        return tok.Type == TToken::Operator && tok.Value.i64 == static_cast<int64_t>(op);
    }
    static TError MakeError(const TToken& tok, const std::string& msg) {
        return TError(tok.Location, msg);
    }
};

using TNodeParser = std::function<TAstTask(IParseHandle&, TLocation)>;
using TNodeParserMap = std::unordered_map<std::string, TNodeParser>;

// Builds a custom type from the generic args of "<named Name [args]>".
using TTypeParser = std::function<std::expected<TTypePtr, std::string>(std::vector<TGenericArg>)>;
using TTypeParserMap = std::unordered_map<std::string, TTypeParser>;

class TParser {
public:
    TNodeParserMap NodeParsers;
    TTypeParserMap TypeParsers;
    std::vector<TPragma> Pragmas;

    std::expected<TExprPtr, TError> Parse(TTokenStream& stream);
};

// Reads a single type off the stream, leaving whatever follows it unconsumed.
std::expected<TTypePtr, TError> ParseType(ITokenStream& stream);
std::expected<TTypePtr, TError> ParseType(ITokenStream& stream, const TTypeParserMap& typeParsers);

} // namespace NCore
} // namespace NAst
} // namespace NQumir
