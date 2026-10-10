#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/semantics/type_annotation/type_annotation.h>

#include <gtest/gtest.h>

#include <sstream>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

std::expected<TExprPtr, TError> Annotate(const std::string& source) {
    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    NSemantics::TNameResolver resolver;
    resolver.ApplyPragmas(parser.Pragmas);
    if (auto error = resolver.Resolve(*parsed)) {
        return std::unexpected(*error);
    }
    return NTypeAnnotation::TTypeAnnotator(resolver).Annotate(*parsed);
}

std::expected<TExprPtr, TError> AnnotateBinary(
    const std::string& op,
    const std::string& left,
    const std::string& right)
{
    auto root = Annotate("(block (var a " + left + ") (var b " + right
        + ") (var result = (" + op + " a b)))");
    if (!root) {
        return root;
    }
    return TMaybeNode<TVarStmt>(TMaybeNode<TBlockExpr>(*root).Cast()->Stmts.back()).Cast()->Init;
}

std::string TypeName(const TTypePtr& type) {
    return TypeKey(UnwrapReferenceType(type));
}

} // namespace

TEST(BinaryTypeAnnotation, NumericVectorPromotionAndDivision) {
    struct TCase {
        std::string Op;
        std::string Left;
        std::string Right;
        std::string Result;
        std::string LeftOperand;
        std::string RightOperand;
    };
    const std::vector<TCase> cases = {
        {"+", "<vec i32 4>", "<vec i32 4>", "Vector::i32::4", "Vector::i32::4", "Vector::i32::4"},
        {"+", "<vec i32 4>", "<vec f64 4>", "Vector::Float::4", "Vector::Float::4", "Vector::Float::4"},
        {"+", "<vec f64 4>", "<vec i32 4>", "Vector::Float::4", "Vector::Float::4", "Vector::Float::4"},
        {"*", "<vec i32 4>", "i64", "Vector::i64::4", "Vector::i64::4", "i64"},
        {"+", "<vec i32 4>", "f64", "Vector::Float::4", "Vector::Float::4", "Float"},
        {"-", "i32", "<vec i32 4>", "Vector::i32::4", "i32", "Vector::i32::4"},
        {"/", "<vec i32 4>", "<vec i32 4>", "Vector::Float::4", "Vector::Float::4", "Vector::Float::4"},
        {"/", "i32", "<vec i32 4>", "Vector::Float::4", "Float", "Vector::Float::4"},
        {"//", "<vec i32 4>", "<vec i32 4>", "Vector::i32::4", "Vector::i32::4", "Vector::i32::4"},
        {"%", "<vec i32 4>", "i32", "Vector::i32::4", "Vector::i32::4", "i32"},
        {">", "<vec i32 4>", "f64", "Vector::Bool::4", "Vector::Float::4", "Float"},
        {"==", "<vec bool 4>", "bool", "Vector::Bool::4", "Vector::Bool::4", "Bool"},
        {"&", "<vec bool 4>", "<vec bool 4>", "Vector::Bool::4", "Vector::Bool::4", "Vector::Bool::4"},
        {"|", "bool", "<vec bool 4>", "Vector::Bool::4", "Bool", "Vector::Bool::4"},
        {"&&", "<vec i32 4>", "i32", "Vector::Bool::4", "Vector::Bool::4", "Bool"},
        {">>", "<vec u32 4>", "i64", "Vector::u32::4", "Vector::u32::4", "u32"},
        {"+", "i32", "f64", "Float", "Float", "Float"},
        {"/", "i32", "i32", "Float", "Float", "Float"},
        {"//", "i32", "i32", "i32", "i32", "i32"},
        {">>", "u32", "i64", "u32", "u32", "i64"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.Left + " " + test.Op + " " + test.Right);
        auto result = AnnotateBinary(test.Op, test.Left, test.Right);
        ASSERT_TRUE(result.has_value()) << result.error().ToString();
        auto binary = TMaybeNode<TBinaryExpr>(*result).Cast();
        ASSERT_NE(binary, nullptr);
        EXPECT_EQ(TypeName(binary->Type), test.Result);
        EXPECT_EQ(TypeName(binary->Left->Type), test.LeftOperand);
        EXPECT_EQ(TypeName(binary->Right->Type), test.RightOperand);
    }
}

