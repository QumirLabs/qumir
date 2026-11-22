#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/parser/core/printer.h>
#include <qumir/semantics/lifetime/pass.h>
#include <qumir/semantics/lifetime/synthetic_name_generator.h>
#include <qumir/semantics/transform/transform.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

TIntrusivePtr<TBlockExpr> Block(std::vector<TExprPtr> statements) {
    return MakeIntrusive<TBlockExpr>(TLocation{}, std::move(statements));
}

TIntrusivePtr<TVarStmt> Variable(std::string name, const TTypePtr& type) {
    return MakeIntrusive<TVarStmt>(TLocation{}, std::move(name), type);
}

TIntrusivePtr<TIdentExpr> Ident(std::string name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, std::move(name));
}

TIntrusivePtr<TStringLiteralExpr> Literal(std::string value) {
    return MakeIntrusive<TStringLiteralExpr>(TLocation{}, std::move(value), NQumir::NTest::TestTypes());
}

TIntrusivePtr<TCallExpr> Call(
    std::string name,
    std::vector<TExprPtr> arguments = {})
{
    return MakeIntrusive<TCallExpr>(
        TLocation{},
        Ident(std::move(name)),
        std::move(arguments));
}

TIntrusivePtr<TFunDecl> Function(
    std::string name,
    std::vector<TParam> parameters,
    const TIntrusivePtr<TBlockExpr>& body,
    const TTypePtr& returnType = NQumir::NTest::TestTypes().Make<TVoidType>())
{
    return MakeIntrusive<TFunDecl>(
        TLocation{},
        std::move(name),
        std::vector<TGenericParam>{},
        std::move(parameters),
        body,
        returnType);
}

std::string Print(const TExprPtr& expr) {
    NAst::NCore::TPrintOptions options;
    options.Pretty = false;
    return NAst::NCore::PrintAst(expr, options);
}

} // namespace

TEST(LifetimePass, RewritesAllStringAssignmentTargets) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto make = Function(
        "make",
        {},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Literal("owned"))}),
        stringType);

    auto a = Variable("a", stringType);
    auto b = Variable("b", stringType);
    auto arrayType = NQumir::NTest::TestTypes().MakeArray(stringType, 1);
    auto items = Variable("items", arrayType);
    auto structType = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"text", stringType},
        });
    auto object = Variable("object", structType);
    auto body = Block({
        a,
        b,
        MakeIntrusive<TAssignExpr>(TLocation{}, "a", Literal("literal")),
        MakeIntrusive<TAssignExpr>(TLocation{}, "a", Ident("b")),
        MakeIntrusive<TAssignExpr>(TLocation{}, "a", Ident("a")),
        MakeIntrusive<TAssignExpr>(
            TLocation{},
            "a",
            MakeIntrusive<TCallExpr>(TLocation{}, Ident("make"), std::vector<TExprPtr>{})),
        items,
        MakeIntrusive<TArrayAssignExpr>(
            TLocation{},
            "items",
            std::vector<TExprPtr>{
                MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
            },
            Literal("element")),
        object,
        MakeIntrusive<TFieldAssignExpr>(
            TLocation{},
            Ident("object"),
            "text",
            Ident("b")),
    });
    auto rewrite = Function("rewrite", {}, body);

    auto refParam = Variable(
        "result",
        NQumir::NTest::TestTypes().MakeReference(stringType));
    auto refBody = Block({
        MakeIntrusive<TAssignExpr>(TLocation{}, "result", Literal("reference")),
    });
    auto writeRef = Function("write_ref", {refParam}, refBody);

    TExprPtr root = Block({make, rewrite, writeRef});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());
    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 12u);
    EXPECT_EQ(Print(a->Init), "(bitcast 0 string)");
    EXPECT_EQ(Print(b->Init), "(bitcast 0 string)");
    EXPECT_EQ(Print(body->Stmts[2]), "(replace a (own-literal \"literal\"))");
    EXPECT_EQ(Print(body->Stmts[3]), "(replace a (retain (borrow b)))");
    EXPECT_EQ(Print(body->Stmts[4]), "(replace a (retain (borrow a)))");
    EXPECT_EQ(Print(body->Stmts[5]), "(replace a (move (call make)))");
    EXPECT_EQ(
        Print(body->Stmts[8]),
        "(replace (index items 1) (own-literal \"element\"))");
    EXPECT_EQ(
        Print(body->Stmts[10]),
        "(replace (field object text) (retain (borrow b)))");
    EXPECT_EQ(
        Print(refBody->Stmts[0]),
        "(replace result (own-literal \"reference\"))");
}

