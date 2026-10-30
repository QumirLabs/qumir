#include <gtest/gtest.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>
#include <qumir/parser/operator.h>

#include <sstream>

using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst::NLiterals;

using NQumir::NAst::TWrappedTokenStream;
using NQumir::NAst::TToken;
using NQumir::NAst::TMaybeNode;
using NQumir::NAst::TMaybeType;
using NQumir::NAst::TAwaitExpr;
using NQumir::NAst::TBlockExpr;
using NQumir::NAst::TCallExpr;
using NQumir::NAst::TFunDecl;
using NQumir::NAst::TFutureType;
using NQumir::NAst::TGenericArg;
using NQumir::NAst::TGenericParam;
using NQumir::NAst::TIntegerType;
using NQumir::NAst::TFloatType;
using NQumir::NAst::TNamedType;
using NQumir::NAst::TPointerType;
using NQumir::NAst::TStructType;
using NQumir::NAst::TTypeDeclStmt;
using NQumir::NAst::TTypePtr;
using NQumir::NAst::TVarStmt;
using NQumir::NAst::TVoidType;
using NQumir::NAst::MangleFunctionSignature;
using NQumir::NAst::TypeKey;
using NQumir::NAst::TypeMangleKey;

namespace {

#define ExpectOp(t, op) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Operator); \
        EXPECT_EQ(value.Value.i64, static_cast<int64_t>(op)); \
    } while (0)

#define ExpectIdent(t, name) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Identifier); \
        EXPECT_EQ(value.Name, (name)); \
    } while (0)

#define ExpectInt(t, v) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Integer); \
        EXPECT_EQ(value.Value.i64, (v)); \
    } while (0)

#define ExpectFloat(t, v) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Float); \
        EXPECT_DOUBLE_EQ(value.Value.f64, (v)); \
    } while (0)

#define ExpectBool(t, v) \
    do { \
        auto value = (t); \
        EXPECT_EQ(value.Type, TToken::Boolean); \
        EXPECT_EQ(value.Value.i64, (v)); \
    } while (0)

TTypePtr ParseVarType(const std::string& type) {
    std::istringstream input("(var value " + type + ")");
    TTokenStream tokens(input);
    TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        ADD_FAILURE() << parsed.error().ToString();
        return nullptr;
    }
    auto variable = TMaybeNode<TVarStmt>(*parsed).Cast();
    if (!variable) {
        ADD_FAILURE() << "expected variable declaration";
        return nullptr;
    }
    return variable->Type;
}

} // namespace

TEST(CoreLexerTest, FormsAndCompositeTypes) {
    std::istringstream input("(let (x 1) (if #t x 0)):<named color i32>");
    TTokenStream tokens(input);

    ExpectOp(tokens.Next(), "("_op);
    ExpectIdent(tokens.Next(), "let");
    ExpectOp(tokens.Next(), "("_op);
    ExpectIdent(tokens.Next(), "x");
    ExpectInt(tokens.Next(), 1);
    ExpectOp(tokens.Next(), ")"_op);
    ExpectOp(tokens.Next(), "("_op);
    ExpectIdent(tokens.Next(), "if");
    ExpectBool(tokens.Next(), 1);
    ExpectIdent(tokens.Next(), "x");
    ExpectInt(tokens.Next(), 0);
    ExpectOp(tokens.Next(), ")"_op);
    ExpectOp(tokens.Next(), ")"_op);
    ExpectOp(tokens.Next(), ":"_op);
    ExpectOp(tokens.Next(), "<"_op);
    ExpectIdent(tokens.Next(), "named");
    ExpectIdent(tokens.Next(), "color");
    ExpectIdent(tokens.Next(), "i32");
    ExpectOp(tokens.Next(), ">"_op);
    ExpectOp(tokens.Next(), NQumir::NAst::TOperator((uint64_t)-1));
}

TEST(CoreLexerTest, LiteralsAndBarIdentifiers) {
    std::istringstream input(R"(|foo bar| 42 1.25 .5 3e-2 "a\nb" '\n' #f)");
    TTokenStream tokens(input);

    ExpectIdent(tokens.Next(), "foo bar");
    ExpectInt(tokens.Next(), 42);
    ExpectFloat(tokens.Next(), 1.25);
    ExpectFloat(tokens.Next(), .5);
    ExpectFloat(tokens.Next(), 3e-2);

    auto str = tokens.Next();
    EXPECT_EQ(str.Type, TToken::String);
    EXPECT_EQ(str.Name, std::string("a\nb"));

    auto ch = tokens.Next();
    EXPECT_EQ(ch.Type, TToken::Char);
    EXPECT_EQ(ch.Value.i64, '\n');

    ExpectBool(tokens.Next(), 0);
    ExpectOp(tokens.Next(), NQumir::NAst::TOperator((uint64_t)-1));
}