TEST(BinaryTypeAnnotation, RejectsInvalidVectorOperands) {
    struct TCase {
        std::string Op;
        std::string Left;
        std::string Right;
    };
    const std::vector<TCase> cases = {
        {"+", "<vec i32 4>", "<vec i32 8>"},
        {"+", "<vec i32 0>", "i32"},
        {"+", "i32", "<vec i32 0>"},
        {"+", "<vec i32 4>", "string"},
        {"+", "<vec i32 4>", "bool"},
        {"+", "<vec bool 4>", "<vec bool 4>"},
        {"*", "<vec bool 4>", "bool"},
        {"+", "<vec i64 4>", "<vec u64 4>"},
        {"//", "<vec f64 4>", "<vec f64 4>"},
        {"%", "<vec f64 4>", "i32"},
        {"&", "<vec f64 4>", "i32"},
        {"<<", "<vec bool 4>", "bool"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.Left + " " + test.Op + " " + test.Right);
        auto result = AnnotateBinary(test.Op, test.Left, test.Right);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().ToString().find("Line: 1"), std::string::npos);
    }
}

TEST(BinaryTypeAnnotation, StringAndSymbolOperators) {
    for (const auto& left : {"string", "char"}) {
        for (const auto& right : {"string", "char"}) {
            for (const auto& op : {"+", "==", "!=", "<", "<=", ">", ">="}) {
                SCOPED_TRACE(std::string(left) + " " + op + " " + right);
                auto result = AnnotateBinary(op, left, right);
                ASSERT_TRUE(result.has_value()) << result.error().ToString();
                auto binary = TMaybeNode<TBinaryExpr>(*result).Cast();
                ASSERT_NE(binary, nullptr);
                EXPECT_EQ(TypeName(binary->Type), std::string(op) == "+" ? "String" : "Bool");
                if (std::string(op) == "+" || std::string(left) == "string" || std::string(right) == "string") {
                    EXPECT_EQ(TypeName(binary->Left->Type), "String");
                    EXPECT_EQ(TypeName(binary->Right->Type), "String");
                }
            }
        }
    }
    EXPECT_FALSE(AnnotateBinary("+", "string", "i32").has_value());
    EXPECT_FALSE(AnnotateBinary("*", "string", "string").has_value());
}

TEST(BinaryTypeAnnotation, ScalarLiteralUsesVectorElementType) {
    auto root = Annotate("(block (var a <vec i8 4>) (var result = (+ a 1)))");
    ASSERT_TRUE(root.has_value()) << root.error().ToString();
    auto value = TMaybeNode<TVarStmt>(TMaybeNode<TBlockExpr>(*root).Cast()->Stmts.back()).Cast()->Init;
    auto binary = TMaybeNode<TBinaryExpr>(value).Cast();
    ASSERT_NE(binary, nullptr);
    EXPECT_EQ(TypeName(binary->Type), "Vector::i8::4");
    EXPECT_EQ(TypeName(binary->Right->Type), "i8");
}

TEST(BinaryTypeAnnotation, VectorAssignmentsCheckShapeAndElementType) {
    EXPECT_FALSE(Annotate("(block (var a <vec i32 4>) (var b <vec i32 8>) (= b a))").has_value());
    EXPECT_FALSE(Annotate("(block (var a <vec i64 4>) (var b <vec i32 4>) (= b a))").has_value());
    auto root = Annotate("(block (var a <vec i32 4>) (var b <vec f64 4>) (= b a))");
    ASSERT_TRUE(root.has_value()) << root.error().ToString();
    auto assignment = TMaybeNode<TAssignExpr>(TMaybeNode<TBlockExpr>(*root).Cast()->Stmts.back()).Cast();
    ASSERT_NE(assignment, nullptr);
    EXPECT_EQ(TypeName(assignment->Value->Type), "Vector::Float::4");
    EXPECT_TRUE(TMaybeNode<TCastExpr>(assignment->Value));
}

