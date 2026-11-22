#include <gtest/gtest.h>

#include <qumir/modules/system/system.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type_factory.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <sstream>
#include <unordered_set>

using namespace NQumir;
using namespace NQumir::NAst;

TEST(TypeFactory, InternsImmutableTypes) {
    TTypeFactory types;
    auto i64 = types.MakeInteger();
    EXPECT_EQ(i64, types.MakeInteger(TIntegerType::I64));
    EXPECT_NE(i64, types.MakeInteger(TIntegerType::I32));
    EXPECT_EQ(types.Make<TBoolType>(), types.Make<TBoolType>());
    EXPECT_EQ(types.MakeFloat(), types.MakeFloat(TFloatType::F64));

    EXPECT_EQ(types.MakePointer(i64), types.MakePointer(i64));
    EXPECT_EQ(types.MakeReference(i64), types.MakeReference(i64));
    EXPECT_EQ(types.MakeFuture(i64), types.MakeFuture(i64));
    EXPECT_EQ(types.MakeArray(i64, 2), types.MakeArray(i64, 2));
    EXPECT_NE(types.MakeArray(i64, 2), types.MakeArray(i64, 3));
    EXPECT_EQ(types.MakeFunction({i64, i64}, i64), types.MakeFunction({i64, i64}, i64));
    EXPECT_NE(types.MakeFunction({i64}, i64), types.MakeFunction({i64, i64}, i64));
    EXPECT_EQ(types.MakeStruct({{"a", i64}}), types.MakeStruct({{"a", i64}}));
    EXPECT_EQ(types.MakeVector(i64, 4), types.MakeVector(i64, 4));
    EXPECT_EQ(types.MakeTensor(i64, {2, 3}), types.MakeTensor(i64, {2, 3}));
    EXPECT_EQ(types.WrapFuture(i64), types.MakeFuture(i64));
    EXPECT_EQ(types.WrapFuture(types.MakeFuture(i64)), types.MakeFuture(i64));
}

TEST(TypeFactory, NamedTypesAreCompletedAfterCreationAndNotInterned) {
    TTypeFactory types;
    auto first = types.MakeNamed("X", nullptr);
    auto second = types.MakeNamed("X", nullptr);
    EXPECT_NE(first, second);
}

TEST(TypeFactory, PointersAreHashableAndOrdered) {
    TTypeFactory types;
    std::unordered_set<TTypePtr> set{types.MakeInteger(), types.MakeInteger(), types.Make<TBoolType>()};
    EXPECT_EQ(set.size(), 2u);
}

TEST(TypeFactory, ParserModulesAndNodesShareOneFactory) {
    TTypeFactory types;
    NRegistry::SystemModule system(types);
    NSemantics::TNameResolver resolver(types);

    std::istringstream input("(block (var x i64) (= x 1))");
    NAst::NCore::TTokenStream tokens(input);
    NAst::NCore::TParser parser(types);
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().ToString();

    auto block = TMaybeNode<TBlockExpr>(*parsed).Cast();
    ASSERT_TRUE(block);
    auto var = TMaybeNode<TVarStmt>(block->Stmts[0]).Cast();
    auto assign = TMaybeNode<TAssignExpr>(block->Stmts[1]).Cast();
    ASSERT_TRUE(var);
    ASSERT_TRUE(assign);
    // The declared type from the parser and the literal's type from the node constructor
    // are the very same object as the factory's.
    EXPECT_EQ(var->Type, types.MakeInteger());
    EXPECT_EQ(TMaybeNode<TNumberExpr>(assign->Value).Cast()->Type, types.MakeInteger());
    EXPECT_EQ(&resolver.Types(), &types);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