TEST(CoreWrappedTokenStreamTest, Window) {
    std::istringstream input("(block x)");
    TTokenStream base(input);
    TWrappedTokenStream tokens(base, 2);

    ExpectOp(tokens.Next(), "("_op);
    ExpectIdent(tokens.Next(), "block");
    ExpectIdent(tokens.Next(), "x");

    ASSERT_EQ(tokens.GetWindow().size(), 2);
    ExpectIdent(tokens.GetWindow()[0], "block");
    ExpectIdent(tokens.GetWindow()[1], "x");
}

TEST(CoreTypeTest, FutureTypesPrintAndParse) {
    auto voidFuture = std::make_shared<TFutureType>(std::make_shared<TVoidType>());
    EXPECT_EQ(PrintType(voidFuture), "<future void>");
    EXPECT_EQ(TypeKey(voidFuture), "Future::Void");

    auto namedFuture = std::make_shared<TFutureType>(
        std::make_shared<TNamedType>("color", std::make_shared<TIntegerType>()));
    EXPECT_EQ(PrintType(namedFuture), "<future <named color i64>>");
    EXPECT_EQ(TypeKey(namedFuture), "Future::Named::color");

    std::istringstream input("(fun f () -> <future i64> (block))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    auto fun = TMaybeNode<TFunDecl>(*parsed).Cast();
    ASSERT_TRUE(fun);
    auto future = TMaybeType<TFutureType>(fun->RetType).Cast();
    ASSERT_TRUE(future);
    EXPECT_TRUE(TMaybeType<TIntegerType>(future->ResultType));
}

TEST(CoreTypeTest, ParametricNamedTypeArgsPrintAndParse) {
    auto type = ParseVarType("<named Nullable [T]>");
    auto named = TMaybeType<TNamedType>(type).Cast();
    ASSERT_TRUE(named);
    ASSERT_EQ(named->TypeArgs.size(), 1u);
    EXPECT_EQ(named->TypeArgs[0].Kind, TGenericArg::EKind::Type);

    auto arg = TMaybeType<TNamedType>(named->TypeArgs[0].Type).Cast();
    ASSERT_TRUE(arg);
    EXPECT_EQ(arg->Name, "T");

    EXPECT_EQ(TypeKey(type), "Named::Nullable[Named::T]");
    EXPECT_EQ(PrintType(type, TPrintOptions{.ShortNamedTypes = {"T"}}), "<named Nullable [T]>");
}

TEST(CoreTypeTest, ParametricNamedTypeConcreteArgsPrintAndParse) {
    auto type = ParseVarType("<named Nullable [i64]>");
    auto named = TMaybeType<TNamedType>(type).Cast();
    ASSERT_TRUE(named);
    ASSERT_EQ(named->TypeArgs.size(), 1u);
    EXPECT_EQ(named->TypeArgs[0].Kind, TGenericArg::EKind::Type);
    EXPECT_TRUE(TMaybeType<TIntegerType>(named->TypeArgs[0].Type));

    EXPECT_EQ(TypeKey(type), "Named::Nullable[i64]");
    EXPECT_EQ(PrintType(type), "<named Nullable [i64]>");
}

TEST(CoreTypeTest, ParametricNamedTypeValueArgsPrintAndParse) {
    auto type = ParseVarType("<named Decimal [42]>");
    auto named = TMaybeType<TNamedType>(type).Cast();
    ASSERT_TRUE(named);
    ASSERT_EQ(named->TypeArgs.size(), 1u);
    EXPECT_EQ(named->TypeArgs[0].Kind, TGenericArg::EKind::Value);
    EXPECT_EQ(named->TypeArgs[0].Value, "42");

    EXPECT_EQ(TypeKey(type), "Named::Decimal[Value::42]");
    EXPECT_EQ(PrintType(type), "<named Decimal [42]>");
}

TEST(CoreTypeTest, TypeMangleKeyUsesPlainNamedAndStructLayout) {
    auto structure = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"Value", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"Valid", std::make_shared<TNamedType>("n", nullptr)},
        {"Bytes", std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8))},
    });
    auto named = std::make_shared<TNamedType>("AggKey_i32_n", structure);

    EXPECT_EQ(TypeMangleKey(named), "AggKey_i32_n");
    EXPECT_EQ(TypeMangleKey(structure), "Struct_i32_n_Ptr_u8");
    EXPECT_EQ(
        MangleFunctionSignature(
            "__overload_key_clone_owned",
            std::make_shared<TVoidType>(),
            {named, std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8))}),
        "__overload_key_clone_owned$void__$AggKey_i32_n$Ptr_u8");
}