TEST(VectorTypeAnnotation, InfersTypeFromScalarExpressions) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"(vec 1 (+ 2 3) 4 5)", "Vector::i64::4"},
        {"(vec 0.5 2.5)", "Vector::Float::2"},
        {"(vec #t #f)", "Vector::Bool::2"},
        {"(vec (: 1 i16) (: 2 i16))", "Vector::i16::2"},
    };
    for (const auto& [source, type] : cases) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), type);
    }
}

TEST(VectorTypeAnnotation, ConvertsElementsToExplicitType) {
    auto result = Annotate(
        "(block (var a i32) (var b i32)"
        " (var value = (: (vec a (+ b 1)) <vec f64 2>)))");
    ASSERT_TRUE(result) << result.error().ToString();
    auto variable = TMaybeNode<TVarStmt>(TMaybeNode<TBlockExpr>(*result).Cast()->Stmts.back()).Cast();
    auto vector = TMaybeNode<TVectorExpr>(variable->Init).Cast();
    ASSERT_NE(vector, nullptr);
    EXPECT_EQ(TypeName(vector->Type), "Vector::Float::2");
    for (const auto& element : vector->Elements) {
        ASSERT_TRUE(TMaybeNode<TCastExpr>(element));
        EXPECT_EQ(TypeName(element->Type), "Float");
    }
    NCore::TPrintOptions options;
    options.Pretty = false;
    auto printed = NCore::PrintAst(*result, options);
    EXPECT_EQ(printed.find("(: (vec"), std::string::npos);
    auto reparsed = Annotate(printed);
    ASSERT_TRUE(reparsed) << reparsed.error().ToString();
    auto reparsedVariable = TMaybeNode<TVarStmt>(TMaybeNode<TBlockExpr>(*reparsed).Cast()->Stmts.back()).Cast();
    EXPECT_EQ(TypeName(reparsedVariable->Type), TypeName(variable->Type));
}

TEST(VectorTypeAnnotation, ConvertsReferencedElementsToExplicitType) {
    auto result = Annotate(
        "(block (var a <ref i32>)"
        " (var value = (: (vec a a) <vec f64 2>)))");
    ASSERT_TRUE(result) << result.error().ToString();
    auto variable = TMaybeNode<TVarStmt>(TMaybeNode<TBlockExpr>(*result).Cast()->Stmts.back()).Cast();
    auto vector = TMaybeNode<TVectorExpr>(variable->Init).Cast();
    ASSERT_NE(vector, nullptr);
    for (const auto& element : vector->Elements) {
        EXPECT_TRUE(TMaybeNode<TCastExpr>(element));
        EXPECT_EQ(TypeName(element->Type), "Float");
    }
}

TEST(VectorTypeAnnotation, LiteralTypesSurvivePrintAndReannotation) {
    for (const std::string source : {
        "(: (vec 1 2) <vec f64 2>)",
        "(: (vec 1 127) <vec i8 2>)",
        "(: (vec 1 0) <vec bool 2>)",
        "(vec 1.0 -0.0)",
    }) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        NCore::TPrintOptions options;
        options.Pretty = false;
        auto printed = NCore::PrintAst(*result, options);
        EXPECT_EQ(printed.find("(: (vec"), std::string::npos);
        auto reparsed = Annotate(printed);
        ASSERT_TRUE(reparsed) << reparsed.error().ToString();
        EXPECT_EQ(TypeName((*reparsed)->Type), TypeName((*result)->Type)) << printed;
    }
}