TEST(LifetimePass, SplitsStringDeclarationInitializerAfterNullInitialization) {
    auto value = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    value->Init = Literal("initial");
    auto body = Block({value});
    TExprPtr root = Block({Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 3u);
    EXPECT_EQ(Print(value->Init), "(bitcast 0 string)");
    EXPECT_EQ(
        Print(body->Stmts[1]),
        "(replace value (own-literal \"initial\"))");
}

TEST(LifetimePass, DestroysTwoOwnedCallArgumentsInReverseOrder) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto makeFirst = Function(
        "make_first",
        {},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Literal("first"))}),
        stringType);
    auto makeSecond = Function(
        "make_second",
        {},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Literal("second"))}),
        stringType);
    auto consume = Function(
        "consume",
        {Variable("first", stringType), Variable("second", stringType)},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, nullptr)}));
    auto body = Block({Call("consume", {
        Call("make_first"),
        Call("make_second"),
    })});
    TExprPtr root = Block({
        makeFirst,
        makeSecond,
        consume,
        Function("main", {}, body),
    });
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 2u);
    EXPECT_EQ(
        Print(body->Stmts[0]),
        "(block (var __lifetime_2 = (move (call make_first))) "
        "(var __lifetime_3 = (move (call make_second))) "
        "(call consume (borrow __lifetime_2) (borrow __lifetime_3)) "
        "(destroy __lifetime_3) (destroy __lifetime_2))");
}

TEST(LifetimePass, PreservesNestedCallResultPastArgumentCleanup) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto make = Function(
        "make",
        {},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Literal("value"))}),
        stringType);
    auto wrap = Function(
        "wrap",
        {Variable("value", stringType)},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Ident("value"))}),
        stringType);
    auto result = Variable("result", stringType);
    auto body = Block({
        result,
        MakeIntrusive<TAssignExpr>(
            TLocation{},
            "result",
            Call("wrap", {Call("make")})),
    });
    TExprPtr root = Block({make, wrap, Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 3u);
    EXPECT_EQ(
        Print(body->Stmts[1]),
        "(replace result (move (block "
        "(var __lifetime_2 = (move (call make))) "
        "(var __lifetime_3 = (call wrap (borrow __lifetime_2))) "
        "(destroy __lifetime_2) (move __lifetime_3))))");
}

TEST(LifetimePass, MaterializesLiteralOnlyWhenCallAbiRequiresIt) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto raw = Function(
        "raw",
        {Variable("value", stringType)},
        Block({}),
        NQumir::NTest::TestTypes().Make<TVoidType>());
    raw->MangledName = "raw";
    auto managed = Function(
        "managed",
        {Variable("value", stringType)},
        Block({}),
        NQumir::NTest::TestTypes().Make<TVoidType>());
    managed->MangledName = "managed";
    managed->RequireArgsMaterialization = true;
    auto body = Block({
        Call("raw", {Literal("raw")}),
        Call("managed", {Literal("managed")}),
        Call("make_unused"),
    });
    auto makeUnused = Function(
        "make_unused",
        {},
        Block({MakeIntrusive<TReturnExpr>(TLocation{}, Literal("unused"))}),
        stringType);
    TExprPtr root = Block({
        raw,
        managed,
        makeUnused,
        Function("main", {}, body),
    });
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 4u);
    EXPECT_EQ(Print(body->Stmts[0]), "(call raw \"raw\")");
    EXPECT_EQ(
        Print(body->Stmts[1]),
        "(block (var __lifetime_1 = (own-literal \"managed\")) "
        "(call managed (borrow __lifetime_1)) (destroy __lifetime_1))");
    EXPECT_EQ(
        Print(body->Stmts[2]),
        "(destroy (move (call make_unused)))");
}

