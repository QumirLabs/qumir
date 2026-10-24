#include "parser.h"

#include <qumir/optional.h>
#include <qumir/parser/operator.h>
#include <qumir/parser/pragma.h>
#include <qumir/parser/type.h>

#include <algorithm>

#include <string>
#include <unordered_set>
#include <vector>

namespace NQumir {
namespace NAst {
namespace NCore {

namespace {

using TAstTask = TExpectedTask<TExprPtr, TError, TLocation>;
using TTypeTask = TExpectedTask<TTypePtr, TError, TLocation>;
using TExprListTask = TExpectedTask<std::vector<TExprPtr>, TError, TLocation>;
using TTypeListTask = TExpectedTask<std::vector<TTypePtr>, TError, TLocation>;
using TGenericArgListTask = TExpectedTask<std::vector<TGenericArg>, TError, TLocation>;
using TOutputArgTask = TExpectedTask<TOutputArg, TError, TLocation>;
using TOutputArgListTask = TExpectedTask<std::vector<TOutputArg>, TError, TLocation>;
using TParamListTask = TExpectedTask<std::vector<TParam>, TError, TLocation>;
using TGenericParamListTask = TExpectedTask<std::vector<TGenericParam>, TError, TLocation>;
using TBoundsTask = TExpectedTask<std::vector<std::pair<TExprPtr, TExprPtr>>, TError, TLocation>;

struct TParserContext;

using TListHandler = std::function<TAstTask(TParserContext&, TLocation)>;
using TListHandlerMap = std::unordered_map<std::string, TListHandler>;

struct TParserContext {
    TWrappedTokenStream& Stream;
    TListHandlerMap Handlers;
    std::vector<TPragma> Pragmas;
    int BlockDepth = 0;
};

std::shared_ptr<TIntegerType> IntegerTypeByName(const std::string& name) {
    using K = TIntegerType::EKind;
    if (name == "i8") return std::make_shared<TIntegerType>(K::I8);
    if (name == "i16") return std::make_shared<TIntegerType>(K::I16);
    if (name == "i32") return std::make_shared<TIntegerType>(K::I32);
    if (name == "i64") return std::make_shared<TIntegerType>(K::I64);
    if (name == "i128") return std::make_shared<TIntegerType>(K::I128);
    if (name == "u8") return std::make_shared<TIntegerType>(K::U8);
    if (name == "u16") return std::make_shared<TIntegerType>(K::U16);
    if (name == "u32") return std::make_shared<TIntegerType>(K::U32);
    if (name == "u64") return std::make_shared<TIntegerType>(K::U64);
    if (name == "u128") return std::make_shared<TIntegerType>(K::U128);
    return nullptr;
}

TTypePtr ScalarTypeByName(const std::string& name) {
    if (auto integerType = IntegerTypeByName(name)) return integerType;
    if (name == "f64") return std::make_shared<TFloatType>();
    if (name == "bool") return std::make_shared<TBoolType>();
    if (name == "string") return std::make_shared<TStringType>();
    if (name == "char") return std::make_shared<TSymbolType>();
    if (name == "void") return std::make_shared<TVoidType>();
    return nullptr;
}

bool IsOp(const TToken& token, char op) {
    return token.Type == TToken::Operator && token.Value.i64 == static_cast<int64_t>(op);
}

bool IsNil(const TToken& token) {
    return token.Type == TToken::Identifier && token.Name == "nil";
}

std::string TokenText(const TToken& token) {
    if (token.Type == TToken::Identifier || token.Type == TToken::String) {
        return token.Name;
    }
    if (token.Type == TToken::Operator) {
        return TOperator(static_cast<uint64_t>(token.Value.i64)).ToString();
    }
    return token.RawValue;
}

TError Error(const TToken& token, const std::string& message) {
    return TError(token.Location, message);
}

TExpectedTask<std::monostate, TError, TLocation> Expect(TParserContext& context, char op);

TExpectedTask<std::string, TError, TLocation> ParseName(TParserContext& context) {
    auto token = context.Stream.Next();
    if (IsOp(token, '<')) {
        auto inner = context.Stream.Next();
        if (inner.Type != TToken::Identifier) {
            co_return Error(inner, "expected angle-bracket identifier");
        }
        co_await Expect(context, '>');
        co_return "<" + inner.Name + ">";
    }
    if (token.Type != TToken::Identifier) {
        co_return Error(token, "expected identifier");
    }
    co_return token.Name;
}

TExpectedTask<std::string, TError, TLocation> ParseNameLike(TParserContext& context) {
    auto token = context.Stream.Next();
    if (token.Type != TToken::Identifier && token.Type != TToken::String) {
        co_return Error(token, "expected name");
    }
    co_return token.Name;
}

TExpectedTask<std::monostate, TError, TLocation> Expect(TParserContext& context, char op) {
    auto token = context.Stream.Next();
    if (!IsOp(token, op)) {
        co_return Error(token, std::string("expected '") + op + "'");
    }
    co_return std::monostate{};
}

TAstTask ParseExpr(TParserContext& context);
TTypeTask ParseType(TParserContext& context);

TExprPtr ApplyTypeAnnotation(TExprPtr expr, TTypePtr type) {
    if (auto maybeNum = TMaybeNode<TNumberExpr>(expr)) {
        auto num = maybeNum.Cast();
        if (TMaybeType<TFloatType>(type) && !num->IsFloat()) {
            expr = std::make_shared<TNumberExpr>(expr->Location, static_cast<double>(num->IntValue));
        } else if (TMaybeType<TIntegerType>(type) && num->IsFloat()) {
            expr = std::make_shared<TNumberExpr>(expr->Location, static_cast<int64_t>(num->FloatValue));
        } else if (TMaybeType<TBoolType>(type)) {
            expr = std::make_shared<TNumberExpr>(expr->Location, num->IsFloat() ? num->FloatValue != 0.0 : num->IntValue != 0);
        } else if (TMaybeType<TSymbolType>(type)) {
            expr = std::make_shared<TNumberExpr>(expr->Location, num->IntValue);
        }
    }
    expr->Type = std::move(type);
    return expr;
}

TExprListTask ParseExprsUntil(TParserContext& context, char close) {
    std::vector<TExprPtr> result;
    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, close)) {
            co_return result;
        }
        if (token.IsEof()) {
            co_return Error(token, std::string("expected '") + close + "'");
        }
        context.Stream.Unget(token);
        result.push_back(co_await ParseExpr(context));
    }
}

TTypeListTask ParseTypeList(TParserContext& context) {
    co_await Expect(context, '(');
    std::vector<TTypePtr> result;
    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ')')) {
            co_return result;
        }
        if (token.IsEof()) {
            co_return Error(token, "expected ')' in type list");
        }
        context.Stream.Unget(token);
        result.push_back(co_await ParseType(context));
    }
}

