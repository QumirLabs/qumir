#include <gtest/gtest.h>

#include <qumir/modules/system/system.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type_factory.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <sstream>
#include <type_traits>
#include <unordered_set>

using namespace NQumir;
using namespace NQumir::NAst;

// Type constructors are private to the factory, and types are not copied.
static_assert(!std::is_default_constructible_v<TBoolType>);
static_assert(!std::is_constructible_v<TIntegerType, TIntegerType::EKind>);
static_assert(!std::is_constructible_v<TNamedType, std::string, TTypePtr>);
static_assert(!std::is_copy_constructible_v<TIntegerType>);
// Final: MakeCustom cannot make an uninterned subclass that TMaybeType accepts.
static_assert(std::is_final_v<TNamedType>);
static_assert(std::is_final_v<TIntegerType>);

namespace {

// A type kind Qumir does not know.
struct TCustomType : TType {
    static constexpr TTypeId TypeId{"Custom"};

private:
    friend class NQumir::NAst::TTypeFactory;

    TCustomType()
        : TType(TypeId)
    {}
};

template<typename T, typename... TArgs>
concept CanMakeCustom = requires(TTypeFactory& types, TArgs... args) {
    types.MakeCustom<T>(args...);
};

} // namespace

// MakeCustom takes unknown kinds only: a built-in kind made there would not be interned.
static_assert(CanMakeCustom<TCustomType>);
static_assert(!CanMakeCustom<TIntegerType, TIntegerType::EKind>);
static_assert(!CanMakeCustom<TNamedType, std::string, TTypePtr>);

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

TEST(TypeFactory, NamedTypesAreInternedByNameAndArgsAndCompletedOnce) {
    TTypeFactory types;
    auto i64 = types.MakeInteger();
    auto f64 = types.MakeFloat();

    // Created incomplete, e.g. by a parser before the declaration is known.
    auto incomplete = types.MakeNamed("X");
    EXPECT_FALSE(incomplete->UnderlyingType());
    EXPECT_EQ(incomplete, types.MakeNamed("X"));

    // Completing it fills the very same object.
    auto complete = types.MakeNamed("X", i64);
    EXPECT_EQ(complete, incomplete);
    EXPECT_EQ(incomplete->UnderlyingType(), i64);
    EXPECT_EQ(complete, types.MakeNamed("X"));
    EXPECT_EQ(complete, types.MakeNamed("X", i64));

    // Different arguments are a different type.
    EXPECT_NE(types.MakeNamed("X", i64, {TGenericArg::TypeArg(i64)}), complete);

    // Value arguments are part of the key too, e.g. a host's "<named DECIMAL [15 2]>".
    auto decimal = types.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("2")});
    EXPECT_EQ(decimal, types.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("2")}));
    EXPECT_NE(decimal, types.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("3")}));

    // A conflicting redefinition of the name does not reuse the stale declaration.
    auto redefined = types.MakeNamed("X", f64);
    EXPECT_NE(redefined, complete);
    EXPECT_EQ(complete->UnderlyingType(), i64);
    EXPECT_EQ(redefined->UnderlyingType(), f64);
}

TEST(TypeFactory, NamedReferenceIsCompletedLikeUnderlyingType) {
    TTypeFactory types;
    auto i64 = types.MakeInteger();

    auto named = types.MakeNamed("X", i64);
    EXPECT_FALSE(named->Reference());

    // The module a type comes from is a property of its name: filled in once.
    EXPECT_EQ(types.MakeNamed("X", i64, {}, "Module"), named);
    EXPECT_EQ(named->Reference(), "Module");
    EXPECT_EQ(types.MakeNamed("X", i64, {}, "Other"), named);
    EXPECT_EQ(named->Reference(), "Module");
}

TEST(TypeFactory, TypesFromDifferentFactoriesAreDifferentObjects) {
    TTypeFactory first;
    TTypeFactory second;
    EXPECT_NE(first.MakeInteger(), second.MakeInteger());
}