TEST(LifetimePass, CleansNestedStringLocalsOnReturnInLifoOrder) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto first = Variable("first", stringType);
    first->Init = Literal("first");
    auto second = Variable("second", stringType);
    second->Init = Literal("second");
    auto nested = Block({
        second,
        MakeIntrusive<TReturnExpr>(TLocation{}, Ident("first")),
    });
    auto body = Block({first, nested});
    TExprPtr root = Block({Function("value", {}, body, stringType)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    EXPECT_EQ(
        Print(nested->Stmts.back()),
        "(block (var __lifetime_0 = (retain (borrow first))) "
        "(cleanup-exit (return (move __lifetime_0)) "
        "(destroy second) (destroy first)))");
    EXPECT_FALSE(TMaybeNode<TDestroyExpr>(body->Stmts.back()));
}

TEST(LifetimePass, LoopExitCleansOnlyLocalsDeclaredBeforeTheExit) {
    auto condition = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes());
    condition->Type = NQumir::NTest::TestTypes().Make<TBoolType>();
    auto loopCondition = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes());
    loopCondition->Type = NQumir::NTest::TestTypes().Make<TBoolType>();
    auto value = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    value->Init = Literal("iteration");
    auto items = Variable(
        "items",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().MakeInteger(), 1));
    items->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
    });
    auto earlyBreak = Block({MakeIntrusive<TBreakStmt>(TLocation{})});
    auto loopBody = Block({
        MakeIntrusive<TIfExpr>(
            TLocation{},
            condition,
            earlyBreak,
            Block({})),
        value,
        items,
        MakeIntrusive<TContinueStmt>(TLocation{}),
    });
    auto loop = MakeIntrusive<TWhileStmtExpr>(
        TLocation{},
        loopCondition,
        loopBody);
    auto body = Block({loop});
    TExprPtr root = Block({Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(earlyBreak->Stmts.size(), 1u);
    EXPECT_EQ(Print(earlyBreak->Stmts[0]), "(cleanup-exit (break))");
    EXPECT_EQ(
        Print(loopBody->Stmts.back()),
        "(cleanup-exit (continue) (destroy items) (destroy value))");
}

TEST(LifetimePass, DestroysLocalArraysOnNormalAndReturnExits) {
    auto i64 = NQumir::NTest::TestTypes().MakeInteger();
    auto plain = Variable("plain", NQumir::NTest::TestTypes().MakeArray(i64, 1));
    plain->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{3}, NQumir::NTest::TestTypes()),
    });
    auto marker = Variable("marker", i64);
    auto nested = Block({
        plain,
        marker,
        MakeIntrusive<TAssignExpr>(
            TLocation{},
            "marker",
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQumir::NTest::TestTypes())),
    });

    auto strings = Variable(
        "strings",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 1));
    strings->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{4}, NQumir::NTest::TestTypes()),
    });
    auto body = Block({nested, strings});
    TExprPtr root = Block({Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    EXPECT_EQ(
        Print(nested),
        "(block (var plain <array i64 1> [1 3]) (var marker i64) "
        "(= marker 0) (destroy plain))");
    ASSERT_EQ(body->Stmts.size(), 4u);
    EXPECT_EQ(
        Print(body->Stmts[3]),
        "(cleanup-exit (return) (destroy strings __lifetime_0))");
}

TEST(LifetimePass, SavesStringArrayAllocationSizeForMultidimensionalBounds) {
    auto i64 = NQumir::NTest::TestTypes().MakeInteger();
    auto lower = Variable("lower", i64);
    lower->Init = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes());
    auto upper = Variable("upper", i64);
    upper->Init = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes());
    auto grid = Variable(
        "grid",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 2));
    grid->Bounds.push_back({Ident("lower"), Ident("upper")});
    grid->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
    });
    auto body = Block({
        lower,
        upper,
        grid,
        MakeIntrusive<TAssignExpr>(
            TLocation{},
            "upper",
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{100}, NQumir::NTest::TestTypes())),
    });
    TExprPtr root = Block({Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    ASSERT_EQ(body->Stmts.size(), 6u);
    EXPECT_EQ(
        Print(body->Stmts[3]),
        "(var __lifetime_0 = (* (* 1 (+ (- upper lower) 1)) (+ (- 2 0) 1)))");
    EXPECT_EQ(
        Print(body->Stmts[5]),
        "(cleanup-exit (return) (destroy grid __lifetime_0))");
}

TEST(LifetimePass, DoesNotDestroyArrayParameter) {
    auto arrayType = NQumir::NTest::TestTypes().MakeArray(
        NQumir::NTest::TestTypes().Make<TStringType>(),
        1);
    auto parameter = Variable("items", arrayType);
    parameter->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
    });
    auto body = Block({});
    TExprPtr root = Block({Function("consume", {parameter}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    EXPECT_EQ(Print(body), "(block (cleanup-exit (return)))");
}

TEST(LifetimePass, EvaluatesEffectfulStringArrayBoundOnce) {
    auto bound = Function(
        "bound",
        {},
        Block({MakeIntrusive<TReturnExpr>(
            TLocation{},
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()))}),
        NQumir::NTest::TestTypes().MakeInteger());
    auto items = Variable(
        "items",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 1));
    items->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        Call("bound"),
    });
    auto body = Block({items});
    TExprPtr root = Block({bound, Function("main", {}, body)});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    const auto printed = Print(body);
    const auto firstCall = printed.find("(call bound)");
    ASSERT_NE(firstCall, std::string::npos);
    EXPECT_EQ(printed.find("(call bound)", firstCall + 1), std::string::npos);
    EXPECT_NE(
        printed.find("(var items <array string 1> [1 __lifetime_0])"),
        std::string::npos);
    EXPECT_NE(
        printed.find("(destroy items __lifetime_1)"),
        std::string::npos);
}