TGenericArgListTask ParseGenericTypeArgs(TParserContext& context) {
    std::vector<TGenericArg> result;
    auto tok = context.Stream.Next();
    if (!IsOp(tok, '[')) {
        context.Stream.Unget(tok);
        co_return result;
    }

    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ']')) {
            co_return result;
        }
        if (token.IsEof()) {
            co_return Error(token, "expected ']' in generic type argument list");
        }
        if (token.Type == TToken::Integer) {
            result.push_back(TGenericArg::ValueArg(std::to_string(token.Value.i64)));
            continue;
        }
        if (IsOp(token, '(')
            || token.Type == TToken::Float
            || token.Type == TToken::Boolean
            || token.Type == TToken::Char
            || token.Type == TToken::String)
        {
            co_return Error(token, "value generic arguments are not supported yet");
        }
        context.Stream.Unget(token);
        result.push_back(TGenericArg::TypeArg(co_await ParseType(context)));
    }
}

TBoundsTask ParseBounds(TParserContext& context) {
    std::vector<std::pair<TExprPtr, TExprPtr>> result;
    while (true) {
        auto token = context.Stream.Next();
        if (!IsOp(token, '[')) {
            context.Stream.Unget(token);
            co_return result;
        }
        auto from = co_await ParseExpr(context);
        auto to = co_await ParseExpr(context);
        co_await Expect(context, ']');
        result.emplace_back(std::move(from), std::move(to));
    }
}

TExprListTask ParseIndexVector(TParserContext& context) {
    co_await Expect(context, '[');
    co_return co_await ParseExprsUntil(context, ']');
}

TParamListTask ParseParams(TParserContext& context) {
    co_await Expect(context, '(');
    std::vector<TParam> result;
    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ')')) {
            co_return result;
        }
        context.Stream.Unget(token);
        auto param = co_await ParseExpr(context);
        auto var = TMaybeNode<TVarStmt>(param);
        if (!var) {
            co_return TError(param ? param->Location : context.Stream.GetLocation(), "expected parameter var");
        }
        result.push_back(var.Cast());
    }
    co_return result;
}

TGenericParamListTask ParseGenericParams(TParserContext& context) {
    std::vector<TGenericParam> result;
    std::unordered_set<std::string> names;
    auto tok = context.Stream.Next();
    if (!IsOp(tok, '[')) {
        context.Stream.Unget(tok);
        co_return result;
    }

    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ']')) {
            co_return result;
        }
        if (token.IsEof()) {
            co_return Error(token, "expected ']' in generic parameter list");
        }
        if (IsOp(token, '(')) {
            auto head = co_await ParseName(context);
            std::string name;
            if (head == "const") {
                name = co_await ParseName(context);
            } else {
                name = std::move(head);
            }
            if (!names.insert(name).second) {
                co_return Error(token, "duplicate generic parameter: " + name);
            }
            auto type = co_await ParseType(context);
            co_await Expect(context, ')');

            result.emplace_back(TGenericParam {
                .Name = std::move(name),
                .Kind = TGenericParam::EKind::Value,
                .ValueType = std::move(type),
            });
        } else {
            context.Stream.Unget(token);
            auto name = co_await ParseName(context);
            if (!names.insert(name).second) {
                co_return Error(token, "duplicate generic parameter: " + name);
            }
            result.emplace_back(TGenericParam {
                .Name = std::move(name),
                .Kind = TGenericParam::EKind::Type,
            });
        }
    }
}

