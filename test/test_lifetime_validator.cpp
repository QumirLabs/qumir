#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/semantics/lifetime/validator.h>
#include <qumir/semantics/transform/transform.h>

#include <memory>
#include <string>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;
using namespace NQumir::NSemantics;

namespace {

TIntrusivePtr<TBlockExpr> Block(std::vector<TExprPtr> statements) {
    return MakeIntrusive<TBlockExpr>(TLocation{}, std::move(statements));
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

TIntrusivePtr<TVarStmt> Variable(std::string name, const TTypePtr& type) {
    return MakeIntrusive<TVarStmt>(TLocation{}, std::move(name), type);
}

TIntrusivePtr<TIdentExpr> Ident(std::string name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, std::move(name));
}

TIntrusivePtr<TOwnLiteralExpr> OwnedLiteral(std::string value = "value") {
    return MakeIntrusive<TOwnLiteralExpr>(
        TLocation{},
        MakeIntrusive<TStringLiteralExpr>(TLocation{}, std::move(value), NQumir::NTest::TestTypes()));
}

std::expected<void, TError> Prepare(
    TExprPtr& root,
    TNameResolver& resolver)
{
    if (auto error = resolver.Resolve(root)) {
        return std::unexpected(*error);
    }
    auto annotation = NTransform::FinalTypeAnnotation(root, resolver);
    if (!annotation) {
        return std::unexpected(annotation.error());
    }
    return {};
}

std::expected<void, TError> PrepareAndValidate(
    TExprPtr& root,
    TNameResolver& resolver)
{
    if (auto result = Prepare(root, resolver); !result) {
        return result;
    }
    return TLifetimeValidator(resolver).Validate(root);
}

void ExpectErrorContains(
    const std::expected<void, TError>& result,
    std::string_view expected)
{
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().ToString().find(expected), std::string::npos)
        << result.error().ToString();
}

} // namespace

TEST(LifetimeAnnotation, AnnotatesAllInternalNodesExplicitly) {
    auto stringType = NQumir::NTest::TestTypes().Make<TStringType>();
    auto value = Variable("value", stringType);
    auto borrowed = MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("value"));
    auto retained = MakeIntrusive<TRetainExpr>(TLocation{}, borrowed);
    auto moved = MakeIntrusive<TMoveExpr>(TLocation{}, OwnedLiteral("moved"));
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("value"),
        OwnedLiteral("replacement"), NQumir::NTest::TestTypes());
    auto destroyRetained = MakeIntrusive<TDestroyExpr>(TLocation{}, retained, NQumir::NTest::TestTypes());
    auto destroyMoved = MakeIntrusive<TDestroyExpr>(TLocation{}, moved, NQumir::NTest::TestTypes());
    auto exit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        nullptr,
        std::vector<TExprPtr>{
            MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value"), NQumir::NTest::TestTypes()),
        }, NQumir::NTest::TestTypes());
    auto global = MakeIntrusive<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{}, NQumir::NTest::TestTypes());
    TExprPtr root = Block({
        Function("main", {}, Block({
            value,
            destroyRetained,
            destroyMoved,
            replace,
            exit,
        })),
        global,
    });
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = Prepare(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
    EXPECT_TRUE(TMaybeType<TStringType>(borrowed->Type));
    EXPECT_TRUE(TMaybeType<TStringType>(retained->Type));
    EXPECT_TRUE(TMaybeType<TStringType>(moved->Type));
    EXPECT_TRUE(TMaybeType<TVoidType>(destroyRetained->Type));
    EXPECT_TRUE(TMaybeType<TVoidType>(replace->Type));
    EXPECT_TRUE(TMaybeType<TVoidType>(exit->Type));
    EXPECT_TRUE(TMaybeType<TVoidType>(global->Type));
}

TEST(LifetimeAnnotation, CleanupReturnChecksFunctionReturnType) {
    auto exit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        MakeIntrusive<TStringLiteralExpr>(TLocation{}, "wrong", NQumir::NTest::TestTypes()),
        std::vector<TExprPtr>{}, NQumir::NTest::TestTypes());
    TExprPtr root = Block({
        Function(
            "value",
            {},
            Block({exit}),
            NQumir::NTest::TestTypes().MakeInteger()),
    });
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = Prepare(root, resolver);
    ExpectErrorContains(result, "возвращаемому типу функции");
}

TEST(LifetimeValidator, AcceptsValidOwnershipGraph) {
    auto value = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    value->Init() = OwnedLiteral("initial");
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("value"),
        MakeIntrusive<TRetainExpr>(
            TLocation{},
            MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("value"))), NQumir::NTest::TestTypes());
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value"), NQumir::NTest::TestTypes());
    TExprPtr root = Block({Function("main", {}, Block({value, replace, destroy}))});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
}

TEST(LifetimeValidator, RejectsRetainOfTrivialValue) {
    auto retain = MakeIntrusive<TRetainExpr>(
        TLocation{},
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()));
    TExprPtr root = Block({retain});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "ref-counted");
}

TEST(LifetimeValidator, RejectsRetainOfUniqueValue) {
    auto arrayType = NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().MakeInteger(), 1);
    auto array = Variable("items", arrayType);
    auto retain = MakeIntrusive<TRetainExpr>(
        TLocation{},
        MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("items")));
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, retain, NQumir::NTest::TestTypes());
    TExprPtr root = Block({Function("main", {}, Block({array, destroy}))});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "ref-counted");
}

TEST(LifetimeValidator, RejectsDestroyOfTrivialValue) {
    auto destroy = MakeIntrusive<TDestroyExpr>(
        TLocation{},
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}, NQumir::NTest::TestTypes()), NQumir::NTest::TestTypes());
    TExprPtr root = Block({destroy});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "managed value");
}

TEST(LifetimeValidator, RejectsDestroyOfBorrowedParameter) {
    auto parameter = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value"), NQumir::NTest::TestTypes());
    TExprPtr root = Block({Function("main", {parameter}, Block({destroy}))});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "borrowed value");
}

TEST(LifetimeValidator, RejectsCopyOfUniqueValue) {
    auto arrayType = NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().MakeInteger(), 1);
    auto source = Variable("source", arrayType);
    auto target = Variable("target", arrayType);
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("target"),
        Ident("source"), NQumir::NTest::TestTypes());
    TExprPtr root = Block({Function("main", {}, Block({source, target, replace}))});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Unique value cannot be copied");
}

TEST(LifetimeValidator, RejectsUnresolvedSyntheticVariable) {
    auto ident = Ident("__lifetime_missing");
    ident->Type = NQumir::NTest::TestTypes().Make<TStringType>();
    TExprPtr root = Block({ident});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = TLifetimeValidator(resolver).Validate(root);
    ExpectErrorContains(result, "Unresolved synthetic lifetime variable");
}

TEST(LifetimeValidator, RejectsRawLiteralInOwnedStorage) {
    auto value = Variable("value", NQumir::NTest::TestTypes().Make<TStringType>());
    value->Init() = MakeIntrusive<TStringLiteralExpr>(TLocation{}, "raw", NQumir::NTest::TestTypes());
    auto marker = MakeIntrusive<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{}, NQumir::NTest::TestTypes());
    TExprPtr root = Block({value, marker});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Raw string literal requires own-literal");
}

TEST(LifetimeValidator, RejectsUnconsumedOwnedResult) {
    TExprPtr root = Block({OwnedLiteral("unused")});
    TNameResolver resolver(NQumir::NTest::TestTypes());

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Owned result has no move/destroy consumer");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
