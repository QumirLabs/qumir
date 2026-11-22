#include <gtest/gtest.h>
#include "test_types.h"
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
using NQumir::MakeIntrusive;
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

NQumir::NAst::TExprPtrOf<TVarStmt> ParseVar(const std::string& type) {
    std::istringstream input("(var value " + type + ")");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());
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
    return NQumir::NAst::TExprPtrOf<TVarStmt>(variable);
}

TTypePtr ParseVarType(const std::string& type) {
    auto variable = ParseVar(type);
    return variable ? variable->Type : nullptr;
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
    auto voidFuture = NQumir::NTest::TestTypes().MakeFuture(NQumir::NTest::TestTypes().Make<TVoidType>());
    EXPECT_EQ(PrintType(voidFuture), "<future void>");
    EXPECT_EQ(TypeKey(voidFuture), "Future::Void");

    auto namedFuture = NQumir::NTest::TestTypes().MakeFuture(
        NQumir::NTest::TestTypes().MakeNamed("color", NQumir::NTest::TestTypes().MakeInteger()));
    EXPECT_EQ(PrintType(namedFuture), "<future <named color i64>>");
    EXPECT_EQ(TypeKey(namedFuture), "Future::Named::color");

    std::istringstream input("(fun f () -> <future i64> (block))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

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
    auto structure = NQumir::NTest::TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
        {"Value", NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I32)},
        {"Valid", NQumir::NTest::TestTypes().MakeNamed("n", nullptr)},
        {"Bytes", NQumir::NTest::TestTypes().MakePointer(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U8))},
    });
    auto named = NQumir::NTest::TestTypes().MakeNamed("AggKey_i32_n", structure);

    EXPECT_EQ(TypeMangleKey(named), "AggKey_i32_n");
    EXPECT_EQ(TypeMangleKey(structure), "Struct_i32_n_Ptr_u8");
    EXPECT_EQ(
        MangleFunctionSignature(
            "__overload_key_clone_owned",
            NQumir::NTest::TestTypes().Make<TVoidType>(),
            {named, NQumir::NTest::TestTypes().MakePointer(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U8))}),
        "__overload_key_clone_owned$void__$AggKey_i32_n$Ptr_u8");
}

TEST(CoreTypeTest, ViewsAndUnwrapsTakeNoReference) {
    // Shared types are read from several threads, and the reference count is not atomic.
    auto& types = NQumir::NTest::TestTypes();
    TTypePtr integer = types.MakeInteger(TIntegerType::I16);
    TTypePtr reference = types.MakeReference(integer);
    TTypePtr named = types.MakeNamed("RefI16", reference);
    NQumir::NAst::TExprPtr node = MakeIntrusive<TVarStmt>(NQumir::TLocation{}, "x", named);
    const int integerCount = integer->Count;
    const int referenceCount = reference->Count;
    const int namedCount = named->Count;
    const int nodeCount = node->Count;

    const auto& unwrapped = NQumir::NAst::UnwrapReferenceType(NQumir::NAst::UnwrapNamedType(named));
    auto maybeInteger = TMaybeType<TIntegerType>(unwrapped);
    ASSERT_TRUE(maybeInteger);
    EXPECT_EQ(maybeInteger.Cast(), integer.Get());
    auto maybeVariable = TMaybeNode<TVarStmt>(node);
    ASSERT_TRUE(maybeVariable);
    EXPECT_EQ(maybeVariable.Cast()->Type, named);

    EXPECT_EQ(integer->Count, integerCount);
    EXPECT_EQ(reference->Count, referenceCount);
    EXPECT_EQ(named->Count, namedCount);
    EXPECT_EQ(node->Count, nodeCount);
}