TOutputArgTask ParseOutputArg(TParserContext& context) {
    auto token = context.Stream.Next();
    if (IsOp(token, '(')) {
        auto head = context.Stream.Next();
        if (head.Type == TToken::Identifier && head.Name == "fmt") {
            auto expr = co_await ParseExpr(context);
            auto width = co_await ParseExpr(context);
            TExprPtr precision;
            auto t = context.Stream.Next();
            if (!IsOp(t, ')')) {
                context.Stream.Unget(t);
                precision = co_await ParseExpr(context);
                co_await Expect(context, ')');
            }
            co_return TOutputArg{
                .Expr = std::move(expr),
                .Width = std::move(width),
                .Precision = std::move(precision),
            };
        }
        context.Stream.Unget(head);
        context.Stream.Unget(token);
    } else {
        context.Stream.Unget(token);
    }
    co_return TOutputArg{.Expr = co_await ParseExpr(context)};
}

TOutputArgListTask ParseOutputArgs(TParserContext& context) {
    std::vector<TOutputArg> result;
    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ')')) {
            co_return result;
        }
        context.Stream.Unget(token);
        result.push_back(co_await ParseOutputArg(context));
    }
}

TExpectedTask<std::monostate, TError, TLocation> SkipBalancedList(TParserContext& context) {
    co_await Expect(context, '(');
    int depth = 1;
    while (depth > 0) {
        auto token = context.Stream.Next();
        if (token.IsEof()) {
            co_return Error(token, "unexpected eof in list");
        }
        if (IsOp(token, '(')) {
            ++depth;
        } else if (IsOp(token, ')')) {
            --depth;
        }
    }
    co_return std::monostate{};
}

struct TFunAttrs {
    TExprPtr After;  // LastAssert
    TExprPtr Before; // pre-condition (future)
};

TExpectedTask<TFunAttrs, TError, TLocation> ParseFunAttrs(TParserContext& context) {
    co_await Expect(context, '(');
    TFunAttrs attrs;
    while (true) {
        auto token = context.Stream.Next();
        if (IsOp(token, ')')) {
            break;
        }
        if (IsOp(token, '(')) {
            // compound attr: (name expr)
            auto name = co_await ParseName(context);
            auto expr = co_await ParseExpr(context);
            co_await Expect(context, ')');
            if (name == "expect_after") {
                attrs.After = std::move(expr);
            } else if (name == "expect_before") {
                attrs.Before = std::move(expr);
            }
            // unknown attrs silently ignored for forward compat
        } else if (token.Type == TToken::Identifier) {
            // simple attr: inline, etc. - ignored for now
        } else {
            co_return Error(token, "expected function attribute");
        }
    }
    co_return attrs;
}

TAstTask ParseList(TParserContext& context, TLocation location) {
    auto headToken = context.Stream.Next();
    if (headToken.Type != TToken::Identifier && headToken.Type != TToken::Operator) {
        co_return Error(headToken, "expected form name");
    }
    const auto head = TokenText(headToken);

    if (auto it = context.Handlers.find(head); it != context.Handlers.end()) {
        co_return co_await it->second(context, location);
    }

    static const std::unordered_set<std::string> KnownOps = {
        "+", "-", "*", "/", "//", "%", "^", "&", "|", "~", "!",
        "==", "!=", "<", "<=", ">", ">=", "&&", "||", "<<", ">>",
        "xor",
    };
    // Associative operators accept more than two arguments and fold left into
    // a chain of binary nodes: (op a b c) => (op (op a b) c).
    static const std::unordered_set<std::string> VariadicOps = {
        "+", "-", "*", "/", "//", "&", "|", "^", "&&", "||", "xor",
    };
    auto args = co_await ParseExprsUntil(context, ')');
    if (!KnownOps.count(head)) {
        co_return TError(location, "unknown core form: " + head);
    }
    if (args.size() == 1) {
        co_return std::make_shared<TUnaryExpr>(location, TOperator(head), std::move(args[0]));
    }
    if (args.size() > 2 && !VariadicOps.count(head)) {
        co_return TError(location, "operator is not variadic: " + head);
    }
    if (args.size() >= 2) {
        auto op = head == "xor" ? "^" : head;
        auto result = std::move(args[0]);
        for (size_t i = 1; i < args.size(); ++i) {
            result = std::make_shared<TBinaryExpr>(
                location,
                TOperator(op),
                std::move(result),
                std::move(args[i]));
        }
        co_return result;
    }
    co_return TError(location, "unknown core form: " + head);
}

TAstTask ParseExpr(TParserContext& context) {
    auto token = context.Stream.Next();
    if (IsNil(token)) {
        co_return TExprPtr{};
    }
    if (token.Type == TToken::Integer) {
        co_return std::make_shared<TNumberExpr>(token.Location, token.Value.i64);
    }
    if (token.Type == TToken::Float) {
        co_return std::make_shared<TNumberExpr>(token.Location, token.Value.f64);
    }
    if (token.Type == TToken::Boolean) {
        co_return std::make_shared<TNumberExpr>(token.Location, token.Value.i64 != 0);
    }
    if (token.Type == TToken::Char) {
        auto expr = std::make_shared<TNumberExpr>(token.Location, token.Value.i64);
        expr->Type = std::make_shared<TSymbolType>();
        co_return expr;
    }
    if (token.Type == TToken::String) {
        co_return std::make_shared<TStringLiteralExpr>(token.Location, token.Name);
    }
    if (token.Type == TToken::Identifier) {
        if (token.Name == "break") co_return std::make_shared<TBreakStmt>(token.Location);
        if (token.Name == "continue") co_return std::make_shared<TContinueStmt>(token.Location);
        co_return std::make_shared<TIdentExpr>(token.Location, token.Name);
    }
    if (IsOp(token, '(')) {
        co_return co_await ParseList(context, token.Location);
    }
    co_return Error(token, "expected expression");
}