TEST(TypeFactory, BaseTypeIsTheSameObjectInTheChild) {
    auto base = std::make_shared<TTypeFactory>();
    auto i64 = base->MakeInteger();
    auto pointer = base->MakePointer(i64);
    auto function = base->MakeFunction({i64}, pointer);
    auto fields = base->MakeStruct({{"a", i64}});
    auto named = base->MakeNamed("X", fields);
    base->Freeze();

    TTypeFactory child(base);
    EXPECT_EQ(child.MakeInteger(), i64);
    EXPECT_EQ(child.Make<TBoolType>(), child.Make<TBoolType>());
    EXPECT_EQ(child.MakePointer(child.MakeInteger()), pointer);
    EXPECT_EQ(child.MakeFunction({i64}, pointer), function);
    EXPECT_EQ(child.MakeStruct({{"a", i64}}), fields);
    EXPECT_EQ(child.MakeNamed("X"), named);
    EXPECT_EQ(child.MakeNamed("X", fields), named);
}

TEST(TypeFactory, ChildCreatesTheTypesTheBaseLacks) {
    auto base = std::make_shared<TTypeFactory>();
    auto i64 = base->MakeInteger();
    base->Freeze();

    TTypeFactory first(base);
    TTypeFactory second(base);
    auto array = first.MakeArray(i64, 2);
    EXPECT_EQ(first.MakeArray(i64, 2), array);
    // The base did not get it, so another child makes its own.
    EXPECT_NE(second.MakeArray(i64, 2), array);
}

TEST(TypeFactory, FrozenFactoryCreatesAndChangesNoType) {
    auto base = std::make_shared<TTypeFactory>();
    auto i64 = base->MakeInteger();
    auto incomplete = base->MakeNamed("X");
    auto unreferenced = base->MakeNamed("Y", i64);
    base->Freeze();

    EXPECT_EQ(base->MakeInteger(), i64);
    EXPECT_THROW(base->MakeArray(i64, 2), std::runtime_error);
    EXPECT_THROW(base->MakeCustom<TCustomType>(), std::runtime_error);
    EXPECT_THROW(base->MakeNamed("X", i64), std::runtime_error);
    EXPECT_THROW(base->MakeNamed("Y", i64, {}, "Module"), std::runtime_error);
    // An unfrozen base could still create a type that a child has already made.
    EXPECT_THROW(TTypeFactory child(std::make_shared<TTypeFactory>()), std::runtime_error);

    // Completing a base type gives the child its own object; the base type stays as it is.
    TTypeFactory child(base);
    auto completed = child.MakeNamed("X", i64);
    EXPECT_NE(completed, incomplete);
    EXPECT_EQ(completed->UnderlyingType(), i64);
    EXPECT_EQ(child.MakeNamed("X"), completed);
    EXPECT_FALSE(incomplete->UnderlyingType());
    auto referenced = child.MakeNamed("Y", nullptr, {}, "Module");
    EXPECT_NE(referenced, unreferenced);
    EXPECT_EQ(referenced->UnderlyingType(), i64);
    EXPECT_EQ(referenced->Reference(), "Module");
    EXPECT_FALSE(unreferenced->Reference());
    EXPECT_TRUE(child.MakeArray(i64, 2));
}

TEST(TypeFactory, NamedTypesWithArgumentsComeFromTheBase) {
    auto base = std::make_shared<TTypeFactory>();
    auto i64 = base->MakeInteger();
    auto layout = base->MakeStruct({{"Value", i64}, {"Valid", base->Make<TBoolType>()}});
    auto decimal = base->MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("2")});
    auto nullable = base->MakeNamed("Nullable", layout, {TGenericArg::TypeArg(i64)});
    base->Freeze();

    TTypeFactory child(base);
    TTypeFactory other(base);
    EXPECT_EQ(child.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("2")}), decimal);
    EXPECT_EQ(child.MakeNamed("Nullable", layout, {TGenericArg::TypeArg(child.MakeInteger())}), nullable);
    EXPECT_EQ(child.MakeNamed("Nullable", nullptr, {TGenericArg::TypeArg(i64)}), nullable);

    auto scale3 = child.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("3")});
    EXPECT_NE(scale3, decimal);
    EXPECT_NE(other.MakeNamed("DECIMAL", nullptr, {TGenericArg::ValueArg("15"), TGenericArg::ValueArg("3")}), scale3);

    // A redefinition replaces the base type in that child only.
    auto redefined = child.MakeNamed("Nullable", i64, {TGenericArg::TypeArg(i64)});
    EXPECT_NE(redefined, nullable);
    EXPECT_EQ(child.MakeNamed("Nullable", nullptr, {TGenericArg::TypeArg(i64)}), redefined);
    EXPECT_EQ(other.MakeNamed("Nullable", nullptr, {TGenericArg::TypeArg(i64)}), nullable);
    EXPECT_EQ(nullable->UnderlyingType(), layout);
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