TEST(VectorTypeAnnotation, RejectsInvalidConstruction) {
    for (const std::string source : {
        "(vec)",
        "(vec 1)",
        "(vec 1 2 3)",
        "(vec nil)",
        "(vec 1 nil)",
        "(vec \"one\" \"two\")",
        "(vec (vec 1 2) (vec 3 4))",
        "(vec 1 #t)",
        "(vec 1 2.5)",
        "(: (vec 1 2) i64)",
        "(: (vec 1 2) <vec i64 4>)",
        "(: (vec 1) <vec i64 0>)",
        "(: (vec 1 128) <vec i8 2>)",
        "(block (var a i64) (: (vec a a) <vec i8 2>))",
    }) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().ToString().find("Line: 1"), std::string::npos);
    }
}

TEST(VectorTypeAnnotation, AcceptsSupportedSizes) {
    for (int size : {2, 4, 8, 16, 32}) {
        SCOPED_TRACE(size);
        std::string source = "(vec";
        for (int index = 0; index < size; ++index) {
            source += " 1";
        }
        source += ')';
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), "Vector::i64::" + std::to_string(size));
        auto declared = Annotate("(block (var v <vec i64 " + std::to_string(size) + ">))");
        ASSERT_TRUE(declared) << declared.error().ToString();
    }
}

TEST(VectorTypeAnnotation, RejectsUnsupportedSizesDuringAnnotation) {
    for (int size : {-1, 0, 1, 3, 5, 31, 33, 64}) {
        SCOPED_TRACE(size);
        auto source = "(block (var v <vec i64 " + std::to_string(size) + ">))";
        std::istringstream input(source);
        NCore::TTokenStream tokens(input);
        auto parsed = NCore::TParser{}.Parse(tokens);
        ASSERT_TRUE(parsed) << parsed.error().ToString();
        NSemantics::TNameResolver resolver;
        ASSERT_FALSE(resolver.Resolve(*parsed));
        auto result = NTypeAnnotation::TTypeAnnotator(resolver).Annotate(*parsed);
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().ToString().find("2, 4, 8, 16 или 32"), std::string::npos);
        if (size >= 0) {
            std::string constructor = "(vec";
            for (int index = 0; index < size; ++index) {
                constructor += " 1";
            }
            constructor += ')';
            EXPECT_FALSE(Annotate(constructor));
        }
    }
}

TEST(VectorTypeAnnotation, ChecksSizesInsideCompositeTypes) {
    for (const std::string source : {
        "(block (var v <ptr <vec i64 3>>))",
        "(block (var v <array <vec i64 3> 1>))",
        "(block (var v <future <vec i64 3>>))",
        "(block (var v <struct (field <vec i64 3>)>))",
        "(block (var v <ref <vec i64 3>>))",
        "(block (var v <fun <vec i64 3> ()>))",
        "(block (type V <vec i64 3>))",
        "(block (fun f ((var v <vec i64 3>)) -> void (block)))",
        "(block (fun f () -> <vec i64 3> (block)))",
    }) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_FALSE(result);
        EXPECT_NE(result.error().ToString().find("2, 4, 8, 16 или 32"), std::string::npos)
            << result.error().ToString();
    }
}

TEST(VectorTypeAnnotation, Rejects128BitElementsDuringAnnotation) {
    for (const std::string element : {"i128", "u128"}) {
        const std::string vector = "<vec " + element + " 2>";
        for (const std::string source : {
            "(block (var v " + vector + "))",
            "(block (var v <ptr " + vector + ">))",
            "(block (var v <array " + vector + " 1>))",
            "(block (var v <future " + vector + ">))",
            "(block (var v <struct (field " + vector + ")>))",
            "(block (var v <ref " + vector + ">))",
            "(block (var v <fun " + vector + " ()>))",
            "(block (type V " + vector + "))",
            "(block (fun f ((var v " + vector + ")) -> void (block)))",
            "(block (fun f () -> " + vector + " (block)))",
            "(: (vec 1 2) " + vector + ")",
            "(vec (: 1 " + element + ") (: 2 " + element + "))",
        }) {
            SCOPED_TRACE(source);
            std::istringstream input(source);
            NCore::TTokenStream tokens(input);
            auto parsed = NCore::TParser{}.Parse(tokens);
            ASSERT_TRUE(parsed) << parsed.error().ToString();
            NSemantics::TNameResolver resolver;
            ASSERT_FALSE(resolver.Resolve(*parsed));
            auto result = NTypeAnnotation::TTypeAnnotator(resolver).Annotate(*parsed);
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().ToString().find("64 бит"), std::string::npos)
                << result.error().ToString();
        }
    }
}