// Reads an optional "(...)" attribute list and applies the modifiers to type.
// Access modifiers (mutable/readonly/writeonly) are mutually exclusive; the
// Access modifiers (mutable/readonly/writeonly) are mutually exclusive.
TExpectedTask<std::monostate, TError, TLocation> ParseTypeAttrs(TParserContext& context, TType& type) {
    auto token = context.Stream.Next();
    if (!IsOp(token, '(')) {
        context.Stream.Unget(token);
        co_return std::monostate{};
    }
    bool accessSeen = false;
    while (true) {
        token = context.Stream.Next();
        if (IsOp(token, ')')) {
            break;
        }
        if (token.Type != TToken::Identifier) {
            co_return Error(token, "expected type attribute");
        }
        if (token.Name == "mutable" || token.Name == "readonly" || token.Name == "writeonly") {
            if (accessSeen) {
                co_return Error(token, "duplicate or conflicting access type attribute: " + token.Name);
            }
            accessSeen = true;
            type.Readable = token.Name != "writeonly";
            type.Mutable = token.Name != "readonly";
        } else {
            co_return Error(token, "unknown type attribute: " + token.Name);
        }
    }
    co_return std::monostate{};
}

TTypeTask ParseCompositeType(TParserContext& context, TLocation location) {
    auto head = co_await ParseName(context);

    // Scalar types wrapped in angle brackets: <i64 (mutable)> etc.
    if (auto scalar = ScalarTypeByName(head)) {
        co_await ParseTypeAttrs(context, *scalar);
        co_await Expect(context, '>');
        co_return scalar;
    }

    if (head == "vec") {
        auto elementType = co_await ParseType(context);
        auto sizeToken = context.Stream.Next();
        auto vector = std::make_shared<TVectorType>(std::move(elementType), 0);
        if (sizeToken.Type == TToken::Integer) {
            vector->BindSize(std::to_string(sizeToken.Value.i64));
        } else if (sizeToken.Type == TToken::Identifier) {
            vector->SizeParam = sizeToken.Name;
        } else {
            co_return Error(sizeToken, "expected vector size or generic value parameter");
        }
        co_await Expect(context, '>');
        co_return vector;
    }
    if (head == "tensor") {
        auto elementType = co_await ParseType(context);
        if (!TMaybeType<TIntegerType>(elementType) && !TMaybeType<TFloatType>(elementType) && !TMaybeType<TBoolType>(elementType)) {
            co_return TError(location, "tensor element type must be integer, float, or bool");
        }
        std::vector<int> shape;
        while (true) {
            auto dimToken = context.Stream.Next();
            if (IsOp(dimToken, '>')) break;
            if (dimToken.Type != TToken::Integer) co_return Error(dimToken, "expected tensor dimension");
            shape.push_back(static_cast<int>(dimToken.Value.i64));
        }
        if (shape.empty()) {
            co_return TError(location, "tensor must have at least one dimension");
        }
        co_return std::make_shared<TTensorType>(std::move(elementType), std::move(shape));
    }

    if (head == "fun") {
        auto returnType = co_await ParseType(context);
        auto params = co_await ParseTypeList(context);
        while (true) {
            auto token = context.Stream.Next();
            if (IsOp(token, '>')) break;
            if (token.IsEof()) co_return Error(token, "expected '>'");
        }
        co_return std::make_shared<TFunctionType>(std::move(params), std::move(returnType));
    }
    if (head == "future") {
        auto resultType = co_await ParseType(context);
        auto type = std::make_shared<TFutureType>(std::move(resultType));
        co_await ParseTypeAttrs(context, *type);
        co_await Expect(context, '>');
        co_return type;
    }
    if (head == "array") {
        auto elementType = co_await ParseType(context);
        auto arity = context.Stream.Next();
        if (arity.Type != TToken::Integer) co_return Error(arity, "expected array arity");
        co_await Expect(context, '>');
        co_return std::make_shared<TArrayType>(std::move(elementType), static_cast<int>(arity.Value.i64));
    }
    if (head == "ptr") {
        auto pointee = co_await ParseType(context);
        auto type = std::make_shared<TPointerType>(std::move(pointee));
        co_await ParseTypeAttrs(context, *type);
        co_await Expect(context, '>');
        co_return type;
    }
    if (head == "ref") {
        auto referenced = co_await ParseType(context);
        auto type = std::make_shared<TReferenceType>(std::move(referenced));
        co_await ParseTypeAttrs(context, *type);
        co_await Expect(context, '>');
        co_return type;
    }
    if (head == "named") {
        auto name = co_await ParseName(context);
        auto typeArgs = co_await ParseGenericTypeArgs(context);
        auto type = std::make_shared<TNamedType>(std::move(name), nullptr, std::move(typeArgs));
        auto tok = context.Stream.Next();
        if (IsOp(tok, '>')) {
            co_return type;
        }
        // "<named Name (attrs)>" - a reference with no inline underlying type,
        // as opposed to "<named Name UnderlyingType (attrs)>".
        if (IsOp(tok, '(')) {
            context.Stream.Unget(tok);
            co_await ParseTypeAttrs(context, *type);
            co_await Expect(context, '>');
            co_return type;
        }
        context.Stream.Unget(tok);
        type->UnderlyingType = co_await ParseType(context);
        co_await ParseTypeAttrs(context, *type);
        co_await Expect(context, '>');
        co_return type;
    }
    if (head == "struct") {
        std::vector<std::pair<std::string, TTypePtr>> fields;
        while (true) {
            auto token = context.Stream.Next();
            if (IsOp(token, '>')) break;
            if (!IsOp(token, '(')) co_return Error(token, "expected struct field type");
            auto name = co_await ParseName(context);
            auto type = co_await ParseType(context);
            co_await Expect(context, ')');
            fields.emplace_back(std::move(name), std::move(type));
        }
        co_return std::make_shared<TStructType>(std::move(fields));
    }
    co_return TError(location, "unknown composite type: " + head);
}

