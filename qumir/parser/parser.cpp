#include "parser.h"
#include "qumir/parser/type.h"

#include <qumir/error.h>
#include <qumir/optional.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/modules/module.h>
#include <qumir/frontend/source_module_loader.h>

#include <set>
#include <iostream>

namespace NQumir {
namespace NAst {

namespace {

struct TParserContext {
    TWrappedTokenStream& Stream;
    IModuleManager* ModuleManager;
    ILexerContext* LexerContext;
    NFrontend::TSourceModuleLoader* Loader;
    int LoopDepth = 0;
    // Whether the enclosing `алг` returns void; used by `выход` outside a
    // loop to decide between `(return)` and `(return знач)`. Defaults to
    // true (void) for `выход` encountered outside any function.
    bool CurrentFunctionReturnsVoid = true;

    TParserContext(TWrappedTokenStream& stream, IModuleManager* mm, ILexerContext* lc,
        NFrontend::TSourceModuleLoader* loader = nullptr)
        : Stream(stream)
        , ModuleManager(mm)
        , LexerContext(lc)
        , Loader(loader)
    { }
};

inline TExprPtr list(TLocation loc, std::vector<TExprPtr> elements) {
    return std::make_shared<TBlockExpr>(std::move(loc), std::move(elements));
}

inline TExprPtr unary(TLocation loc, TOperator op, TExprPtr operand) {
    return std::make_shared<TUnaryExpr>(std::move(loc), op, std::move(operand));
}

inline TExprPtr binary(TLocation loc, TOperator op, TExprPtr left, TExprPtr right) {
    return std::make_shared<TBinaryExpr>(std::move(loc), op, std::move(left), std::move(right));
}

inline TExprPtr num(TLocation loc, int64_t v) {
    return std::make_shared<TNumberExpr>(std::move(loc), v);
}

inline TExprPtr num(TLocation loc, double v) {
    return std::make_shared<TNumberExpr>(std::move(loc), v);
}

inline TExprPtr num(TLocation loc, bool v) {
    return std::make_shared<TCastExpr>(loc,
        std::make_shared<TNumberExpr>(loc, v),
        std::make_shared<TBoolType>());
}

inline TExprPtr sym(TLocation loc, int64_t v) {
    return std::make_shared<TCastExpr>(loc,
        std::make_shared<TNumberExpr>(loc, v),
        std::make_shared<TSymbolType>());
}

inline TExprPtr ident(TLocation loc, std::string n) {
    return std::make_shared<TIdentExpr>(loc, std::move(n));
}

inline bool isEof(const TToken& tok) {
    return tok.Type == TToken::Operator && (EOperator)tok.Value.i64 == EOperator::Eof;
}

// A block is left open: `got` stands where its closing keyword was expected.
TError unclosedBlock(const TToken& got, const std::string& expected, TLocation opener) {
    std::string found = isEof(got) ? "программа закончилась" : "встретилось '" + got.RawValue + "'";
    return TError(got.Location, "не хватает " + expected + ": " + found
        + ", а блок, начатый в строке " + std::to_string(opener.Line) + ", не закрыт");
}

inline bool isOp(const TToken& tok, EOperator op) {
    return tok.Type == TToken::Operator && (EOperator)tok.Value.i64 == op;
}

inline bool isKeyword(const TToken& tok, EKeyword kw) {
    return tok.Type == TToken::Keyword && (EKeyword)tok.Value.i64 == kw;
}

using TAstTask = TExpectedTask<TExprPtr, TError, TLocation>;
TAstTask stmt(TParserContext& context);
// `opener` is where the block starts, for the message about a block left open.
TAstTask stmt_list(TParserContext& context, std::set<EKeyword> terminators, TLocation opener, std::vector<TExprPtr> stmts = {});
TExpectedTask<std::vector<TExprPtr>, TError, TLocation> var_decl_list(TParserContext& context, bool parseAttributes);
TAstTask expr(TParserContext& context);
TAstTask for_loop(TParserContext& context, TLocation location);
TAstTask for_times(TParserContext& context, TExprPtr countExpr, TLocation location);
TError unexpectedOperator(TWrappedTokenStream& stream);

void SkipEols(TWrappedTokenStream& stream) {
    while (true) {
        auto t = stream.Next();
        if (isEof(t)) break;
        if (isOp(t, EOperator::Eol)) {
            continue;
        }
        stream.Unget(t);
        break;
    }
}

/*
enum class EOperator : uint8_t {
    // Arithmetic operators
    Pow, // **
    Mul, // *
    FDiv, // /
    Plus, // +
    Minus, // -
    // Comparison operators
    Eq, // =
    Neq, // <>
    Lt, // <
    Gt, // >
    Leq, // <=
    Geq, // >=
    // Other operators
    Assign, // :=
    Comma, // ,
    LParen, // (
    RParen, // )
    LSqBr, // [
    RSqBr, // ]
    Colon, // :
    // Special operators
    Eol, // \n
    // Logical operators
    And,
    Or,
    Not,
    // Integer division and modulus
    Div,
    Mod,
};
*/
inline TOperator MakeOperator(EOperator op) {
    switch (op) {
        case EOperator::Pow: return TOperator("**");
        case EOperator::Mul: return '*';
        case EOperator::FDiv: return '/';
        case EOperator::Plus: return '+';
        case EOperator::Minus: return '-';

        case EOperator::Eq: return TOperator("==");
        case EOperator::Neq: return TOperator("!=");
        case EOperator::Lt: return TOperator("<");
        case EOperator::Gt: return TOperator(">");
        case EOperator::Leq: return TOperator("<=");
        case EOperator::Geq: return TOperator(">=");

        case EOperator::And: return TOperator("&&");
        case EOperator::Or: return TOperator("||");
        case EOperator::Not: return TOperator("!");

        default:
            throw std::runtime_error("internal error: unknown operator");
    }
}

inline bool IsTypeKeyword(EKeyword kw) {
    return kw == EKeyword::Int
        || kw == EKeyword::Float
        || kw == EKeyword::Bool
        || kw == EKeyword::String
        || kw == EKeyword::Char

        || kw == EKeyword::IntTab
        || kw == EKeyword::FloatTab
        || kw == EKeyword::BoolTab
        || kw == EKeyword::StringTab
        || kw == EKeyword::CharTab

        || kw == EKeyword::Array
        || kw == EKeyword::File
        || kw == EKeyword::InArg // for function parameter declarations
        || kw == EKeyword::OutArg // for function parameter declarations
        || kw == EKeyword::InOutArg // for function parameter declarations

        || kw == EKeyword::NamedType; // for built-in types imported from modules
        ;
}

/*
StmtList ::= Stmt*
*/
TAstTask stmt_list(TParserContext& context, std::set<EKeyword> terminators, TLocation opener, std::vector<TExprPtr> stmts) {
    auto& stream = context.Stream;
    auto loc = stream.GetLocation();
    while (true) {
        // Skip standalone EOLs between statements
        bool skipped = false;
        while (true) {
            auto t = stream.Next();
            if (isEof(t)) break;
            if (isOp(t, EOperator::Eol)) {
                skipped = true;
                continue;
            }
            stream.Unget(t);
            break;
        }
        (void)skipped;

        // Stop conditions: EOF or block terminator 'кон' (End)
        auto t = stream.Next();
        if (isEof(t)) {
            break;
        }
        if (t.Type == TToken::Keyword && terminators.contains(static_cast<EKeyword>(t.Value.i64))) {
            stream.Unget(t);
            break;
        }
        // A closing keyword of an outer block means this block was left open:
        // `кон' right after a loop body is a missing `кц', not a stray `кон'.
        // A function body is the outermost block, there a stray keyword is just stray.
        if (!terminators.empty() && !terminators.contains(EKeyword::End) && t.Type == TToken::Keyword) {
            auto kw = static_cast<EKeyword>(t.Value.i64);
            if (kw == EKeyword::End || kw == EKeyword::EndIf || kw == EKeyword::LoopEnd
                || kw == EKeyword::LoopEndWhen || kw == EKeyword::Else)
            {
                co_return unclosedBlock(t, terminators.contains(EKeyword::LoopEnd) ? "'кц'" : "'все'", opener);
            }
        }
        stream.Unget(t);
        auto s = co_await stmt(context);
        // Flatten if stmt returned a Block of multiple declarations
        if (auto blk = TMaybeNode<TVarsBlockExpr>(s)) {
            auto be = blk.Cast();
            for (auto& ch : be->Vars) {
                stmts.push_back(ch);
            }
        } else {
            stmts.push_back(std::move(s));
        }
    }
    co_return list(loc, std::move(stmts));
}

TExpectedTask<std::pair<TExprPtr, TExprPtr>, TError, TLocation> array_bounds(TParserContext& context)
{
    auto& stream = context.Stream;
    // left bound
    auto leftExpr = co_await expr(context);
    auto colonTok = stream.Next();
    if (!isOp(colonTok, EOperator::Colon)) {
        co_return TError(colonTok.Location, "ожидается ':' между границами массива");
    }
    // right bound
    auto rightExpr = co_await expr(context);
    co_return std::make_pair(std::move(leftExpr), std::move(rightExpr));
}

/*
VarDecl ::= TypeKw Name (',' Name)* EOL
TypeKw  ::= 'цел' | 'вещ' | 'лог' | 'лит' | 'таб'
Name    ::= NamePart (' ' NamePart)*
NamePart::= Identifier | Keyword
*/
TExpectedTask<std::shared_ptr<TVarStmt>, TError, TLocation> var_decl(TParserContext& context, TTypePtr scalarType, bool isArray, bool isPointer, bool isReference) {
    auto& stream = context.Stream;
    auto nameTok = stream.Next();
    if (nameTok.Type != TToken::Identifier) {
        // Если здесь пошёл новый тип - пусть внешняя логика разрулит (мы вернём ошибку)
        co_return TError(nameTok.Location, "ожидался идентификатор переменной");
    }

    std::string name = nameTok.Name;
    TTypePtr varType = scalarType;
    std::vector<std::pair<TExprPtr, TExprPtr>> bounds;

    if (isArray) {
        // ожидаем границы массива: '[' expr ':' expr (',' expr ':' expr )* ']'
        auto t = stream.Next();
        if (!isOp(t, EOperator::LSqBr)) {
            co_return TError(t.Location, "для табличного типа ожидаются границы массива после имени: '['");
        }
        while (true) {
            bounds.push_back(co_await array_bounds(context));
            // ']'
            auto rsbTok = stream.Next();
            if (!isOp(rsbTok, EOperator::RSqBr) && !isOp(rsbTok, EOperator::Comma)) {
                co_return TError(rsbTok.Location, "ожидалась закрывающая ']' для границ массива");
            }
            if (isOp(rsbTok, EOperator::RSqBr)) {
                break;
            }
            if (isOp(rsbTok, EOperator::Comma)) {
                // multi-dimensional array index
                continue;
            }
            co_return TError(rsbTok.Location, "ожидается ',' или ']' после границ массива");
        }

        // TODO: limit arity of multi-dimensional arrays, smth like: arity <= MAX_ARRAY_DIMENSIONS = 6
        // TODO: create hidden variables for bounds expressions, i.e.
        // __name_dim0_left, __name_dim0_right, __name_dim1_left, __name_dim1_right, ...
        // [i,    j,    k]
        //  ^     ^     ^
        //  dim0  dim1  dim2
        // flat index = i * (dim1_size * dim2_size) + j * dim2_size + k, where
        // dimN_size = right_bound_N - left_bound_N + 1
        // precompute sizes during variable initialization?
        varType = std::make_shared<TArrayType>(scalarType, bounds.size());
    } else {
        // скобки после имени запрещены для скалярного типа
        auto t = stream.Next();
        if (isOp(t, EOperator::LSqBr)) {
            co_return TError(t.Location, "границы массива не допускаются для скалярного типа");
        }
        stream.Unget(t);
    }

    if (isPointer) {
        varType = std::make_shared<TPointerType>(varType);
    } else if (isReference) {
        varType = std::make_shared<TReferenceType>(varType);
    }

    auto var = std::make_shared<TVarStmt>(nameTok.Location, name, varType, bounds);
    co_return var;
}

TTypePtr getScalarType(EKeyword kw, bool& isArray, const std::string& typeName, IModuleManager* nameResolver) {
    isArray = false;
    switch (kw) {
        case EKeyword::Int:
            return std::make_shared<TIntegerType>();
        case EKeyword::Float:
            return std::make_shared<TFloatType>();
        case EKeyword::Bool:
            return std::make_shared<TBoolType>();
        case EKeyword::String:
            return std::make_shared<TStringType>();
        case EKeyword::Char:
            return std::make_shared<TSymbolType>();

        case EKeyword::IntTab:
            isArray = true;
            return std::make_shared<TIntegerType>();
        case EKeyword::FloatTab:
            isArray = true;
            return std::make_shared<TFloatType>();
        case EKeyword::BoolTab:
            isArray = true;
            return std::make_shared<TBoolType>();
        case EKeyword::StringTab:
            isArray = true;
            return std::make_shared<TStringType>();
        case EKeyword::CharTab:
            isArray = true;
            return std::make_shared<TSymbolType>();

        case EKeyword::File:
            return std::make_shared<TNamedType>("file", nameResolver->LookupType("file"));
        case EKeyword::NamedType:
            return std::make_shared<TNamedType>(typeName, nameResolver->LookupType(typeName));
        default:
            return nullptr;
    }
}

TExpectedTask<std::vector<TExprPtr>, TError, TLocation> var_decl_list(TParserContext& context, bool parseAttributes) {
    auto& stream = context.Stream;
    auto* nameResolver = context.ModuleManager;
    auto first = stream.Next();

    bool isPointer = false;
    bool isReference = false;
    bool isMutable = true;
    bool isReadable = true;
    if (parseAttributes) {
        isMutable = false;
        if (isKeyword(first, EKeyword::InArg)) {
            // skip
            first = stream.Next();
            if (isKeyword(first, EKeyword::OutArg)) {
                isReference = true;
                isMutable = true;
                isReadable = true;
                first = stream.Next();
            }
        } else if (isKeyword(first, EKeyword::OutArg)) {
            isReference = true;
            isMutable = true; // mutability of underlying data is implied by being an out-parameter
            isReadable = false;
            first = stream.Next();
        } else if (isKeyword(first, EKeyword::InOutArg)) {
            isReference = true;
            isMutable = true;
            isReadable = true;
            first = stream.Next();
        }
    }

    if (! (first.Type == TToken::Keyword && IsTypeKeyword(static_cast<EKeyword>(first.Value.i64)))) {
        co_return TError(first.Location, "ожидается тип переменной");
    }

    // Parse one or more names
    std::vector<TExprPtr> decls;
    bool isArray = false;

    TTypePtr scalarType = getScalarType(static_cast<EKeyword>(first.Value.i64), isArray, first.Name, nameResolver);
    if (!scalarType) {
        co_return TError(first.Location, "неизвестный тип переменной");
    }
    scalarType->Mutable = isMutable;
    scalarType->Readable = isReadable;
    // опциональный признак массива после базового типа: 'таб'
    if (isArray == false) {
        auto t = stream.Next();
        if (isKeyword(t, EKeyword::Array)) {
            isArray = true;
        } else {
            stream.Unget(t);
        }
    }
    // TODO: fix attributes
    if (isArray) {
        // Arrays are already handled as non-pointer types
        isPointer = false;
        isReference = false;
    }
    while (true) {
        auto decl = co_await var_decl(context, scalarType, isArray, isPointer, isReference);
        decls.push_back(decl);

        auto t = stream.Next();
        if (isEof(t)) {
            break;
        }
        if (t.Type == TToken::Operator) {
            auto op = static_cast<EOperator>(t.Value.i64);
            if (op == EOperator::Eq) {
                // цел a = 5
                // read scalar value
                auto initExpr = co_await expr(context);
                // insert assignment to variable
                auto name = decl->Name;
                auto assignStmt = std::make_shared<TAssignExpr>(t.Location,
                    name,
                    initExpr);
                decls.push_back(assignStmt);

                t = stream.Next();
                if (t.Type == TToken::Operator) {
                    op = static_cast<EOperator>(t.Value.i64);
                } else {
                    co_return TError(t.Location, "ожидалась ',' или перевод строки после имени переменной");
                }
            }
            if (op == EOperator::Comma) {
                // после запятой может идти либо следующее имя, либо новый базовый тип -
                // в последнем случае завершаем текущий стейтмент
                auto look = stream.Next();
                if (look.Type == TToken::Keyword && IsTypeKeyword(static_cast<EKeyword>(look.Value.i64))) {
                    // новый стейтмент начинается с базового типа
                    stream.Unget(look);
                    break;
                }
                stream.Unget(look);
                continue;
            }
            if (op == EOperator::Eol) {
                // end of declaration statement
                break;
            }
            if (op == EOperator::RParen) {
                // Likely end of parameter list in function declaration
                stream.Unget(t);
                break;
            }
            // Unexpected operator
            co_return TError(t.Location, "ожидалась ',' или перевод строки после имени переменной");
        } else {
            // Something else after name - error for now
            co_return TError(t.Location, "недопустимый токен после имени переменной");
        }
    }

    co_return decls;
}


/*
FunDecl ::= 'алг' EOL? 'нач' EOL? StmtList 'кон'                   // main: имя пропущено; EOL после 'алг' допустим
         | 'алг' Ident OptSignature EOL? 'нач' EOL? StmtList 'кон' // именованная; EOL между 'алг' и именем не допускается
OptSignature ::= '(' ParamList? ')'
ParamList ::= Param (',' Param)*
Param ::= 'рез' TypeSpec IdentList | 'арг' TypeSpec IdentList
TypeSpec ::= TypeKw ArrayMark?
TypeKw ::= 'цел' | 'вещ' | 'лог' | 'лит'
ArrayMark ::= 'таб'  [массивные параметры, если используются]
IdentList ::= Ident (',' Ident)*
*/
TAstTask fun_decl(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;
    auto next = stream.Next();
    TTypePtr returnType = std::make_shared<TVoidType>();
    std::vector<std::shared_ptr<TVarStmt>> args;
    std::string name = "<main>";

    if (next.Type == TToken::Keyword && IsTypeKeyword(static_cast<EKeyword>(next.Value.i64))) {
        // function return type
        bool isArray = false;
        returnType = getScalarType(static_cast<EKeyword>(next.Value.i64), isArray, next.Name, mm);
        if (isArray) {
            co_return TError(next.Location, "функция не может возвращать табличный тип");
        }
        next = stream.Next();
    }

    if (next.Type == TToken::Identifier) {
        name = next.Name;

        // parse signature
        next =  stream.Next();
        if (isOp(next, EOperator::LParen)) {
            // '(' ... ')'
            while (true) {
                next = stream.Next();
                if (next.Type == TToken::Keyword && IsTypeKeyword(static_cast<EKeyword>(next.Value.i64))) {
                    stream.Unget(next);
                    auto tmpArgs = co_await var_decl_list(context, true);
                    for (auto& arg : tmpArgs) {
                        if (auto varArg = TMaybeNode<TVarStmt>(arg)) {
                            args.push_back(varArg.Cast());
                        } else {
                            co_return TError(arg->Location, "ожидался параметр функции");
                        }
                    }
                } else {
                    break;
                }
            }

            if (! isOp(next, EOperator::RParen)) {
                co_return TError(next.Location, "ожидалась закрывающая скобка ')' после списка параметров функции");
            }

            next = stream.Next();
        }
        // else: no signature (empty parameter list)
    }

    // 1. optional дано EKeyword::AssertBefore
    // 2. optional надо EKeyword::AssertAfter
    // 3. required нач EKeyword::Begin
    std::shared_ptr<TAssertStmt> assertBefore = nullptr;
    std::shared_ptr<TAssertStmt> assertAfter = nullptr;
    if (isOp(next, EOperator::Eol)) {
        // optional EOL after 'алг' or after return type
        next = stream.Next();
    }
    if (isKeyword(next, EKeyword::AssertBefore)) {
        const auto keywordLocation = next.Location;
        next = stream.Next();
        stream.Unget(next);
        // Only a comment after the keyword: empty condition, nothing to assert.
        if (!isOp(next, EOperator::Eol) && !isEof(next)) {
            assertBefore = std::make_shared<TAssertStmt>(keywordLocation, co_await expr(context));
        }
        next = stream.Next();
    }
    if (isOp(next, EOperator::Eol)) {
        // optional EOL after 'алг' or after return type
        next = stream.Next();
    }
    if (isKeyword(next, EKeyword::AssertAfter)) {
        const auto keywordLocation = next.Location;
        next = stream.Next();
        stream.Unget(next);
        // Only a comment after the keyword: empty condition, nothing to assert.
        if (!isOp(next, EOperator::Eol) && !isEof(next)) {
            assertAfter = std::make_shared<TAssertStmt>(keywordLocation, co_await expr(context));
        }
        next = stream.Next();
    }
    if (isOp(next, EOperator::Eol)) {
        // optional EOL after 'алг' or after return type
        next = stream.Next();
    }

    if (!isKeyword(next, EKeyword::Begin)) {
        co_return TError(next.Location, "ожидалось 'нач' после заголовка функции");
    }

    std::vector<TExprPtr> bodyStmts;
    if (assertBefore) {
        bodyStmts.push_back(assertBefore);
    }

    if (!TMaybeType<TVoidType>(returnType)) {
        bodyStmts.push_back(std::make_shared<TVarStmt>(next.Location, "знач", returnType));
    }

    auto savedLoopDepth = context.LoopDepth;
    auto savedReturnsVoid = context.CurrentFunctionReturnsVoid;
    context.LoopDepth = 0;
    context.CurrentFunctionReturnsVoid = TMaybeType<TVoidType>(returnType);

    auto body = co_await stmt_list(context, { EKeyword::End }, location, std::move(bodyStmts));

    context.LoopDepth = savedLoopDepth;
    context.CurrentFunctionReturnsVoid = savedReturnsVoid;

    next = stream.Next();
    if (!isKeyword(next, EKeyword::End)) {
        co_return unclosedBlock(next, "'кон'", location);
    }

    if (auto maybeBlock = TMaybeNode<TBlockExpr>(body)) {
        auto block = maybeBlock.Cast();

        // Append a trailing `(return знач)` / `(return)` so every function
        // body has an explicit return, populating retLocal even when control
        // falls off the end of the body.
        if (TMaybeType<TVoidType>(returnType)) {
            block->Stmts.push_back(std::make_shared<TReturnExpr>(next.Location, nullptr));
        } else {
            block->Stmts.push_back(std::make_shared<TReturnExpr>(next.Location, ident(next.Location, "знач")));
        }

        auto funDecl = std::make_shared<TFunDecl>(location,
            name, std::vector<TGenericParam>{}, std::move(args),
            std::move(maybeBlock.Cast()),
            returnType);

        funDecl->LastAssert = assertAfter;

        std::vector<TTypePtr> paramTypes;
        for (auto& a : funDecl->Params) {
            paramTypes.push_back(a->Type);
        }
        funDecl->Type = std::make_shared<TFunctionType>(std::move(paramTypes), returnType);

        co_return funDecl;
    } else {
        co_return TError(body->Location, "ожидался блок операторов в теле функции");
    }
}

/*
    ForLoop ::= identifier 'от' expr 'до' expr ('шаг' expr)?
*/
TAstTask for_loop(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;


    auto varTok = stream.Next();
    if (varTok.Type != TToken::Identifier) {
        co_return TError(varTok.Location, "ожидался идентификатор переменной в операторе 'для'");
    }

    auto fromTok = stream.Next();
    if (!isKeyword(fromTok, EKeyword::From)) {
        co_return TError(fromTok.Location, "ожидалось 'от' в операторе 'для'");
    }

    auto fromExpr = co_await expr(context);

    auto toTok = stream.Next();
    if (!isKeyword(toTok, EKeyword::To)) {
        co_return TError(toTok.Location, "ожидалось 'до' в операторе 'для'");
    }

    auto toExpr = co_await expr(context);

    TExprPtr stepExpr = nullptr;
    auto stepTok = stream.Next();
    if (isKeyword(stepTok, EKeyword::Step)) {
        stepExpr = co_await expr(context);
    } else {
        stepExpr = num(stream.GetLocation(), (int64_t)1); // default step = 1
        stream.Unget(stepTok);
    }

    context.LoopDepth++;
    auto body = co_await stmt_list(context, { EKeyword::LoopEnd }, location);
    context.LoopDepth--;

    auto endTok = stream.Next();
    if (!isKeyword(endTok, EKeyword::LoopEnd)) {
        co_return unclosedBlock(endTok, "'кц'", location);
    }

    co_return std::make_shared<TForStmtExpr>(
        location,
        varTok.Name,
        fromExpr,
        toExpr,
        stepExpr,
        body);
}

/*
    нц expr раз
        body
    кц

    sugar for: for i from 1 to expr
*/
TAstTask for_times(TParserContext& context, TExprPtr countExpr, TLocation location) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;