TEST(BinaryTypeAnnotation, RejectsPromotionTo128BitVectorElements) {
    for (const std::string scalar : {"i128", "u128"}) {
        const std::string vector = scalar == "i128"
            ? "<vec i64 2>"
            : "<vec u64 2>";
        for (const std::string op : {"+", "*", "==", "<"}) {
            for (bool vectorLeft : {false, true}) {
                SCOPED_TRACE(scalar + " " + op + " " + std::to_string(vectorLeft));
                auto result = AnnotateBinary(op,
                    vectorLeft ? vector : scalar,
                    vectorLeft ? scalar : vector);
                ASSERT_FALSE(result);
                EXPECT_NE(result.error().ToString().find("64 бит"), std::string::npos)
                    << result.error().ToString();
            }
        }
    }
}

TEST(VectorTypeAnnotation, AllowsScalar128BitValuesConvertedToFloatElements) {
    for (const std::string scalar : {"i128", "u128"}) {
        auto constructed = Annotate("(block (var a " + scalar
            + ") (var v = (: (vec a a) <vec f64 2>)))");
        ASSERT_TRUE(constructed) << constructed.error().ToString();
        auto arithmetic = AnnotateBinary("+", "<vec f64 2>", scalar);
        ASSERT_TRUE(arithmetic) << arithmetic.error().ToString();
        EXPECT_EQ(TypeName((*arithmetic)->Type), "Vector::Float::2");
        auto division = AnnotateBinary("/", scalar == "i128" ? "<vec i64 2>" : "<vec u64 2>", scalar);
        ASSERT_TRUE(division) << division.error().ToString();
        EXPECT_EQ(TypeName((*division)->Type), "Vector::Float::2");
        auto scalarArithmetic = AnnotateBinary("+", scalar, scalar);
        ASSERT_TRUE(scalarArithmetic) << scalarArithmetic.error().ToString();
        EXPECT_EQ(TypeName((*scalarArithmetic)->Type), scalar);
    }
}

TEST(VectorTypeAnnotation, GenericVectorBindsTypeAndSizeAndCachesInstances) {
    auto result = Annotate(
        "(block (pragma language overloads)"
        " (fun size [T (const N i32)] ((var v <ref <vec T N>>)) -> i32 (block (return N)))"
        " (var a = (: (vec 1 2) <vec i8 2>)) (var b = (vec 1 2 3 4))"
        " (var x = (call size a)) (var y = (call size a)) (var z = (call size b)))");
    ASSERT_TRUE(result) << result.error().ToString();
    int instances = 0;
    for (const auto& stmt : TMaybeNode<TBlockExpr>(*result).Cast()->Stmts) {
        auto function = TMaybeNode<TFunDecl>(stmt).Cast();
        if (!function || function->OriginalName != "size") {
            continue;
        }
        ++instances;
        auto vector = TMaybeType<TVectorType>(UnwrapReferenceType(function->Params[0]->Type)).Cast();
        ASSERT_NE(vector, nullptr);
        EXPECT_TRUE(vector->SizeParam.empty());
        EXPECT_TRUE(vector->Size == 2 || vector->Size == 4);
        auto body = TMaybeNode<TBlockExpr>(function->Body).Cast();
        auto returned = TMaybeNode<TReturnExpr>(body->Stmts.back()).Cast();
        ASSERT_NE(returned, nullptr);
        auto number = TMaybeNode<TNumberExpr>(returned->Value).Cast();
        ASSERT_NE(number, nullptr);
        EXPECT_EQ(number->IntValue, vector->Size);
        EXPECT_EQ(TypeName(number->Type), "i32");
    }
    EXPECT_EQ(instances, 2);
    NCore::TPrintOptions options;
    options.Pretty = false;
    auto reparsed = Annotate(NCore::PrintAst(*result, options));
    ASSERT_TRUE(reparsed) << reparsed.error().ToString();
}