TEST(CoreTypeTest, ParametricTypeDeclPrintsAndParses) {
    std::istringstream input("(block (type Nullable [T] <struct (Value T) (Valid i8)>))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    auto block = TMaybeNode<TBlockExpr>(*parsed).Cast();
    ASSERT_TRUE(block);
    ASSERT_EQ(block->Stmts.size(), 1u);

    auto typeDecl = TMaybeNode<TTypeDeclStmt>(block->Stmts[0]).Cast();
    ASSERT_TRUE(typeDecl);
    ASSERT_EQ(typeDecl->GenericParams.size(), 1u);
    EXPECT_EQ(typeDecl->GenericParams[0].Name, "T");
    EXPECT_EQ(typeDecl->GenericParams[0].Kind, TGenericParam::EKind::Type);

    auto named = TMaybeType<TNamedType>(typeDecl->Type).Cast();
    ASSERT_TRUE(named);
    EXPECT_EQ(named->Name, "Nullable");

    auto structure = TMaybeType<TStructType>(named->UnderlyingType).Cast();
    ASSERT_TRUE(structure);
    ASSERT_EQ(structure->Fields.size(), 2u);

    auto value = TMaybeType<TNamedType>(structure->Fields[0].second).Cast();
    ASSERT_TRUE(value);
    EXPECT_EQ(value->Name, "T");

    const auto expected = std::string("(type Nullable [T] <struct (Value T) (Valid i8)>)");
    EXPECT_EQ(PrintAst(typeDecl, TPrintOptions{.Pretty = false}), expected);
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(block " + expected + ")");
}

TEST(CoreTypeTest, ParametricTypeDeclConstParamsPrintAndParse) {
    std::istringstream input("(block (type Decimal [(const Scale i32) (const Precision i32)] <struct (Lo i64) (Hi i64)>))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    auto block = TMaybeNode<TBlockExpr>(*parsed).Cast();
    ASSERT_TRUE(block);
    ASSERT_EQ(block->Stmts.size(), 1u);

    auto typeDecl = TMaybeNode<TTypeDeclStmt>(block->Stmts[0]).Cast();
    ASSERT_TRUE(typeDecl);
    ASSERT_EQ(typeDecl->GenericParams.size(), 2u);
    EXPECT_EQ(typeDecl->GenericParams[0].Name, "Scale");
    EXPECT_EQ(typeDecl->GenericParams[0].Kind, TGenericParam::EKind::Value);
    EXPECT_TRUE(TMaybeType<TIntegerType>(typeDecl->GenericParams[0].ValueType));
    EXPECT_EQ(typeDecl->GenericParams[1].Name, "Precision");
    EXPECT_EQ(typeDecl->GenericParams[1].Kind, TGenericParam::EKind::Value);
    EXPECT_TRUE(TMaybeType<TIntegerType>(typeDecl->GenericParams[1].ValueType));

    const auto expected = std::string("(type Decimal [(const Scale i32) (const Precision i32)] <struct (Lo i64) (Hi i64)>)");
    EXPECT_EQ(PrintAst(typeDecl, TPrintOptions{.Pretty = false}), expected);
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(block " + expected + ")");
}

TEST(CoreTypeTest, ParametricTypeDeclRejectsDuplicateParams) {
    std::istringstream input("(block (type Box [T T] <struct (Value T)>))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().ToString().find("duplicate generic parameter: T"), std::string::npos);
}

TEST(CoreTypeTest, IntegerWidthsPrintAndParse) {
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::I8)), "i8");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::I16)), "i16");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::I32)), "i32");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::I64)), "i64");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::U8)), "u8");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::U16)), "u16");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::U32)), "u32");
    EXPECT_EQ(PrintType(std::make_shared<TIntegerType>(TIntegerType::U64)), "u64");
    EXPECT_EQ(PrintType(std::make_shared<TFloatType>(TFloatType::F32)), "f32");
    EXPECT_EQ(PrintType(std::make_shared<TFloatType>()), "f64");

    std::istringstream input("(var x u32)");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(var x u32)");
}

TEST(CoreParseTypeTest, ParsesStandaloneType) {
    const std::vector<std::string> types = {
        "i64",
        "u8",
        "f32",
        "f64",
        "bool",
        "string",
        "char",
        "void",
        "<i64 (readonly)>",
        "<array i64 2>",
        "<ptr f64>",
    };

    for (const auto& text : types) {
        SCOPED_TRACE(text);
        std::istringstream input(text);
        TTokenStream tokens(input);

        auto type = ParseType(tokens);
        ASSERT_TRUE(type.has_value()) << type.error().ToString();
        EXPECT_EQ(PrintType(*type), text);
    }
}