    context.LoopDepth++;
    auto body = co_await stmt_list(context, { EKeyword::LoopEnd }, location);
    context.LoopDepth--;

    auto endTok = stream.Next();
    if (!isKeyword(endTok, EKeyword::LoopEnd)) {
        co_return unclosedBlock(endTok, "'кц'", location);
    }

    co_return std::make_shared<TTimesStmtExpr>(location, std::move(countExpr), body);
}

/*
  нц пока условие
    body
  кц
 */
TAstTask while_loop(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;

    auto cond = co_await expr(context);

    context.LoopDepth++;
    auto body = co_await stmt_list(context, { EKeyword::LoopEnd }, location);
    context.LoopDepth--;

    auto endTok = stream.Next();
    if (!isKeyword(endTok, EKeyword::LoopEnd)) {
        co_return unclosedBlock(endTok, "'кц'", location);
    }

    co_return std::make_shared<TWhileStmtExpr>(location, cond, body);
}

/*
  нц
    body
  кц_при условие
  or
  нц
    body
  кц при условие
*/
TAstTask repeat_until_loop(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;

    context.LoopDepth++;
    auto body = co_await stmt_list(context, { EKeyword::LoopEndWhen, EKeyword::LoopEnd }, location);
    context.LoopDepth--;

    auto untilTok = stream.Next();
    // кц при or кц_при
    if (!isKeyword(untilTok, EKeyword::LoopEndWhen) && !isKeyword(untilTok, EKeyword::LoopEnd)) {
        co_return unclosedBlock(untilTok, "'кц' или 'кц_при'", location);
    }
    // one more token if 'кц'
    TExprPtr condExpr;
    bool invert = true;
    if (static_cast<EKeyword>(untilTok.Value.i64) == EKeyword::LoopEnd) {
        // look ahead: if 'при' then parse condition, else infinite loop
        auto look = stream.Next();
        if (isKeyword(look, EKeyword::Case)) {
            // parse condition normally
            condExpr = co_await expr(context);
        } else {
            // plain 'кц' => infinite loop, push back look token for next parser stage
            stream.Unget(look);
            condExpr = num(location, true);
            invert = false;
        }
    } else { // 'кц_при'
        condExpr = co_await expr(context);
    }

    if (invert) {
        condExpr = unary(location, TOperator("!"), condExpr);
    }

    co_return std::make_shared<TRepeatStmtExpr>(location, body, condExpr);
}

/*
выбор
  при условие 1 : серия 1
  при условие 2 : серия 2
  ...
  при условие n : серия n
  иначе серия n+1
все

or

выбор
  при условие 1 : серия 1
  при условие 2 : серия 2
  ...
  при условие n : серия n
все

*/
TAstTask switch_expr(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;

    SkipEols(stream);
    // collect cases
    std::vector<std::pair<TExprPtr, TExprPtr>> cases;
    TExprPtr elseBranch = nullptr;
    while (true) {
        auto caseTok = stream.Next();
        if (isKeyword(caseTok, EKeyword::EndIf)) {
            // end of switch
            break;
        }
        if (isKeyword(caseTok, EKeyword::Else)) {
            elseBranch = co_await stmt_list(context, { EKeyword::EndIf }, location);

            auto endTok = stream.Next();
            if (!isKeyword(endTok, EKeyword::EndIf)) {
                co_return unclosedBlock(endTok, "'все'", location);
            }

            break;
        }
        if (!isKeyword(caseTok, EKeyword::Case)) {
            co_return TError(caseTok.Location, "ожидалось 'при' или 'иначе' или 'все' в операторе 'выбор'");
        }

        auto cond = co_await expr(context);
        auto colonTok = stream.Next();
        if (!isOp(colonTok, EOperator::Colon)) {
            co_return TError(colonTok.Location, "ожидался ':' после условия в операторе 'выбор'");
        }

        auto body = co_await stmt_list(context, { EKeyword::Case, EKeyword::Else, EKeyword::EndIf }, location);
        cases.emplace_back(std::move(cond), std::move(body));
    }

    // need to build if-then-else chain from cases
    std::shared_ptr<TIfExpr> rootIf = nullptr;
    std::shared_ptr<TIfExpr> lastIf = nullptr;
    for (auto& c : cases) {
        auto newIf = std::make_shared<TIfExpr>(location, std::move(c.first), std::move(c.second), nullptr);
        if (!rootIf) {
            rootIf = newIf;
            lastIf = newIf;
        } else {
            lastIf->Else = newIf;
            lastIf = newIf;
        }
    }
    if (lastIf) {
        lastIf->Else = elseBranch;
    }
    if (rootIf) {
        co_return rootIf;
    } else {
        co_return TError(location, "ожидался хотя бы один 'при' в операторе 'выбор'");
    }
}

/*
If ::= 'если' Expr EOL* 'то' EOL* StmtList OptElse 'все'
OptElse ::= EOL* 'иначе' EOL* StmtList | eps
// Примечания:
// - Expr, StmtList не раскрываются здесь (используются как чёрные ящики).
// - EOL* означает, что между элементами могут быть пустые строки/переводы строк.
// - Примеры допускают как серию на той же строке после 'то'/'иначе', так и на следующих строках.
*/
TAstTask if_expr(TParserContext& context, TLocation location) {
    auto& stream = context.Stream;
    auto cond = co_await expr(context);
    SkipEols(stream);

    auto thenTok = stream.Next();
    if (!isKeyword(thenTok, EKeyword::Then)) {
        co_return TError(thenTok.Location, "ожидалось 'то' после условия в операторе 'если'");
    }

    auto thenBranch = co_await stmt_list(context, { EKeyword::Else, EKeyword::EndIf }, location);

    SkipEols(stream);
    auto elseTok = stream.Next();
    if (isKeyword(elseTok, EKeyword::EndIf)) {
        // if without else
        co_return std::make_shared<TIfExpr>(location, cond, thenBranch, nullptr);
    }

    if (!isKeyword(elseTok, EKeyword::Else)) {
        co_return unclosedBlock(elseTok, "'все'", location);
    }

    auto elseBranch = co_await stmt_list(context, { EKeyword::EndIf }, location);

    auto endTok = stream.Next();
    if (!isKeyword(endTok, EKeyword::EndIf)) {
        co_return unclosedBlock(endTok, "'все'", location);
    }

    co_return std::make_shared<TIfExpr>(location, cond, thenBranch, elseBranch);
}

// Parse optional argument list after '(' then ')' or after '[' then ']'
TExpectedTask<std::vector<TExprPtr>, TError, TLocation> parse_arg_list_opt(TParserContext& context, EOperator rParen = EOperator::RParen) {
    auto& stream = context.Stream;
    std::vector<TExprPtr> args;
    auto tok = stream.Next();
    if (isOp(tok, rParen)) {
        co_return args; // empty
    }
    stream.Unget(tok);
    // first expr
    auto e = co_await expr(context);
    args.push_back(std::move(e));
    while (true) {
        auto t = stream.Next();
        if (isOp(t, rParen)) {
            break;
        }
        if (isOp(t, EOperator::Comma)) {
            auto e2 = co_await expr(context);
            args.push_back(std::move(e2));
            continue;
        }
        stream.Unget(t);
        if (rParen == EOperator::RParen) {
            co_return TError(t.Location, "ожидается ',' или ')'");
        } else {
            co_return TError(t.Location, "ожидается ',' или ']'");
        }
    }
    co_return args;
}

// Parse input/output operator list - i.e. arguments for 'ввод'/'вывод' separated by commas, without surrounding parentheses
// ввод a:width:prec, b, c
template<typename TIoArg>
TExpectedTask<std::vector<TIoArg>, TError, TLocation> parse_io_arg_list_opt(TParserContext& context) {
    auto& stream = context.Stream;
    std::vector<TIoArg> args;

    // In classic KuMir a statement ends not only at a line break: a keyword
    // that closes a block ends it too. That is what makes the single-line form
    // `если a > 0 то вывод "да" все' legal - textbooks and the sample programs
    // shipped with KuMir itself are written that way.
    //
    // The set matches the reference implementation (kumir2, lexer.cpp,
    // popLexemsUntilSemicolonOrBlockClose: `;', иначе, кц, при, все) plus `кон',
    // which closes a block in this grammar exactly the same way; no program
    // valid in the original changes meaning because of it.
    //
    // Only keywords that really close a block belong here - "any keyword" would
    // not do, since `нс' in `вывод a, нс' is a keyword as well.
    auto endsBlock = [](const TToken& t) {
        if (t.Type != TToken::Keyword) {
            return false;
        }
        switch (static_cast<EKeyword>(t.Value.i64)) {
            case EKeyword::EndIf:       // все
            case EKeyword::Else:        // иначе
            case EKeyword::Case:        // при
            case EKeyword::LoopEnd:     // кц
            case EKeyword::LoopEndWhen: // кц_при
            case EKeyword::End:         // кон
                return true;
            default:
                return false;
        }
    };

    auto tok = stream.Next();
    if (isOp(tok, EOperator::Eol)) {
        co_return args; // empty
    }
    stream.Unget(tok);
    while (true) {
        TExprPtr width = nullptr;
        TExprPtr prec = nullptr;
        auto e = co_await expr(context);
        auto t = stream.Next();
        if constexpr(std::is_same_v<TIoArg, TOutputArg>) {
            if (t.Type == TToken::Operator) {
                if ((EOperator)t.Value.i64 == EOperator::Colon) {
                    width = co_await expr(context);
                    t = stream.Next();
                }
            }
            if (t.Type == TToken::Operator) {
                if ((EOperator)t.Value.i64 == EOperator::Colon) {
                    prec = co_await expr(context);
                    t = stream.Next();
                }
            }
        }
        const bool comma = t.Type == TToken::Operator && (EOperator)t.Value.i64 == EOperator::Comma;
        const bool eol = t.Type == TToken::Operator && (EOperator)t.Value.i64 == EOperator::Eol;
        const bool blockEnd = endsBlock(t);
        if (!comma && !eol && !blockEnd) {
            stream.Unget(t);
            co_return TError(t.Location, "ожидается ',' или конец строки в списке аргументов ввода/вывода");
        }
        if (blockEnd) {
            stream.Unget(t); // the keyword closes the block; stmt_list reads it
        }
        if constexpr(std::is_same_v<TIoArg, TOutputArg>) {
            args.push_back(TIoArg{ std::move(e), std::move(width), std::move(prec) });
        } else {
            args.push_back(std::move(e));
        }
        if (!comma) {
            break;
        }
    }
    co_return args;
}

/*
Factor/Primary ::= Number | Ident | ( Expr ) | fun
*/
TAstTask factor(TParserContext& context) {
    auto& stream = context.Stream;
    auto token = stream.Next();
    auto withSuffix = [&](TExprPtr lit) -> TExprPtr {
        auto peek = stream.Next();
        if (peek.Type == TToken::Identifier && context.LexerContext) {
            const auto& suffixes = context.LexerContext->GetLiteralSuffixes();
            auto it = suffixes.find(peek.Name);
            if (it != suffixes.end()) {
                auto callee = std::make_shared<TIdentExpr>(peek.Location, it->second);
                return std::make_shared<TCallExpr>(peek.Location, std::move(callee), std::vector<TExprPtr>{lit});
            }
        }
        stream.Unget(peek);
        return lit;
    };

    if (token.Type == TToken::Integer) {
        co_return withSuffix(num(token.Location, token.Value.i64));
    } else if (token.Type == TToken::Float) {
        co_return withSuffix(num(token.Location, token.Value.f64));
    } else if (token.Type == TToken::Char) {
        co_return sym(token.Location, token.Value.i64);
    } else if (token.Type == TToken::Keyword && static_cast<EKeyword>(token.Value.i64) == EKeyword::NewLine) {
        co_return std::make_shared<TStringLiteralExpr>(token.Location, "\n");
    } else if (token.Type == TToken::String) {
        co_return std::make_shared<TStringLiteralExpr>(token.Location, token.Name);
    } else if (token.Type == TToken::Identifier) {
        co_return ident(token.Location, token.Name);
    } else if (token.Type == TToken::Keyword && static_cast<EKeyword>(token.Value.i64) == EKeyword::Return) {
        co_return ident(token.Location, "знач");
    } else if (token.Type == TToken::Operator) {
        if ((EOperator)token.Value.i64 == EOperator::LParen) {
            auto ret = co_await expr(context);
            token = stream.Next();
            if (!isOp(token, EOperator::RParen)) {
                co_return TError(token.Location, std::string("ожидается ')'"));
            }
            co_return ret;
        } else {
            co_return unexpectedOperator(stream);
        }
    } else if (token.Type == TToken::Keyword && ((EKeyword)token.Value.i64 == EKeyword::True || (EKeyword)token.Value.i64 == EKeyword::False)) {
        bool v = (EKeyword)token.Value.i64 == EKeyword::True;
        co_return num(token.Location, v);
    } else {
        co_return TError(token.Location, std::string("ожидалось число или '('"));
    }
}

// call_expr ::=  factor ( '(' arg_list_opt ')' )*
//              | factor ( '[' expr ':' expr ']' )* // <- string slice
//              | factor ( '[' expr ']' )* // <- array index or string index
//              | factor ( '[' expr (',' expr)? ']' )* // <- multi-dimensional array index
TAstTask call_expr(TParserContext& context) {
    auto& stream = context.Stream;
    auto base = co_await factor(context);
    TToken tok;
    while (!isEof(tok = stream.Next())) {
        if (isOp(tok, EOperator::LParen)) {
            // Разрешаем вызов функции только если базовое выражение - идентификатор
            if (!TMaybeNode<TIdentExpr>(base)) {
                co_return TError(tok.Location, "ожидалось имя функции перед '('");
            }
            auto args = co_await parse_arg_list_opt(context);
            base = std::make_shared<TCallExpr>(tok.Location, std::move(base), std::move(args));
            continue;
        }
        if (isOp(tok, EOperator::LSqBr)) {
            auto indexExpr = co_await expr(context);
            auto rbrOrColonTok = stream.Next();
            if (rbrOrColonTok.Type != TToken::Operator) {
                co_return TError(rbrOrColonTok.Location, "ожидается ']' или ':' после индекса массива");
            }
            if ((EOperator)rbrOrColonTok.Value.i64 == EOperator::Colon) {
                // array slice
                auto endIndexExpr = co_await expr(context);
                auto rsbTok = stream.Next();
                if (!isOp(rsbTok, EOperator::RSqBr)) {
                    co_return TError(rsbTok.Location, "ожидается ']' после среза массива");
                }
                base = std::make_shared<TSliceExpr>(tok.Location, std::move(base), std::move(indexExpr), std::move(endIndexExpr));
                continue;
            } else if ((EOperator)rbrOrColonTok.Value.i64 == EOperator::Comma) {
                // multi-dimensional array index
                auto restArgs = co_await parse_arg_list_opt(context, EOperator::RSqBr);
                std::vector<TExprPtr> allIndices; allIndices.reserve(1 + restArgs.size());
                allIndices.push_back(std::move(indexExpr));
                allIndices.insert(allIndices.end(),
                    std::make_move_iterator(restArgs.begin()),
                    std::make_move_iterator(restArgs.end()));
                base = std::make_shared<TMultiIndexExpr>(tok.Location, std::move(base), std::move(allIndices));
                continue;
            } else if ((EOperator)rbrOrColonTok.Value.i64 == EOperator::RSqBr) {
                // single index
                // done
            } else {
                co_return TError(rbrOrColonTok.Location, "ожидается ']' или ':' после индекса массива");
            }
            base = std::make_shared<TIndexExpr>(tok.Location, std::move(base), std::move(indexExpr));
            continue;
        }
        stream.Unget(tok);
        break;
    }
    co_return base;
}

// Forward declaration for mutual recursion with power_expr
TAstTask unary_expr(TParserContext& context);
// Forward declaration to place logical NOT between equality and AND
TAstTask not_expr(TParserContext& context);

// power_expr ::= call_expr ( '**' unary_expr )?
// Right-associative: a ** b ** c == a ** (b ** c)
TAstTask power_expr(TParserContext& context) {
    auto& stream = context.Stream;
    auto base = co_await call_expr(context);
    auto tok = stream.Next();
    if (isOp(tok, EOperator::Pow)) {
        // RHS allows unary sign, e.g., 2 ** -3
        auto rhs = co_await unary_expr(context);
        co_return binary(tok.Location, MakeOperator(EOperator::Pow), std::move(base), std::move(rhs));
    } else {
        stream.Unget(tok);
    }
    co_return base;
}

// unary ::= call_expr | '+' unary | '-' unary
TAstTask unary_expr(TParserContext& context) {
    auto& stream = context.Stream;
    auto tok = stream.Next();
    if (tok.Type == TToken::Operator && ((EOperator)tok.Value.i64 == EOperator::Plus
        || (EOperator)tok.Value.i64 == EOperator::Minus))
    {
        auto inner = co_await unary_expr(context);
        if ((EOperator)tok.Value.i64 == EOperator::Plus) {
            co_return unary(tok.Location, MakeOperator(EOperator::Plus), std::move(inner));
        } else if ((EOperator)tok.Value.i64 == EOperator::Minus) {
            co_return unary(tok.Location, MakeOperator(EOperator::Minus), std::move(inner));
        }
    }
    stream.Unget(tok);
    // Exponentiation has higher precedence than unary: -2**2 == -(2**2)
    co_return co_await power_expr(context);
}

template<typename Func, typename... TOps>
TAstTask binary_op_helper(TParserContext& context, Func prev, TOps... ops) {
    auto& stream = context.Stream;
    auto ret = co_await prev(context);
    TToken token;
    while (!isEof(token = stream.Next())) {
        if (token.Type == TToken::Operator
            && ((token.Value.i64 == (int64_t)ops) || ...))
        {
            auto next = co_await prev(context);
            ret = binary(token.Location, MakeOperator((EOperator)token.Value.i64), std::move(ret), std::move(next));
        } else {
            stream.Unget(token);
            break;
        }
    }
    co_return ret;
}

/*
MulExpr ::= Factor
         | MulExpr*Factor
         | MulExpr/Factor
*/
TAstTask mul_expr(TParserContext& context) {
    co_return co_await binary_op_helper(context, unary_expr
        , EOperator::Mul, EOperator::FDiv);
}

/*
AddExpr ::= MulExpr
         | AddExpr+MulExpr
         | AddExpr-MulExpr
*/
TAstTask add_expr(TParserContext& context) {
    co_return co_await binary_op_helper(context, mul_expr, EOperator::Plus, EOperator::Minus);
}

/*
Comparison / equality chain level (replaces former rel_expr + eq_expr):
Grammar sugar:
  AddExpr (CompOp AddExpr)+
Builds: (a op1 b) && (b op2 c) && ...
If zero operators -> returns the sole AddExpr.
If one operator  -> returns single binary comparison node (backwards compatible).
Mixed operators allowed (e.g. a < b == c <= d).
Precedence: sits where equality used to sit (above AND, below + / -).
*/
TAstTask comp_chain_expr(TParserContext& context) {
    auto& stream = context.Stream;
    auto first = co_await add_expr(context);
    std::vector<EOperator> ops;
    std::vector<TExprPtr> operands; operands.push_back(first);
    while (true) {
        auto tok = stream.Next();
        if (isEof(tok) || tok.Type != TToken::Operator) {
            if (!isEof(tok)) stream.Unget(tok);
            break;
        }
        auto op = static_cast<EOperator>(tok.Value.i64);
        bool end = false;
        switch (op) {
            case EOperator::Eq: case EOperator::Neq:
            case EOperator::Lt: case EOperator::Gt:
            case EOperator::Leq: case EOperator::Geq:
                break;
            default:
                stream.Unget(tok);
                end = true;
                break;
        }
        if (end) break;
        auto nextOperand = co_await add_expr(context);
        ops.push_back(op);
        operands.push_back(nextOperand);
    }
    if (ops.size() <= 1) {
        if (ops.empty()) {
            co_return operands.front();
        }
        auto loc = operands[0]->Location;
        co_return binary(loc, MakeOperator(ops[0]), operands[0], operands[1]);
    }
    std::vector<TExprPtr> pairwise; pairwise.reserve(ops.size());
    for (size_t i = 0; i < ops.size(); ++i) {
        auto loc = operands[i]->Location;
        pairwise.push_back(binary(loc, MakeOperator(ops[i]), operands[i], operands[i+1]));
    }
    auto result = pairwise[0];
    for (size_t i = 1; i < pairwise.size(); ++i) {
        auto loc = pairwise[i]->Location;
        result = binary(loc, MakeOperator(EOperator::And), result, pairwise[i]);
    }
    co_return result;
}

/* NotExpr ::= '!' NotExpr | CompChain */
TAstTask not_expr(TParserContext& context) {
    auto& stream = context.Stream;
    auto tok = stream.Next();
    if (tok.Type == TToken::Operator && (EOperator)tok.Value.i64 == EOperator::Not) {
        auto inner = co_await not_expr(context);
        co_return unary(tok.Location, MakeOperator(EOperator::Not), std::move(inner));
    }
    stream.Unget(tok);
    co_return co_await comp_chain_expr(context);
}

/* AndExpr ::= NotExpr ( "&&" NotExpr )* */
TAstTask and_expr(TParserContext& context) {
    co_return co_await binary_op_helper(context, not_expr, EOperator::And);
}

/* OrExpr ::= AndExpr ( "||" OrExpr )* */
TAstTask or_expr(TParserContext& context) {
    co_return co_await binary_op_helper(context, and_expr, EOperator::Or);
}

TAstTask expr(TParserContext& context) {
    co_return co_await or_expr(context);
}


/*
Generate a context-aware error message for unexpected tokens.

Analyzes the token window to provide helpful hints about common mistakes.

Handled cases:
  1. Function call without parentheses: `sqrt 16` -> suggests `sqrt(16)`
  2. Number at line start: `10` -> suggests missing variable for assignment
  3. Parenthesized expr at line start: `(x+5)` -> suggests missing variable
  4. Keyword after identifier: `foo нач` -> suggests missing `алг`
  5. String at line start: `"hello"` -> suggests `вывод "hello"`
  6. Minus at line start: `-5` -> suggests missing variable for assignment
  7. Plus at line start: `+5` -> suggests missing variable for assignment
  8. Assignment without LHS: `:= 10` -> suggests missing variable name
  9. Closing keywords without opening:
     - `кон` without `алг/нач`
     - `все` without `если/выбор/цикл`
     - `кц` without `нц`
     - `иначе` without `если/выбор`
     - `то` without `если`
 10. Two identifiers in sequence (across lines)
 11. Closing paren at line start: `)` without `(`
 12. Closing bracket at line start: `]` without `[`
*/
TError unexpected(TWrappedTokenStream& stream) {
    auto& window = stream.GetWindow();
    if (window.empty()) {
        return TError(stream.GetLocation(), "неожиданный конец файла");
    }

    auto current = window.back();
    auto location = current.Location;

    // Analyze context: look at previous tokens in the window
    // window = [tok1, tok2, ..., tokN] where tokN is the current (failing) token

    std::string hint;
    std::string baseMsg = "не ожидалось `" + current.RawValue + "'";

    // Helper: check if token is a number (integer or float)
    auto isNumber = [](const TToken& t) {
        return t.Type == TToken::Integer || t.Type == TToken::Float;
    };

    // Helper: check if previous token is EOL or we're at the start
    auto afterEol = [&window]() {
        return window.size() < 2 ||
            (window[window.size() - 2].Type == TToken::Operator &&
             (EOperator)window[window.size() - 2].Value.i64 == EOperator::Eol);
    };

    // Case 1: identifier followed by number -> possibly function call without parentheses
    // foo 10 -> foo(10)
    // foo 0.5 -> foo(0.5)
    if (window.size() >= 2) {
        auto& prev = window[window.size() - 2];
        if (prev.Type == TToken::Identifier && isNumber(current)) {
            hint = "возможно, вы имели в виду вызов функции: " + prev.Name + "(" + current.RawValue + ")";
        }
    }

    // Case 2: number at line start -> meaningless expression
    if (hint.empty() && isNumber(current) && afterEol()) {
        hint = "число не может быть началом оператора; возможно, вы забыли имя переменной для присваивания";
    }

    // Case 3: opening parenthesis at line start
    if (hint.empty() && current.Type == TToken::Operator && (EOperator)current.Value.i64 == EOperator::LParen) {
        if (afterEol()) {
            hint = "выражение в скобках не может быть началом оператора; возможно, вы забыли имя переменной для присваивания";
        }
    }

    // Case 4: keyword after identifier
    if (hint.empty() && window.size() >= 2) {
        auto& prev = window[window.size() - 2];
        if (prev.Type == TToken::Identifier && current.Type == TToken::Keyword) {
            auto kw = static_cast<EKeyword>(current.Value.i64);
            // identifier followed by 'нач' -> missing 'алг'?
            if (kw == EKeyword::Begin) {
                hint = "возможно, пропущено объявление алгоритма 'алг' перед '" + prev.Name + "'";
            }
        }
    }

    // Case 5: string at line start
    if (hint.empty() && current.Type == TToken::String && afterEol()) {
        hint = "строка не может быть началом оператора; возможно, вы имели в виду 'вывод \"" + current.Name + "\"'";
    }

    // Case 6: minus at line start (possibly negative number without assignment)
    if (hint.empty() && current.Type == TToken::Operator &&
        (EOperator)current.Value.i64 == EOperator::Minus && afterEol()) {
        hint = "выражение не может быть началом оператора; возможно, вы забыли имя переменной для присваивания";
    }

    // Case 7: plus at line start
    if (hint.empty() && current.Type == TToken::Operator &&
        (EOperator)current.Value.i64 == EOperator::Plus && afterEol()) {
        hint = "выражение не может быть началом оператора; возможно, вы забыли имя переменной для присваивания";
    }

    // Case 8: assignment operator without LHS (:= at line start)
    if (hint.empty() && current.Type == TToken::Operator &&
        (EOperator)current.Value.i64 == EOperator::Assign && afterEol()) {
        hint = "оператор присваивания ':=' требует имя переменной слева";
    }

    // Case 9: closing keywords without corresponding opening keywords
    if (hint.empty() && current.Type == TToken::Keyword && afterEol()) {
        auto kw = static_cast<EKeyword>(current.Value.i64);
        if (kw == EKeyword::End) {
            hint = "'кон' без соответствующего 'алг'/'нач'";
        } else if (kw == EKeyword::EndIf) {
            hint = "'все' без соответствующего 'если', 'выбор' или 'цикл'";
        } else if (kw == EKeyword::LoopEnd) {
            hint = "'кц' без соответствующего 'нц'";
        } else if (kw == EKeyword::Else) {
            hint = "'иначе' без соответствующего 'если' или 'выбор'";
        } else if (kw == EKeyword::Then) {
            hint = "'то' без соответствующего 'если'";
        }
    }

    // Case 10: identifier after identifier and EOL (possibly missing operator)
    // Usually this is a multi-word identifier case, but if we got here something is wrong
    if (hint.empty() && window.size() >= 3 && current.Type == TToken::Identifier) {
        auto& prev = window[window.size() - 2];
        auto& prevprev = window[window.size() - 3];
        // If there was EOL, then identifier, and now another identifier
        if (prev.Type == TToken::Operator && (EOperator)prev.Value.i64 == EOperator::Eol &&
            prevprev.Type == TToken::Identifier) {
            hint = "два идентификатора подряд; возможно, пропущен оператор между '" +
                   prevprev.Name + "' и '" + current.Name + "'";
        }
    }

    // Case 11: closing parenthesis at line start
    if (hint.empty() && current.Type == TToken::Operator &&
        (EOperator)current.Value.i64 == EOperator::RParen && afterEol()) {
        hint = "закрывающая скобка ')' без соответствующей открывающей '('";
    }

    // Case 12: closing bracket at line start
    if (hint.empty() && current.Type == TToken::Operator &&
        (EOperator)current.Value.i64 == EOperator::RSqBr && afterEol()) {
        hint = "закрывающая скобка ']' без соответствующей открывающей '['";
    }

    // Build final message
    std::string expected = "ожидались: объявление переменной, присваивание, ввод/вывод, условие, цикл, выбор, объявление функции";

    if (!hint.empty()) {
        return TError(location, baseMsg + "; " + hint);
    }
    return TError(location, baseMsg + "; " + expected);
}

/*
Generate error message when unexpected token appears after expression.

Handles cases like:
   i := j + 10 + 5   mod(30)
                     ^-- unexpected identifier after expression

Analyzes window to suggest adding an operator between expressions.
*/
TError unexpectedTokenAfterExpr(TWrappedTokenStream& stream) {
    auto& window = stream.GetWindow();
    if (window.empty()) {
        return TError(stream.GetLocation(), "неожиданный конец файла после выражения");
    }

    auto current = window.back();
    auto location = current.Location;

    std::string hint;
    std::string baseMsg = "после выражения не ожидалось `" + current.RawValue + "'";

    // Helper: check if token is a number (integer or float)
    auto isNumber = [](const TToken& t) {
        return t.Type == TToken::Integer || t.Type == TToken::Float;
    };

    // Look for pattern: ... number/ident identifier/number
    // This suggests missing operator between two parts of expression
    if (window.size() >= 2) {
        auto& prev = window[window.size() - 2];

        // Case: number followed by identifier (like "5 mod")
        if (isNumber(prev) && current.Type == TToken::Identifier) {
            hint = "возможно, пропущен оператор между `" + prev.RawValue + "' и `" + current.RawValue +
                   "'; попробуйте добавить +, -, *, / или другой оператор";
        }
        // Case: identifier followed by identifier (like "x mod")
        else if (prev.Type == TToken::Identifier && current.Type == TToken::Identifier) {
            hint = "возможно, пропущен оператор между `" + prev.Name + "' и `" + current.Name +
                   "'; попробуйте добавить +, -, *, / или другой оператор";
        }
        // Case: closing paren followed by identifier (like ") mod")
        else if (isOp(prev, EOperator::RParen) && current.Type == TToken::Identifier) {
            hint = "возможно, пропущен оператор перед `" + current.RawValue +
                   "'; попробуйте добавить +, -, *, / или другой оператор";
        }
        // Case: closing bracket followed by identifier (like "] mod")
        else if (isOp(prev, EOperator::RSqBr) && current.Type == TToken::Identifier) {
            hint = "возможно, пропущен оператор перед `" + current.RawValue +
                   "'; попробуйте добавить +, -, *, / или другой оператор";
        }
        // Case: number followed by number (like "5 3")
        else if (isNumber(prev) && isNumber(current)) {
            hint = "два числа подряд; возможно, пропущен оператор между `" + prev.RawValue +
                   "' и `" + current.RawValue + "'";
        }
        // Case: number followed by opening paren (like "5 (")
        else if (isNumber(prev) && isOp(current, EOperator::LParen)) {
            hint = "возможно, пропущен оператор перед `('; попробуйте добавить *, +, - или другой оператор";
        }
        // Case: identifier followed by number (like "x 5")
        else if (prev.Type == TToken::Identifier && isNumber(current)) {
            // This might be function call without parens, but in this context it's after expr
            hint = "возможно, пропущен оператор между `" + prev.Name + "' и `" + current.RawValue +
                   "'; попробуйте добавить +, -, *, / или другой оператор";
        }
    }

    if (hint.empty()) {
        hint = "ожидался конец строки или оператор (+, -, *, /, и, или, ...)";
    }

    return TError(location, baseMsg + "; " + hint);
}

/*
Generate error message for unexpected operator in expression context (factor).

Called when parsing factor() and encountered an operator instead of
number, identifier, or opening parenthesis.

Handled cases:
  1. `)` - closing paren without opening
  2. `]` - closing bracket without opening
  3. `:=` - assignment in expression context
  4. `,` - comma in unexpected place
  5. `:` - colon outside array bounds or output format
  6. `*`, `/` - binary operator at start of expression
  7. `=`, `<>`, `<`, `>`, `<=`, `>=` - comparison at start
  8. `и`, `или` - logical operator at start
*/
TError unexpectedOperator(TWrappedTokenStream& stream) {
    auto& window = stream.GetWindow();
    if (window.empty()) {
        return TError(stream.GetLocation(), "неожиданный конец файла в выражении");
    }

    auto current = window.back();
    auto location = current.Location;
    auto op = static_cast<EOperator>(current.Value.i64);

    std::string hint = "неожиданный оператор `" + current.RawValue + "'";

    switch (op) {
        case EOperator::RParen:
            hint = "закрывающая скобка ')' без соответствующей открывающей '('";
            break;
        case EOperator::RSqBr:
            hint = "закрывающая скобка ']' без соответствующей открывающей '['";
            break;
        case EOperator::Assign:
            hint = "оператор присваивания ':=' не может использоваться внутри выражения";
            break;
        case EOperator::Comma:
            hint = "запятая ',' в неожиданном месте; возможно, лишняя запятая или пропущен аргумент";
            break;
        case EOperator::Colon:
            hint = "двоеточие ':' в неожиданном месте";
            break;
        case EOperator::Mul:
        case EOperator::FDiv:
            hint = "оператор `" + current.RawValue + "' требует операнд слева; возможно, пропущено число или переменная";
            break;
        case EOperator::Pow:
            hint = "оператор возведения в степень '**' требует операнд слева";
            break;
        case EOperator::Eq:
        case EOperator::Neq:
        case EOperator::Lt:
        case EOperator::Gt:
        case EOperator::Leq:
        case EOperator::Geq:
            hint = "оператор сравнения `" + current.RawValue + "' требует операнд слева";
            break;
        case EOperator::And:
            hint = "логический оператор 'и' требует операнд слева";
            break;
        case EOperator::Or:
            hint = "логический оператор 'или' требует операнд слева";
            break;
        case EOperator::Eol:
            hint = "неожиданный конец строки; возможно, выражение не завершено";
            break;
        case EOperator::Eof:
            hint = "неожиданный конец файла; возможно, выражение не завершено";
            break;
        default:
            hint = "в этом месте ожидалось число, переменная или '('";
            break;
    }

    return TError(location, hint);
}

/*
Stmt ::= VarDecl
    | Assign
    | Input
    | Output
    | If
    | Loop
    | Switch
    | Break
    | Continue
    | FunDecl
    | Use
*/
TAstTask stmt(TParserContext& context) {
    auto& stream = context.Stream;
    auto* mm = context.ModuleManager;

    // Variable declarations:
    //   (цел|вещ|лог|лит|таб) Name (',' Name)* EOL
    // Names may consist of identifiers and/or keywords (multi-word), e.g. "не готов", "если число"
    // If no matching statement type found yet, return error (to be extended later).

    auto first = stream.Next();

    if (isEof(first)) {
        co_return TError(first.Location, "ожидался стейтмент, но достигнут конец файла");
    }

    if (first.Type == TToken::Keyword && IsTypeKeyword(static_cast<EKeyword>(first.Value.i64))) {
        stream.Unget(first);
        auto decls = co_await var_decl_list(context, false);
        co_return std::make_shared<TVarsBlockExpr>(first.Location, decls);
    } else if (isKeyword(first, EKeyword::Alg)) {
        co_return co_await fun_decl(context, first.Location);
    } else if (isKeyword(first, EKeyword::If)) {
        co_return co_await if_expr(context, first.Location);
    } else if (isKeyword(first, EKeyword::Return)) {
        // skip ':='
        auto next = stream.Next();
        if (!isOp(next, EOperator::Assign)) {
            stream.Unget(next);
            co_return TError(next.Location, "ожидался ':=' после 'знач'");
        }
        auto value = co_await expr(context);
        co_return std::make_shared<TAssignExpr>(first.Location, "знач", value);
    } else if (isKeyword(first, EKeyword::LoopStart)) {
        auto next = stream.Next();
        if (isKeyword(next, EKeyword::For)) {
            co_return co_await for_loop(context, first.Location);
        } else if (isKeyword(next, EKeyword::While)) {
            co_return co_await while_loop(context, first.Location);
        } else if (isOp(next, EOperator::Eol)) {
            co_return co_await repeat_until_loop(context, first.Location);
        } else {
            stream.Unget(next);
            auto countExpr = co_await expr(context);
            auto timesTok = stream.Next();
            if (!isKeyword(timesTok, EKeyword::Times)) {
                co_return TError(timesTok.Location, "ожидалось 'раз' после выражения в операторе 'нц'");
            }

            co_return co_await for_times(context, std::move(countExpr), first.Location);
        }
    } else if (isKeyword(first, EKeyword::Switch)) {
        co_return co_await switch_expr(context, first.Location);
    } else if (isKeyword(first, EKeyword::Input)) {
        auto args = co_await parse_io_arg_list_opt<TExprPtr>(context);
        co_return std::make_shared<TInputExpr>(first.Location, std::move(args));
    } else if (isKeyword(first, EKeyword::Output)) {
        auto args = co_await parse_io_arg_list_opt<TOutputArg>(context);
        co_return std::make_shared<TOutputExpr>(first.Location, std::move(args));
    } else if (isKeyword(first, EKeyword::Break)) {
        if (context.LoopDepth > 0) {
            co_return std::make_shared<TBreakStmt>(first.Location);
        } else if (context.CurrentFunctionReturnsVoid) {
            // 'выход' outside any loop: early return from the enclosing алг.
            co_return std::make_shared<TReturnExpr>(first.Location, nullptr);
        } else {
            co_return std::make_shared<TReturnExpr>(first.Location, ident(first.Location, "знач"));
        }
    } else if (isKeyword(first, EKeyword::Continue)) {
        co_return std::make_shared<TContinueStmt>(first.Location);
    } else if (isKeyword(first, EKeyword::Assert)) {
        co_return std::make_shared<TAssertStmt>(first.Location, co_await expr(context));
    } else if (first.Type == TToken::Identifier) {
        auto next = stream.Next();
        if (isOp(next, EOperator::LSqBr)) {
            // Array element assignment: Ident '[' expr (',' expr)* ']' ':=' expr
            auto exprs = co_await parse_arg_list_opt(context, EOperator::RSqBr);
            auto assignTok = stream.Next();
            if (!isOp(assignTok, EOperator::Assign)) {
                co_return TError(assignTok.Location,
                    "после `" + first.Name + "[...]' ожидался `:=' для присваивания элементу массива, получено `" + assignTok.RawValue + "'");
            }
            auto rhs = co_await expr(context);
            auto next = stream.Next();
            if (!(next.Type == TToken::EType::Operator || next.Type == TToken::EType::Keyword)) {
                co_return unexpectedTokenAfterExpr(stream);
            }
            stream.Unget(next);
            co_return std::make_shared<TArrayAssignExpr>(first.Location, first.Name, std::move(exprs), rhs);
        } else if (isOp(next, EOperator::Assign)) {
            // Assignment statement
            auto rhs = co_await expr(context);
            auto next = stream.Next();
            if (!(next.Type == TToken::EType::Operator || next.Type == TToken::EType::Keyword)) {
                co_return unexpectedTokenAfterExpr(stream);
            }
            stream.Unget(next);
            co_return std::make_shared<TAssignExpr>(first.Location, first.Name, rhs);
        } else {
            // Important: restore tokens in reverse order of reading
            // so that the identifier comes before '(' again.
            stream.Unget(next);
            stream.Unget(first);
            auto rhs = co_await expr(context);
            auto next = stream.Next();
            if (!(next.Type == TToken::EType::Operator || next.Type == TToken::EType::Keyword)) {
                co_return unexpectedTokenAfterExpr(stream);
            }
            stream.Unget(next);
            co_return rhs;
        }
    } else if (isKeyword(first, EKeyword::Use)) {
        auto next = stream.Next();
        if (next.Type != TToken::Identifier) {
            co_return TError(next.Location, "ожидалось имя модуля после 'использовать'");
        }
        auto moduleName = next.Name;
        next = stream.Next();
        if (!isOp(next, EOperator::Eol) && !isEof(next)) {
            co_return TError(next.Location, "ожидается новая строка после имени модуля");
        }
        // A `.oz` source module is inlined during post-parse composition. Its
        // exported types must still be registered now so the lexer tokenizes
        // them as type names and `LookupType` resolves variable declarations
        // referencing them during parsing.
        bool sourceModule = context.Loader && context.Loader->Resolvable(moduleName);
        if (sourceModule) {
            auto loaded = context.Loader->Load(moduleName);
            if (!loaded) {
                co_return loaded.error();
            }
            if (auto block = TMaybeNode<TBlockExpr>((*loaded)->Ast)) {
                for (const auto& stmt : block.Cast()->Stmts) {
                    if (auto fd = TMaybeNode<TFunDecl>(stmt)) {
                        auto fun = fd.Cast();
                        if (fun->LiteralSuffix && context.LexerContext) {
                            context.LexerContext->ImportLiteralSuffix(*fun->LiteralSuffix, fun->Name);
                        }
                        continue;
                    }
                    auto td = TMaybeNode<TTypeDeclStmt>(stmt);
                    if (!td) continue;
                    auto named = TMaybeType<TNamedType>(td.Cast()->Type);
                    if (!named) continue;
                    if (mm) {
                        mm->RegisterImportedType(named.Cast()->Name, named.Cast()->UnderlyingType);
                    }
                    if (context.LexerContext) {
                        context.LexerContext->ImportTypeNames({named.Cast()->Name});
                    }
                }
            }
        } else if (mm) {
            auto result = mm->ImportModule(moduleName);
            if (!result) {
                co_return TError(first.Location, result.error());
            }
            if (result.value() && context.LexerContext) {
                context.LexerContext->ImportTypeNames(mm->GetAllImportedTypeNames());
                for (const auto& s : mm->GetAllImportedLiteralSuffixes()) {
                    context.LexerContext->ImportLiteralSuffix(s.Suffix, s.CtorFunction);
                }
            }
        }
        co_return std::make_shared<TUseExpr>(first.Location, moduleName);
    } else {
        co_return unexpected(stream);
    }
}

} // namespace

std::expected<TExprPtr, TError> TParser::parse(
    TTokenStream& stream, IModuleManager* mm, NFrontend::TSourceModuleLoader* loader)
{
    TWrappedTokenStream wrappedStream(stream, /*windowSize = */ 10);
    TParserContext context(wrappedStream, mm, stream.GetContext(), loader);
    auto task = stmt_list(context, {}, context.Stream.GetLocation());
    auto result = task.result();
    if (result && mm) {
        mm->ApplyPragmas(stream.GetContext()->GetPragmas());
    }
    return result;
}

} // namespace NAst
} // namespace NQumir