TEST(CoreTypeTest, ParametricTypeDeclPrintsAndParses) {
    std::istringstream input("(block (type Nullable [T] <struct (Value T) (Valid i8)>))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

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
    EXPECT_EQ(PrintAst(block->Stmts[0], TPrintOptions{.Pretty = false}), expected);
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(block " + expected + ")");
}

TEST(CoreTypeTest, ParametricTypeDeclConstParamsPrintAndParse) {
    std::istringstream input("(block (type Decimal [(const Scale i32) (const Precision i32)] <struct (Lo i64) (Hi i64)>))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

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
    EXPECT_EQ(PrintAst(block->Stmts[0], TPrintOptions{.Pretty = false}), expected);
    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(block " + expected + ")");
}

TEST(CoreTypeTest, ParametricTypeDeclRejectsDuplicateParams) {
    std::istringstream input("(block (type Box [T T] <struct (Value T)>))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

    auto parsed = parser.Parse(tokens);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_NE(parsed.error().ToString().find("duplicate generic parameter: T"), std::string::npos);
}

TEST(CoreTypeTest, IntegerWidthsPrintAndParse) {
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I8)), "i8");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I16)), "i16");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I32)), "i32");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::I64)), "i64");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U8)), "u8");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U16)), "u16");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U32)), "u32");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeInteger(TIntegerType::U64)), "u64");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeFloat(TFloatType::F32)), "f32");
    EXPECT_EQ(PrintType(NQumir::NTest::TestTypes().MakeFloat()), "f64");

    std::istringstream input("(var x u32)");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

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
        "<array i64 2>",
        "<ptr f64>",
    };

    for (const auto& text : types) {
        SCOPED_TRACE(text);
        std::istringstream input(text);
        TTokenStream tokens(input);

        auto type = ParseType(tokens, NQumir::NTest::TestTypes());
        ASSERT_TRUE(type.has_value()) << type.error().ToString();
        EXPECT_EQ(PrintType(*type), text);
    }
}

TEST(CoreParseTypeTest, ParsesGenericNamedType) {
    std::istringstream input("<named Pair [i64 42 <ptr f64>]>");
    TTokenStream tokens(input);

    auto type = ParseType(tokens, NQumir::NTest::TestTypes());
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

    auto type = ParseType(tokens, NQumir::NTest::TestTypes());
    ASSERT_FALSE(type.has_value());
    EXPECT_NE(type.error().ToString().find("expected array arity"), std::string::npos);
}

TEST(CoreParseTypeTest, LeavesTrailingTokensInTheStream) {
    std::istringstream input("i64 f64");
    TTokenStream tokens(input);

    auto first = ParseType(tokens, NQumir::NTest::TestTypes());
    ASSERT_TRUE(first.has_value()) << first.error().ToString();
    EXPECT_EQ(PrintType(*first), "i64");

    auto second = ParseType(tokens, NQumir::NTest::TestTypes());
    ASSERT_TRUE(second.has_value()) << second.error().ToString();
    EXPECT_EQ(PrintType(*second), "f64");
}

TEST(CoreVarAttrs, ParseAndPrintCanonicalAccessModes) {
    struct TCase {
        std::string Source;
        bool Readable;
        bool Mutable;
        std::string Canonical;
    };
    const std::vector<TCase> cases = {
        {"i64", true, true, "i64"},
        {"i64 (attrs)", true, true, "i64"},
        {"i64 (attrs mutable)", true, true, "i64"},
        {"i64 (attrs readonly)", true, false, "i64 (attrs readonly)"},
        {"<ref i64> (attrs writeonly)", false, true, "<ref i64> (attrs writeonly)"},
        {"<array f64 1> [0 9] (attrs readonly)", true, false, "<array f64 1> [0 9] (attrs readonly)"},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.Source);
        auto var = ParseVar(testCase.Source);
        ASSERT_NE(var, nullptr);
        EXPECT_EQ(var->Access.Readable, testCase.Readable);
        EXPECT_EQ(var->Access.Mutable, testCase.Mutable);
        EXPECT_EQ(PrintAst(var), "(var value " + testCase.Canonical + ")");

        auto reparsed = ParseVar(testCase.Canonical);
        ASSERT_NE(reparsed, nullptr);
        EXPECT_EQ(reparsed->Access.Readable, testCase.Readable);
        EXPECT_EQ(reparsed->Access.Mutable, testCase.Mutable);
    }
}