TTypeTask ParseType(TParserContext& context) {
    auto token = context.Stream.Next();
    if (IsNil(token)) co_return TTypePtr{};
    if (token.Type == TToken::Identifier) {
        if (auto scalar = ScalarTypeByName(token.Name)) co_return scalar;
        co_return std::make_shared<TNamedType>(token.Name, nullptr);
    }
    if (IsOp(token, '<')) {
        co_return co_await ParseCompositeType(context, token.Location);
    }
    co_return Error(token, "expected type");
}

TListHandlerMap MakeDefaultHandlers() {
    return TListHandlerMap{
        {":", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto expr = co_await ParseExpr(ctx);
            auto type = co_await ParseType(ctx);
            expr = ApplyTypeAnnotation(std::move(expr), std::move(type));
            co_await Expect(ctx, ')');
            co_return expr;
        }},
        {"bitcast", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto expr = co_await ParseExpr(ctx);
            auto type = co_await ParseType(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TBitcastExpr>(loc, std::move(expr), std::move(type));
        }},
        {"retain", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TRetainExpr>(loc, std::move(value));
        }},
        {"borrow", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TBorrowExpr>(loc, std::move(value));
        }},
        {"move", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TMoveExpr>(loc, std::move(value));
        }},
        {"own", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto dash = co_await ParseName(ctx);
            if (dash != "-") {
                co_return TError(loc, "expected '-' in own-literal");
            }
            auto suffix = co_await ParseName(ctx);
            if (suffix != "literal") {
                co_return TError(loc, "unknown core form: own-" + suffix);
            }
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TOwnLiteralExpr>(loc, std::move(value));
        }},
        {"destroy", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto value = co_await ParseExpr(ctx);
            TExprPtr aux;
            auto token = ctx.Stream.Next();
            if (!IsOp(token, ')')) {
                ctx.Stream.Unget(token);
                aux = co_await ParseExpr(ctx);
                co_await Expect(ctx, ')');
            }
            co_return std::make_shared<TDestroyExpr>(
                loc,
                std::move(value),
                std::move(aux));
        }},
        {"replace", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto target = co_await ParseExpr(ctx);
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TReplaceExpr>(
                loc,
                std::move(target),
                std::move(value));
        }},
        {"cleanup", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto dash = co_await ParseName(ctx);
            if (dash != "-") {
                co_return TError(loc, "expected '-' in cleanup form");
            }
            auto suffix = co_await ParseName(ctx);
            if (suffix == "global") {
                co_return std::make_shared<TGlobalCleanupExpr>(
                    loc,
                    co_await ParseExprsUntil(ctx, ')'));
            }
            if (suffix != "exit") {
                co_return TError(loc, "unknown core form: cleanup-" + suffix);
            }

            co_await Expect(ctx, '(');
            auto kindName = co_await ParseName(ctx);
            ECleanupExitKind kind;
            TExprPtr value;
            if (kindName == "return") {
                kind = ECleanupExitKind::Return;
                auto token = ctx.Stream.Next();
                if (!IsOp(token, ')')) {
                    ctx.Stream.Unget(token);
                    value = co_await ParseExpr(ctx);
                    co_await Expect(ctx, ')');
                }
            } else if (kindName == "break") {
                kind = ECleanupExitKind::Break;
                co_await Expect(ctx, ')');
            } else if (kindName == "continue") {
                kind = ECleanupExitKind::Continue;
                co_await Expect(ctx, ')');
            } else {
                co_return TError(loc, "unknown cleanup exit kind: " + kindName);
            }
            co_return std::make_shared<TCleanupExitExpr>(
                loc,
                kind,
                std::move(value),
                co_await ParseExprsUntil(ctx, ')'));
        }},
        {"=", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto name = co_await ParseName(ctx);
            auto token = ctx.Stream.Next();
            if (IsOp(token, '[')) {
                ctx.Stream.Unget(token);
                auto indices = co_await ParseIndexVector(ctx);
                auto value = co_await ParseExpr(ctx);
                co_await Expect(ctx, ')');
                co_return std::make_shared<TArrayAssignExpr>(loc, std::move(name), std::move(indices), std::move(value));
            }
            ctx.Stream.Unget(token);
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TAssignExpr>(loc, std::move(name), std::move(value));
        }},
        {"block", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            ++ctx.BlockDepth;
            auto exprs = co_await ParseExprsUntil(ctx, ')');
            --ctx.BlockDepth;
            // validate order: null(pragma)... -> TUseExpr... -> TTypeDeclStmt... -> real expr...
            auto isTypeDecl = [](const TExprPtr& e) { return e && TMaybeNode<TTypeDeclStmt>(e); };
            auto isUseExpr  = [](const TExprPtr& e) { return e && TMaybeNode<TUseExpr>(e); };
            auto firstNonNull = std::find_if(exprs.begin(), exprs.end(), [](const TExprPtr& e) { return !!e; });
            auto badNull = std::find_if(firstNonNull, exprs.end(), [](const TExprPtr& e) { return !e; });
            if (badNull != exprs.end()) {
                auto goodPragmas = std::count_if(exprs.begin(), firstNonNull, [](const TExprPtr& e) { return !e; });
                auto badLoc = goodPragmas < (ptrdiff_t)ctx.Pragmas.size() ? ctx.Pragmas[goodPragmas].Location : loc;
                co_return TError(badLoc, "pragma допускается только в начале первого блока");
            }
            auto firstNonUse      = std::find_if(firstNonNull, exprs.end(), [&](const TExprPtr& e) { return !isUseExpr(e); });
            auto badUse           = std::find_if(firstNonUse, exprs.end(), isUseExpr);
            if (badUse != exprs.end()) {
                co_return TError((*badUse)->Location, "use допускается только в начале первого блока, после прагм");
            }
            auto firstNonTypeDecl = std::find_if(firstNonUse, exprs.end(), [&](const TExprPtr& e) { return !isTypeDecl(e); });
            auto badTypeDecl      = std::find_if(firstNonTypeDecl, exprs.end(), isTypeDecl);
            if (badTypeDecl != exprs.end()) {
                co_return TError((*badTypeDecl)->Location, "объявление типа допускается только в начале первого блока, после прагм и use");
            }
            exprs.erase(std::remove_if(exprs.begin(), exprs.end(), [](const TExprPtr& e) { return !e; }), exprs.end());
            co_return std::make_shared<TBlockExpr>(loc, std::move(exprs));
        }},
        {"vars", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            co_return std::make_shared<TVarsBlockExpr>(loc, co_await ParseExprsUntil(ctx, ')'));
        }},
        {"input", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            co_return std::make_shared<TInputExpr>(loc, co_await ParseExprsUntil(ctx, ')'));
        }},
        {"cond", [](TParserContext&, TLocation loc) -> TAstTask {
            co_return TError(loc, "форма (cond ...) устарела, используйте (if ...)");
        }},
        {"if", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto cond = co_await ParseExpr(ctx);
            auto thenBranch = co_await ParseExpr(ctx);
            TExprPtr elseBranch;
            auto token = ctx.Stream.Next();
            if (!IsOp(token, ')')) {
                ctx.Stream.Unget(token);
                elseBranch = co_await ParseExpr(ctx);
                co_await Expect(ctx, ')');
            }
            co_return std::make_shared<TIfExpr>(loc, std::move(cond), std::move(thenBranch), std::move(elseBranch));
        }},
        {"while", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto cond = co_await ParseExpr(ctx);
            auto body = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TWhileStmtExpr>(loc, std::move(cond), std::move(body));
        }},
        {"repeat", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto body = co_await ParseExpr(ctx);
            auto cond = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TRepeatStmtExpr>(loc, std::move(body), std::move(cond));
        }},
        {"for", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto name = co_await ParseName(ctx);
            auto from = co_await ParseExpr(ctx);
            auto to = co_await ParseExpr(ctx);
            auto step = co_await ParseExpr(ctx);
            auto body = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TForStmtExpr>(loc, std::move(name), std::move(from), std::move(to), std::move(step), std::move(body));
        }},
        {"times", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto count = co_await ParseExpr(ctx);
            auto body = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TTimesStmtExpr>(loc, std::move(count), std::move(body));
        }},
        {"return", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto token = ctx.Stream.Next();
            if (IsOp(token, ')')) {
                co_return std::make_shared<TReturnExpr>(loc, nullptr);
            }
            ctx.Stream.Unget(token);
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TReturnExpr>(loc, std::move(value));
        }},
        {"var", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto name = co_await ParseName(ctx);
            auto peek = ctx.Stream.Next();
            if (peek.Type == TToken::Identifier && peek.Name == "=") {
                // (var name = expr) - type inferred from init expression
                auto init = co_await ParseExpr(ctx);
                co_await Expect(ctx, ')');
                auto var = std::make_shared<TVarStmt>(loc, std::move(name), nullptr);
                var->Init = std::move(init);
                co_return var;
            }
            ctx.Stream.Unget(peek);
            auto type = co_await ParseType(ctx);
            auto bounds = co_await ParseBounds(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TVarStmt>(loc, std::move(name), std::move(type), std::move(bounds));
        }},
        {"fun", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            // (fun Name [optional generic params] ((var a) (var b)) -> return type (block body))
            auto name = co_await ParseName(ctx);
            auto genericParams = co_await ParseGenericParams(ctx);
            auto params = co_await ParseParams(ctx);

            // optional return type introduced by '->'
            TTypePtr returnType = std::make_shared<TVoidType>();
            auto tok = ctx.Stream.Next();
            if (tok.Type == TToken::Identifier && tok.Name == "->") {
                returnType = co_await ParseType(ctx);
            } else {
                ctx.Stream.Unget(tok);
            }

            // optional (attrs ...) block
            TFunAttrs attrs;
            std::optional<std::string> operatorName;
            std::optional<std::string> externSymbol;
            std::optional<std::string> literalSuffix;
            bool used = false;
            bool cacheable = false;
            bool inlineFunction = false;
            bool noInlineFunction = false;
            tok = ctx.Stream.Next();
            if (IsOp(tok, '(')) {
                auto peek = ctx.Stream.Next();
                if (peek.Type == TToken::Identifier && peek.Name == "attrs") {
                    while (true) {
                        auto attrTok = ctx.Stream.Next();
                        if (IsOp(attrTok, ')')) { break; }
                        if (IsOp(attrTok, '(')) {
                            auto attrName = co_await ParseName(ctx);
                            auto attrExpr = co_await ParseExpr(ctx);
                            co_await Expect(ctx, ')');
                            if (attrName == "expect_after") {
                                attrs.After = std::move(attrExpr);
                            } else if (attrName == "expect_before") {
                                attrs.Before = std::move(attrExpr);
                            } else if (attrName == "operator") {
                                auto str = TMaybeNode<TStringLiteralExpr>(attrExpr);
                                if (!str) {
                                    co_return Error(attrTok, "expected string value for (operator ...) attribute");
                                }
                                operatorName = str.Cast()->Value;
                            } else if (attrName == "literal") {
                                auto str = TMaybeNode<TStringLiteralExpr>(attrExpr);
                                if (!str) {
                                    co_return Error(attrTok, "expected string value for (literal ...) attribute");
                                }
                                literalSuffix = str.Cast()->Value;
                            } else if (attrName == "extern") {
                                // (extern <symbol>): external function bound to <symbol>.
                                if (auto id = TMaybeNode<TIdentExpr>(attrExpr)) {
                                    externSymbol = id.Cast()->Name;
                                } else if (auto str = TMaybeNode<TStringLiteralExpr>(attrExpr)) {
                                    externSymbol = str.Cast()->Value;
                                } else {
                                    co_return Error(attrTok, "expected symbol name in (extern ...) attribute");
                                }
                            }
                        } else if (attrTok.Type == TToken::Identifier) {
                            // `print`: the function is its argument type's printer
                            // (a unary "print" operator, dispatched by `вывод`).
                            // `extern`: external function bound to its own name.
                            // Unknown simple attributes are ignored for forward compatibility.
                            if (attrTok.Name == "print") {
                                operatorName = "print";
                            } else if (attrTok.Name == "extern") {
                                // bare extern: symbol is the function's own name
                                externSymbol = name;
                            } else if (attrTok.Name == "used") {
                                used = true;
                            } else if (attrTok.Name == "cacheable") {
                                cacheable = true;
                            } else if (attrTok.Name == "inline") {
                                inlineFunction = true;
                            } else if (attrTok.Name == "noinline") {
                                noInlineFunction = true;
                            }
                        } else {
                            co_return Error(attrTok, "expected function attribute");
                        }
                    }
                } else {
                    ctx.Stream.Unget(peek);
                    ctx.Stream.Unget(tok);
                }
            } else {
                ctx.Stream.Unget(tok);
            }

            auto bodyExpr = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            auto body = TMaybeNode<TBlockExpr>(bodyExpr).Cast();
            if (!body) {
                co_return TError(bodyExpr ? bodyExpr->Location : loc, "function body must be block");
            }
            auto funDecl = std::make_shared<TFunDecl>(loc, std::move(name), std::move(genericParams), std::move(params), std::move(body), std::move(returnType));
            funDecl->LastAssert = std::move(attrs.After);
            funDecl->OperatorName = std::move(operatorName);
            funDecl->LiteralSuffix = std::move(literalSuffix);
            funDecl->Used = used;
            funDecl->Cacheable = cacheable;
            funDecl->Inline = inlineFunction;
            funDecl->NoInline = noInlineFunction;
            if (externSymbol) {
                funDecl->MangledName = std::move(*externSymbol);
            }
            std::vector<TTypePtr> paramTypes;
            for (const auto& param : funDecl->Params) {
                paramTypes.push_back(param->Type);
            }
            funDecl->Type = std::make_shared<TFunctionType>(std::move(paramTypes), funDecl->RetType);
            co_return funDecl;
        }},
        {"call", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto callee = co_await ParseExpr(ctx);
            auto args = co_await ParseExprsUntil(ctx, ')');
            co_return std::make_shared<TCallExpr>(loc, std::move(callee), std::move(args));
        }},
        {"await", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto operand = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TAwaitExpr>(loc, std::move(operand));
        }},
        {"output", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto args = co_await ParseOutputArgs(ctx);
            co_return std::make_shared<TOutputExpr>(loc, std::move(args));
        }},
        {"cast", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto operand = co_await ParseExpr(ctx);
            auto type = co_await ParseType(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TCastExpr>(loc, std::move(operand), std::move(type));
        }},
        {"index", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto collection = co_await ParseExpr(ctx);
            auto token = ctx.Stream.Next();
            if (IsOp(token, '[')) {
                ctx.Stream.Unget(token);
                auto indices = co_await ParseIndexVector(ctx);
                co_await Expect(ctx, ')');
                co_return std::make_shared<TMultiIndexExpr>(loc, std::move(collection), std::move(indices));
            }
            ctx.Stream.Unget(token);
            auto index = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TIndexExpr>(loc, std::move(collection), std::move(index));
        }},
        {"slice", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto collection = co_await ParseExpr(ctx);
            auto bounds = co_await ParseIndexVector(ctx);
            if (bounds.empty() || bounds.size() > 2) {
                co_return TError(loc, "slice expects one or two bounds");
            }
            co_await Expect(ctx, ')');
            co_return std::make_shared<TSliceExpr>(loc, std::move(collection), std::move(bounds[0]), bounds.size() == 2 ? std::move(bounds[1]) : nullptr);
        }},
        {"use", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto moduleName = co_await ParseNameLike(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TUseExpr>(loc, std::move(moduleName));
        }},
        {"pragma", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            if (ctx.BlockDepth != 1) {
                co_return TError(loc, "pragma допускается только в первом блоке");
            }
            auto group = co_await ParseName(ctx);
            std::vector<std::string> values;
            while (true) {
                auto vt = ctx.Stream.Next();
                if (IsOp(vt, ')')) break;
                if (vt.IsEof()) co_return Error(vt, "unexpected eof in pragma");
                if (vt.Type != TToken::Identifier) co_return Error(vt, "expected pragma value");
                values.push_back(vt.Name);
            }
            ctx.Pragmas.push_back(TPragma{std::move(group), std::move(values), loc});
            co_return TExprPtr{};
        }},
        {"type", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            if (ctx.BlockDepth != 1) {
                co_return TError(loc, "объявление типа допускается только в первом блоке");
            }
            auto name = co_await ParseName(ctx);
            auto genericParams = co_await ParseGenericParams(ctx);
            auto underlying = co_await ParseType(ctx);
            co_await Expect(ctx, ')');
            auto namedType = std::make_shared<TNamedType>(std::move(name), std::move(underlying));
            co_return std::make_shared<TTypeDeclStmt>(loc, std::move(namedType), std::move(genericParams));
        }},
        {"assert", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto expr = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TAssertStmt>(loc, std::move(expr));
        }},
        {"field", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto object = co_await ParseExpr(ctx);
            auto fieldName = co_await ParseName(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TFieldAccessExpr>(loc, std::move(object), std::move(fieldName));
        }},
        {"struct", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            co_await Expect(ctx, '(');
            std::vector<std::pair<std::string, TTypePtr>> fieldTypes;
            std::vector<TExprPtr> fields;
            std::vector<std::string> fieldNames;
            while (true) {
                auto token = ctx.Stream.Next();
                if (IsOp(token, ')')) break;
                if (!IsOp(token, '(')) co_return Error(token, "expected struct field");
                auto name = co_await ParseName(ctx);
                fieldNames.push_back(name);
                fields.push_back(co_await ParseExpr(ctx));
                fieldTypes.emplace_back(std::move(name), nullptr);
                co_await Expect(ctx, ')');
            }
            co_await Expect(ctx, ')');
            co_return std::make_shared<TStructConstructExpr>(
                loc,
                std::make_shared<TStructType>(std::move(fieldTypes)),
                std::move(fields),
                std::move(fieldNames));
        }},
        {"field_assign", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto object = co_await ParseExpr(ctx);
            auto fieldName = co_await ParseName(ctx);
            auto value = co_await ParseExpr(ctx);
            co_await Expect(ctx, ')');
            co_return std::make_shared<TFieldAssignExpr>(loc, std::move(object), std::move(fieldName), std::move(value));
        }},
        {"vec", [](TParserContext& ctx, TLocation loc) -> TAstTask {
            auto elements = co_await ParseExprsUntil(ctx, ')');
            co_return std::make_shared<TVectorExpr>(loc, std::move(elements));
        }},
    };
}