TEST(VectorTypeAnnotation, GenericVectorRejectsConflictingBindingsAndInvalidElements) {
    for (const std::string source : {
        "(block (pragma language overloads)"
        " (fun same [T (const N i64)] ((var a <ref <vec T N>>) (var b <ref <vec T N>>))"
        " -> void (block)) (var a = (vec 1 2)) (var b = (vec 1 2 3 4)) (call same a b))",
        "(block (pragma language overloads)"
        " (fun same [T (const N i64)] ((var a <ref <vec T N>>) (var b <ref <vec T N>>))"
        " -> void (block)) (var a = (vec 1 2)) (var b = (vec #t #f)) (call same a b))",
        "(block (var v <vec string 2>))",
        "(block (var v <vec i64 N>))",
        "(block (pragma language overloads) (fun f [T] ((var v <vec T T>)) -> void (block)))",
        "(block (type V [T (const N i64)] <vec T N>) (var v <named V [i128 2]>))",
        "(block (type V [T (const N i64)] <vec T N>) (var v <named V [i64 3]>))",
        "(block (var v <vec i64 4294967298>))",
    }) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_FALSE(result);
    }
}

TEST(VectorTypeAnnotation, VectorIndexInfersScalarElementType) {
    for (const auto& [source, type] : std::vector<std::pair<std::string, std::string>>{
        {"(index (vec 1 2) 0)", "i64"},
        {"(index (: (vec -1 127) <vec i8 2>) (: 1 i32))", "i8"},
        {"(index (vec #t #f) 1)", "Bool"},
        {"(index (vec 1.0 2.0) 0)", "Float"},
    }) {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), type);
    }
    EXPECT_FALSE(Annotate("(index (vec 1 2) \"first\")"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(VectorTypeAnnotation, IntrinsicsKeepCallsAndInferResults) {
    for (const auto& [source, type] : std::vector<std::pair<std::string, std::string>>{
        {"(call builtin::all (vec #t #f))", "Bool"},
        {"(call builtin::any (vec #t #f))", "Bool"},
        {"(call builtin::shuffle (: (vec 1 2) <vec i8 2>) (vec 1 0))", "Vector::i8::2"},
        {"(call builtin::shuffle (vec 1.5 2.5) (vec 0 0))", "Vector::Float::2"},
        {"(call builtin::movemask (vec #t #f))", "u32"},
        {"(call builtin::select (vec #t #f) (vec 1 2) (vec 3 4))", "Vector::i64::2"},
        {"(call builtin::gather (cast 0 <ptr f64>) (: (vec 0 1) <vec u8 2>))", "Vector::Float::2"},
        {"(call builtin::prefetch (cast 0 <ptr i8>))", "Void"}})
    {
        SCOPED_TRACE(source);
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        ASSERT_TRUE(TMaybeNode<TCallExpr>(*result));
        EXPECT_EQ(TypeName((*result)->Type), type);
        auto repeated = Annotate(NCore::PrintAst(*result));
        ASSERT_TRUE(repeated) << repeated.error().ToString();
        EXPECT_EQ(TypeName((*repeated)->Type), type);
    }
    for (const auto& [name, builtinCall] : std::vector<std::pair<std::string, std::string>>{
        {"all", "(call builtin::all (vec #t #f))"},
        {"any", "(call builtin::any (vec #t #f))"},
        {"shuffle", "(call builtin::shuffle (vec 1 2) (vec 1 0))"}})
    {
        SCOPED_TRACE(name);
        auto result = Annotate("(block (fun " + name + " ((var x i64)) -> i64 (block x)) "
            "(var intrinsic = " + builtinCall + ") (call " + name + " 5))");
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), "i64");
    }
}

