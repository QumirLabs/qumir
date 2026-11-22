#include <gtest/gtest.h>

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
    TIntrusivePtr<TBlockExpr> body,
    TTypePtr returnType = std::make_shared<TVoidType>())
{
    return MakeIntrusive<TFunDecl>(
        TLocation{},
        std::move(name),
        std::vector<TGenericParam>{},
        std::move(parameters),
        std::move(body),
        std::move(returnType));
}

TIntrusivePtr<TVarStmt> Variable(std::string name, TTypePtr type) {
    return MakeIntrusive<TVarStmt>(TLocation{}, std::move(name), std::move(type));
}

TIntrusivePtr<TIdentExpr> Ident(std::string name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, std::move(name));
}

TIntrusivePtr<TOwnLiteralExpr> OwnedLiteral(std::string value = "value") {
    return MakeIntrusive<TOwnLiteralExpr>(
        TLocation{},
        MakeIntrusive<TStringLiteralExpr>(TLocation{}, std::move(value)));
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
    auto stringType = std::make_shared<TStringType>();
    auto value = Variable("value", stringType);
    auto borrowed = MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("value"));
    auto retained = MakeIntrusive<TRetainExpr>(TLocation{}, borrowed);
    auto moved = MakeIntrusive<TMoveExpr>(TLocation{}, OwnedLiteral("moved"));
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("value"),
        OwnedLiteral("replacement"));
    auto destroyRetained = MakeIntrusive<TDestroyExpr>(TLocation{}, retained);
    auto destroyMoved = MakeIntrusive<TDestroyExpr>(TLocation{}, moved);
    auto exit = MakeIntrusive<TCleanupExitExpr>(
        TLocation{},
        ECleanupExitKind::Return,
        nullptr,
        std::vector<TExprPtr>{
            MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value")),
        });
    auto global = MakeIntrusive<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{});
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
    TNameResolver resolver;

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
        MakeIntrusive<TStringLiteralExpr>(TLocation{}, "wrong"),
        std::vector<TExprPtr>{});
    TExprPtr root = Block({
        Function(
            "value",
            {},
            Block({exit}),
            std::make_shared<TIntegerType>()),
    });
    TNameResolver resolver;

    auto result = Prepare(root, resolver);
    ExpectErrorContains(result, "возвращаемому типу функции");
}

TEST(LifetimeValidator, AcceptsValidOwnershipGraph) {
    auto value = Variable("value", std::make_shared<TStringType>());
    value->Init = OwnedLiteral("initial");
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("value"),
        MakeIntrusive<TRetainExpr>(
            TLocation{},
            MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("value"))));
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value"));
    TExprPtr root = Block({Function("main", {}, Block({value, replace, destroy}))});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ASSERT_TRUE(result.has_value()) << result.error().ToString();
}

TEST(LifetimeValidator, RejectsRetainOfTrivialValue) {
    auto retain = MakeIntrusive<TRetainExpr>(
        TLocation{},
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}));
    TExprPtr root = Block({retain});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "ref-counted");
}

TEST(LifetimeValidator, RejectsRetainOfUniqueValue) {
    auto arrayType = std::make_shared<TArrayType>(std::make_shared<TIntegerType>(), 1);
    auto array = Variable("items", arrayType);
    auto retain = MakeIntrusive<TRetainExpr>(
        TLocation{},
        MakeIntrusive<TBorrowExpr>(TLocation{}, Ident("items")));
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, retain);
    TExprPtr root = Block({Function("main", {}, Block({array, destroy}))});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "ref-counted");
}

TEST(LifetimeValidator, RejectsDestroyOfTrivialValue) {
    auto destroy = MakeIntrusive<TDestroyExpr>(
        TLocation{},
        MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}));
    TExprPtr root = Block({destroy});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "managed value");
}

TEST(LifetimeValidator, RejectsDestroyOfBorrowedParameter) {
    auto parameter = Variable("value", std::make_shared<TStringType>());
    auto destroy = MakeIntrusive<TDestroyExpr>(TLocation{}, Ident("value"));
    TExprPtr root = Block({Function("main", {parameter}, Block({destroy}))});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "borrowed value");
}

TEST(LifetimeValidator, RejectsCopyOfUniqueValue) {
    auto arrayType = std::make_shared<TArrayType>(std::make_shared<TIntegerType>(), 1);
    auto source = Variable("source", arrayType);
    auto target = Variable("target", arrayType);
    auto replace = MakeIntrusive<TReplaceExpr>(
        TLocation{},
        Ident("target"),
        Ident("source"));
    TExprPtr root = Block({Function("main", {}, Block({source, target, replace}))});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Unique value cannot be copied");
}

TEST(LifetimeValidator, RejectsUnresolvedSyntheticVariable) {
    auto ident = Ident("__lifetime_missing");
    ident->Type = std::make_shared<TStringType>();
    TExprPtr root = Block({ident});
    TNameResolver resolver;

    auto result = TLifetimeValidator(resolver).Validate(root);
    ExpectErrorContains(result, "Unresolved synthetic lifetime variable");
}

TEST(LifetimeValidator, RejectsRawLiteralInOwnedStorage) {
    auto value = Variable("value", std::make_shared<TStringType>());
    value->Init = MakeIntrusive<TStringLiteralExpr>(TLocation{}, "raw");
    auto marker = MakeIntrusive<TGlobalCleanupExpr>(
        TLocation{},
        std::vector<TExprPtr>{});
    TExprPtr root = Block({value, marker});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Raw string literal requires own-literal");
}

TEST(LifetimeValidator, RejectsUnconsumedOwnedResult) {
    TExprPtr root = Block({OwnedLiteral("unused")});
    TNameResolver resolver;

    auto result = PrepareAndValidate(root, resolver);
    ExpectErrorContains(result, "Owned result has no move/destroy consumer");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