struct TParseHandleImpl : NQumir::NAst::NCore::IParseHandle {
    TParserContext& Ctx;
    explicit TParseHandleImpl(TParserContext& ctx) : Ctx(ctx) {}

    TAstTask Expr() override {
        co_return co_await ParseExpr(Ctx);
    }
    TExpectedTask<TTypePtr, TError, TLocation> Type() override {
        co_return co_await ParseType(Ctx);
    }
    TToken Next() override {
        return Ctx.Stream.Next();
    }
    void Unget(TToken tok) override {
        Ctx.Stream.Unget(std::move(tok));
    }
    TExpectedTask<std::monostate, TError, TLocation> Take(char op) override {
        co_return co_await Expect(Ctx, op);
    }
};

} // namespace

std::expected<TExprPtr, TError> TParser::Parse(TTokenStream& baseStream) {
    TWrappedTokenStream stream(baseStream, 4);
    TParserContext context{stream, MakeDefaultHandlers()};
    for (auto& [key, handler] : NodeParsers) {
        context.Handlers[key] = [h = handler](TParserContext& ctx, TLocation loc) -> TAstTask {
            TParseHandleImpl handle{ctx};
            co_return co_await h(handle, loc);
        };
    }
    auto result = ParseExpr(context).result();
    if (!result) {
        return std::unexpected(result.error());
    }
    auto token = stream.Next();
    if (!token.IsEof()) {
        return std::unexpected(Error(token, "expected eof"));
    }
    Pragmas = std::move(context.Pragmas);
    return result;
}

std::expected<TTypePtr, TError> ParseType(ITokenStream& baseStream) {
    TWrappedTokenStream stream(baseStream, 4);
    TParserContext context{stream};
    return ParseType(context).result();
}

} // namespace NCore
} // namespace NAst
} // namespace NQumir