TEST(VectorTypeAnnotation, RejectsInvalidIntrinsicArguments) {
    for (const auto& source : {
        "(call all (vec #t #f))", "(call any (vec #t #f))",
        "(call shuffle (vec 1 2) (vec 1 0))",
        "(call builtin::all)", "(call builtin::any (vec #t #f) #t)", "(call builtin::all 1)",
        "(call builtin::any (vec 1 2))", "(call builtin::shuffle (vec 1 2))",
        "(call builtin::shuffle (vec 1 2) (vec 0 1 0 1))",
        "(call builtin::shuffle (vec 1 2) (vec -1 0))",
        "(call builtin::shuffle (vec 1 2) (vec 0 2))",
        "(call builtin::shuffle (vec 1 2) (vec 0.0 1.0))",
        "(call builtin::movemask (vec 1 2))", "(call builtin::movemask)",
        "(call builtin::select (vec 1 2) (vec 1 2) (vec 3 4))",
        "(call builtin::select (vec #t #f) (vec 1 2) (vec 1.0 2.0))",
        "(call builtin::select (vec #t #f) (vec 1 2 3 4) (vec 1 2 3 4))",
        "(call builtin::select (vec #t #f) 1 2)",
        "(call builtin::gather 1 (vec 0 1))",
        "(call builtin::gather (cast 0 <ptr i128>) (vec 0 1))",
        "(call builtin::gather (cast 0 <ptr <ptr i8>>) (vec 0 1))",
        "(call builtin::gather (cast 0 <ptr i8>) (vec #t #f))",
        "(call builtin::prefetch 0)", "(call builtin::prefetch)"})
    {
        SCOPED_TRACE(source);
        EXPECT_FALSE(Annotate(source));
    }
}

TEST(VectorTypeAnnotation, ShuffleAcceptsDynamicIntegerMasks) {
    for (const auto& source : {
        "(block (var i = 0) (call builtin::shuffle (vec 1 2) (vec i 1)))",
        "(block (var mask = (: (vec 0 1) <vec u8 2>)) (call builtin::shuffle (vec 1 2) mask))"})
    {
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), "Vector::i64::2");
        EXPECT_TRUE(Annotate(NCore::PrintAst(*result)));
    }
}

TEST(VectorTypeAnnotation, UnaryMasksPreserveShapeAndCheckElements) {
    for (const auto& [source, type] : std::vector<std::pair<std::string, std::string>>{
        {"(! (vec #t #f))", "Vector::Bool::2"},
        {"(! (: (vec 0 2) <vec i8 2>))", "Vector::Bool::2"},
        {"(! (vec 0.0 0.5))", "Vector::Bool::2"},
        {"(~ (: (vec 0 255) <vec u8 2>))", "Vector::u8::2"}})
    {
        auto result = Annotate(source);
        ASSERT_TRUE(result) << result.error().ToString();
        EXPECT_EQ(TypeName((*result)->Type), type);
    }
    EXPECT_FALSE(Annotate("(~ (vec #t #f))"));
    EXPECT_FALSE(Annotate("(~ (vec 0.0 1.0))"));
}

TEST(VectorTypeAnnotation, CastsPreserveLaneCount) {
    for (const auto& source : {
        "(cast (vec 1 2) <vec i32 4>)",
        "(cast (vec 1 2 3 4) <vec f64 2>)",
        "(cast (vec 1 2) i64)",
        "(cast 1 <vec i64 2>)"})
    {
        EXPECT_FALSE(Annotate(source)) << source;
    }
    EXPECT_TRUE(Annotate("(cast (vec 1 2) <vec f64 2>)"));
}
