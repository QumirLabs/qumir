#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/parser/ast.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

void ExpectDisjointChildren(const TExprPtr& original, const TExprPtr& clone) {
    ASSERT_EQ(static_cast<bool>(original), static_cast<bool>(clone));
    if (!original) {
        return;
    }
    EXPECT_NE(original.Get(), clone.Get());
    const auto originalChildren = original->Children();
    const auto cloneChildren = clone->Children();
    ASSERT_EQ(originalChildren.size(), cloneChildren.size());
    for (size_t index = 0; index < originalChildren.size(); ++index) {
        ExpectDisjointChildren(originalChildren[index], cloneChildren[index]);
    }
}

} // namespace

TEST(AstClone, LifetimeAndHiddenChildrenAreDeeplyCloned) {
    TLocation loc{};
    auto ident = [&](std::string name) {
        return MakeIntrusive<TIdentExpr>(loc, std::move(name));
    };

    auto variable = MakeIntrusive<TVarStmt>(
        loc, "variable", NQumir::NTest::TestTypes().MakeInteger());
    variable->Init = ident("variable_init");
    auto vars = MakeIntrusive<TVarsBlockExpr>(
        loc, std::vector<TExprPtr>{variable});

    auto parameter = MakeIntrusive<TVarStmt>(
        loc, "parameter", NQumir::NTest::TestTypes().MakeInteger());
    parameter->Init = ident("parameter_init");
    auto function = MakeIntrusive<TFunDecl>(
        loc,
        "function",
        std::vector<TGenericParam>{},
        std::vector<TParam>{parameter},
        MakeIntrusive<TBlockExpr>(
            loc, std::vector<TExprPtr>{ident("function_body")}),
        NQumir::NTest::TestTypes().Make<TVoidType>());

    auto original = MakeIntrusive<TBlockExpr>(loc, std::vector<TExprPtr>{
        MakeIntrusive<TRetainExpr>(loc, ident("retain")),
        MakeIntrusive<TOwnLiteralExpr>(loc, ident("own")),
        MakeIntrusive<TMoveExpr>(loc, ident("move")),
        MakeIntrusive<TBorrowExpr>(loc, ident("borrow")),
        MakeIntrusive<TDestroyExpr>(loc, ident("destroy"), NQumir::NTest::TestTypes(), ident("aux")),
        MakeIntrusive<TReplaceExpr>(loc, ident("target"), ident("value"), NQumir::NTest::TestTypes()),
        MakeIntrusive<TCleanupExitExpr>(
            loc,
            ECleanupExitKind::Return,
            ident("return"),
            std::vector<TExprPtr>{ident("cleanup")}, NQumir::NTest::TestTypes()),
        MakeIntrusive<TGlobalCleanupExpr>(
            loc, std::vector<TExprPtr>{ident("global_cleanup")}, NQumir::NTest::TestTypes()),
        vars,
        function,
    });

    auto clone = TMaybeNode<TBlockExpr>(DeepCloneExpr(original)).Cast();
    ASSERT_NE(clone, nullptr);
    ExpectDisjointChildren(original, clone);

    auto clonedVars = TMaybeNode<TVarsBlockExpr>(clone->Stmts[8]).Cast();
    ASSERT_NE(clonedVars, nullptr);
    ASSERT_EQ(clonedVars->Vars.size(), 1u);
    ExpectDisjointChildren(variable, clonedVars->Vars[0]);

    auto clonedFunction = TMaybeNode<TFunDecl>(clone->Stmts[9]).Cast();
    ASSERT_NE(clonedFunction, nullptr);
    ASSERT_EQ(clonedFunction->Params.size(), 1u);
    ExpectDisjointChildren(parameter, clonedFunction->Params[0]);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