TEST(LifetimePass, CollectsGlobalCleanupOnceInReverseDeclarationOrder) {
    auto first = Variable("first", NQumir::NTest::TestTypes().Make<TStringType>());
    first->Init = Literal("first");
    auto plain = Variable(
        "plain",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().MakeInteger(), 1));
    plain->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{2}, NQumir::NTest::TestTypes()),
    });
    auto words = Variable(
        "words",
        NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 1));
    words->Bounds.push_back({
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{3}, NQumir::NTest::TestTypes()),
    });
    TExprPtr root = Block({
        first,
        plain,
        words,
        Function("main", {}, Block({})),
    });
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();

    auto rootBlock = TMaybeNode<TBlockExpr>(root).Cast();
    size_t cleanupCount = 0;
    TExprPtr cleanup;
    for (const auto& statement : rootBlock->Stmts) {
        if (TMaybeNode<TGlobalCleanupExpr>(statement)) {
            ++cleanupCount;
            cleanup = statement;
        }
    }
    EXPECT_EQ(cleanupCount, 1u);
    ASSERT_TRUE(cleanup);
    EXPECT_EQ(
        Print(cleanup),
        "(cleanup-global (destroy words __lifetime_0) "
        "(destroy plain) (destroy first))");
}

TEST(LifetimePass, DoesNotRewriteFinalizedLifetimeAstTwice) {
    auto value = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    value->Init = Literal("value");
    TExprPtr root = Block({Function("main", {}, Block({value}))});
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());

    auto source = NTransform::RunSourceTransformFixpoint(root, resolver);
    ASSERT_TRUE(source.has_value()) << source.error().ToString();
    auto final = NTransform::RunFinalSemanticPipeline(root, resolver);
    ASSERT_TRUE(final.has_value()) << final.error().ToString();
    const auto before = Print(root);

    NSemantics::TSyntheticNameGenerator syntheticNames(resolver, root);
    auto secondRun = NSemantics::LifetimePass(root, resolver, syntheticNames);
    ASSERT_TRUE(secondRun.has_value()) << secondRun.error().ToString();
    EXPECT_FALSE(*secondRun);
    EXPECT_EQ(Print(root), before);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