TEST(CoreVarAttrs, RejectsDuplicateConflictingAndMisplacedAttributes) {
    const std::vector<std::string> types = {
        "i64 (attrs mutable mutable)",
        "i64 (attrs readonly mutable)",
        "i64 (attrs readonly writeonly)",
        "i64 (attrs readable)",
        "i64 (readonly)",
        "<i64 (readonly)>", // access belongs to the variable, not to the type
    };

    for (const auto& type : types) {
        SCOPED_TRACE(type);
        std::istringstream input("(var value " + type + ")");
        TTokenStream tokens(input);
        TParser parser(NQumir::NTest::TestTypes());
        EXPECT_FALSE(parser.Parse(tokens).has_value());
    }
}

TEST(CoreParserTest, NonDefaultIntegerLiteralKeepsTypeAnnotation) {
    std::istringstream input("(output (: 42 u32))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), "(output (: 42 u32))");
}

TEST(CoreParserTest, AwaitPrintAndParse) {
    std::istringstream input("(await (call f))");
    TTokenStream tokens(input);
    TParser parser(NQumir::NTest::TestTypes());

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
        TParser parser(NQumir::NTest::TestTypes());

        auto parsed = parser.Parse(tokens);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();
        EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), form);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace {

const std::vector<std::string>& TrickyIdentifiers() {
    static const std::vector<std::string> names = {
        "x(y)", "a)b", "a(b", "a;b", "a>b", "a<b", "a-b", "a/b", "a,b", "a\"b", "a'b", "a#b",
        "a\\b", "a\tb", "a\nb", "x ", "a b", "a.b", "1x", "42", "-1", ".5", "#t", "a[b]", "a=b",
        "\xD0\xB6 \xD1\x8B", "x\ry", "foo", "my_var", "$x", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82",
        "|", "||", "a|b", "|a", "a|", "\\", "\\|", "|\\", "\\n", "\\\\|", " x", ")x", " ", ")", "\tx", "\nx", "\rx", "\vx", "\fx",
        "<<", "<=", ">=", ">>", "a\\nb", "| |", "|)|", " |)\t\n\r\\|", "x |y\\z| w",
    };
    return names;
}

} // namespace

TEST(CoreIdentifierQuotingTest, PlainNamesStayBare) {
    for (const auto* name : {"foo", "my_var", "$x", "x1", "+", "-", "->", "a:b", "<main>"}) {
        SCOPED_TRACE(name);
        auto var = MakeIntrusive<NQumir::NAst::TVarStmt>(
            NQumir::TLocation{}, name, NQumir::NTest::TestTypes().Make<NQumir::NAst::TBoolType>());
        EXPECT_EQ(PrintAst(var, TPrintOptions{.Pretty = false}), std::string("(var ") + name + " bool)");
    }
}

TEST(CoreIdentifierQuotingTest, PlainIdentifiersMatchTheLexer) {
    for (const auto* name : {"foo", "my_var", "$x", "x1", "a:b", "_", "\xD0\xB6"}) {
        SCOPED_TRACE(name);
        EXPECT_TRUE(IsPlainIdentifier(name));
    }
    for (const auto* name : {"", "1x", "a b", "+", "->", "<main>", "<<", "|", "a-b", ":a"}) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(IsPlainIdentifier(name));
    }
    for (const auto& name : TrickyIdentifiers()) {
        if (!IsPlainIdentifier(name)) {
            continue;
        }
        SCOPED_TRACE(name);
        std::istringstream input("(x " + name + ")");
        TTokenStream tokens(input);
        tokens.Next();
        tokens.Next();
        auto ident = tokens.Next();
        EXPECT_TRUE(ident.Type == TToken::Identifier && ident.Name == name);
        EXPECT_EQ(tokens.Next().RawValue, ")");
    }
}