TEST(CoreParseTypeTest, ParsesGenericNamedType) {
    std::istringstream input("<named Pair [i64 42 <ptr f64>]>");
    TTokenStream tokens(input);

    auto type = ParseType(tokens);
    ASSERT_TRUE(type.has_value()) << type.error().ToString();

    auto named = TMaybeType<TNamedType>(*type).Cast();
    ASSERT_TRUE(named);
    ASSERT_EQ(named->TypeArgs.size(), 3u);

    EXPECT_EQ(named->TypeArgs[0].Kind, TGenericArg::EKind::Type);
    EXPECT_TRUE(TMaybeType<TIntegerType>(named->TypeArgs[0].Type));

    EXPECT_EQ(named->TypeArgs[1].Kind, TGenericArg::EKind::Value);
    EXPECT_EQ(named->TypeArgs[1].Value, "42");

    EXPECT_EQ(named->TypeArgs[2].Kind, TGenericArg::EKind::Type);
    EXPECT_TRUE(TMaybeType<TPointerType>(named->TypeArgs[2].Type));

    EXPECT_EQ(TypeKey(*type), "Named::Pair[i64,Value::42,Ptr::f64]");
    EXPECT_EQ(PrintType(*type), "<named Pair [i64 42 <ptr f64>]>");
}

TEST(CoreParseTypeTest, ReportsMalformedType) {
    std::istringstream input("<array i64>");
    TTokenStream tokens(input);

    auto type = ParseType(tokens);
    ASSERT_FALSE(type.has_value());
    EXPECT_NE(type.error().ToString().find("expected array arity"), std::string::npos);
}

TEST(CoreParseTypeTest, LeavesTrailingTokensInTheStream) {
    std::istringstream input("i64 f64");
    TTokenStream tokens(input);

    auto first = ParseType(tokens);
    ASSERT_TRUE(first.has_value()) << first.error().ToString();
    EXPECT_EQ(PrintType(*first), "i64");

    auto second = ParseType(tokens);
    ASSERT_TRUE(second.has_value()) << second.error().ToString();
    EXPECT_EQ(PrintType(*second), "f64");
}

TEST(CoreTypeAttrs, ParseAndPrintCanonicalAccessModes) {
    struct TCase {
        std::string Source;
        bool Readable;
        bool Mutable;
        std::string Canonical;
    };
    const std::vector<TCase> cases = {
        {"i64", true, true, "i64"},
        {"<i64 (mutable)>", true, true, "i64"},
        {"<i64 (readonly)>", true, false, "<i64 (readonly)>"},
        {"<i64 (writeonly)>", false, true, "<i64 (writeonly)>"},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.Source);
        auto type = ParseVarType(testCase.Source);
        ASSERT_NE(type, nullptr);
        EXPECT_EQ(type->Readable, testCase.Readable);
        EXPECT_EQ(type->Mutable, testCase.Mutable);
        EXPECT_EQ(PrintType(type), testCase.Canonical);

        auto reparsed = ParseVarType(testCase.Canonical);
        ASSERT_NE(reparsed, nullptr);
        EXPECT_EQ(reparsed->Readable, testCase.Readable);
        EXPECT_EQ(reparsed->Mutable, testCase.Mutable);
    }
}

TEST(CoreTypeAttrs, RejectsDuplicateAndConflictingAttributes) {
    const std::vector<std::string> types = {
        "<i64 (mutable mutable)>",
        "<i64 (readonly mutable)>",
        "<i64 (readonly writeonly)>",
        "<i64 (readable)>",
    };

    for (const auto& type : types) {
        SCOPED_TRACE(type);
        std::istringstream input("(var value " + type + ")");
        TTokenStream tokens(input);
        TParser parser;
        EXPECT_FALSE(parser.Parse(tokens).has_value());
    }
}

TEST(CoreParserTest, NonDefaultIntegerLiteralKeepsTypeAnnotation) {
    std::istringstream input("(output (: 42 u32))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(output (: 42 u32))");
}

TEST(CoreParserTest, AwaitPrintAndParse) {
    std::istringstream input("(await (call f))");
    TTokenStream tokens(input);
    TParser parser;

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    auto awaitExpr = TMaybeNode<TAwaitExpr>(*parsed).Cast();
    ASSERT_TRUE(awaitExpr);
    auto call = TMaybeNode<TCallExpr>(awaitExpr->Operand).Cast();
    ASSERT_TRUE(call);
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(await (call f))");
}

TEST(CoreParserTest, LifetimeFormsPrintAndParse) {
    const std::vector<std::string> forms{
        "(retain (borrow source))",
        "(own-literal \"literal\")",
        "(move owned)",
        "(destroy value)",
        "(destroy array size)",
        "(replace target (retain (borrow source)))",
        "(cleanup-exit (return (move result)) (destroy local))",
        "(cleanup-exit (break) (destroy local))",
        "(cleanup-exit (continue))",
        "(cleanup-global (destroy second) (destroy first))",
    };

    for (const auto& form : forms) {
        SCOPED_TRACE(form);
        std::istringstream input(form);
        TTokenStream tokens(input);
        TParser parser;

        auto parsed = parser.Parse(tokens);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
        EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), form);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