TEST(CoreIdentifierQuotingTest, StructFieldNamesRoundTrip) {
    for (const auto& name : TrickyIdentifiers()) {
        SCOPED_TRACE(name);
        auto type = NQumir::NTest::TestTypes().MakeStruct(std::vector<std::pair<std::string, TTypePtr>>{
            {name, NQumir::NTest::TestTypes().Make<NQumir::NAst::TBoolType>()},
            {"z", NQumir::NTest::TestTypes().Make<NQumir::NAst::TBoolType>()},
        });
        const auto text = PrintType(type);

        std::istringstream input(text);
        TTokenStream tokens(input);
        auto parsed = ParseType(tokens, NQumir::NTest::TestTypes());
        ASSERT_TRUE(parsed.has_value()) << text << ": " << parsed.error().ToString();
        auto st = TMaybeType<TStructType>(*parsed).Cast();
        ASSERT_TRUE(st) << text;
        ASSERT_EQ(st->Fields.size(), 2u);
        EXPECT_EQ(st->Fields[0].first, name) << text;
        EXPECT_EQ(PrintType(*parsed), text);
    }
}

TEST(CoreIdentifierQuotingTest, VariableNamesRoundTrip) {
    for (const auto& name : TrickyIdentifiers()) {
        SCOPED_TRACE(name);
        auto var = MakeIntrusive<NQumir::NAst::TVarStmt>(
            NQumir::TLocation{}, name, NQumir::NTest::TestTypes().Make<NQumir::NAst::TBoolType>());
        const auto text = PrintAst(var, TPrintOptions{.Pretty = false});

        std::istringstream input(text);
        TTokenStream tokens(input);
        TParser parser(NQumir::NTest::TestTypes());
        auto parsed = parser.Parse(tokens);
        ASSERT_TRUE(parsed.has_value()) << text << ": " << parsed.error().ToString();
        auto parsedVar = NQumir::NAst::TMaybeNode<NQumir::NAst::TVarStmt>(*parsed).Cast();
        ASSERT_TRUE(parsedVar) << text;
        EXPECT_EQ(parsedVar->Name, name) << text;
        EXPECT_EQ(PrintAst(*parsed, TPrintOptions{.Pretty = false}), text);
    }
}

TEST(CoreIdentifierQuotingTest, EscapesInBars) {
    auto read = [](const std::string& text) {
        std::istringstream input(text);
        TTokenStream tokens(input);
        auto ident = tokens.Next();
        EXPECT_TRUE(ident.Type == TToken::Identifier) << text;
        EXPECT_TRUE(tokens.Next().IsEof()) << text;
        return ident.Name;
    };
    EXPECT_EQ(read("|a\\|b|"), "a|b");
    EXPECT_EQ(read("|a\\\\b|"), "a\\b");
    EXPECT_EQ(read("|a\\nb\\tc\\rd|"), "a\nb\tc\rd");
    EXPECT_EQ(read("|\\ x|"), " x");
    EXPECT_EQ(read("|\\)x|"), ")x");
    // Names without a backslash are read as before; an unknown escape stays verbatim.
    EXPECT_EQ(read("|a b.c|"), "a b.c");
    EXPECT_EQ(read("|a\\qb|"), "a\\qb");
    EXPECT_EQ(read("||"), "||");
    EXPECT_EQ(read("|"), "|");
}

TEST(CoreIdentifierQuotingTest, EmptyNameIsRejected) {
    auto var = MakeIntrusive<NQumir::NAst::TVarStmt>(
        NQumir::TLocation{}, "", NQumir::NTest::TestTypes().Make<NQumir::NAst::TBoolType>());
    EXPECT_THROW(PrintAst(var, TPrintOptions{.Pretty = false}), std::invalid_argument);
}
