#include <gtest/gtest.h>

#include <qumir/semantics/lifetime/type_traits.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;
using namespace NQumir::NSemantics;

namespace {

struct TUnknownManagedType : TType {
    static constexpr const char* TypeId = "UnknownManaged";

    const std::string_view TypeName() const override {
        return TypeId;
    }
};

void ExpectTraits(
    const TTypePtr& type,
    ELifetimeKind kind,
    bool canCopy,
    bool needsDestroy)
{
    const auto traits = GetLifetimeTraits(type);
    EXPECT_EQ(traits.Kind, kind);
    EXPECT_EQ(traits.CanCopy, canCopy);
    EXPECT_EQ(traits.NeedsDestroy, needsDestroy);
}

} // namespace

TEST(LifetimeTraits, ScalarTypesAreTrivial) {
    ExpectTraits(MakeIntrusive<TIntegerType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(MakeIntrusive<TFloatType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(MakeIntrusive<TBoolType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(MakeIntrusive<TSymbolType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(
        MakeIntrusive<TPointerType>(MakeIntrusive<TStringType>()),
        ELifetimeKind::Trivial,
        true,
        false);
}

TEST(LifetimeTraits, StringIsRefCounted) {
    ExpectTraits(MakeIntrusive<TStringType>(), ELifetimeKind::RefCounted, true, true);
}

TEST(LifetimeTraits, NumericVectorsAreTrivial) {
    const std::vector<TTypePtr> elements = {
        MakeIntrusive<TIntegerType>(),
        MakeIntrusive<TFloatType>(),
        MakeIntrusive<TBoolType>(),
    };
    for (const auto& element : elements) {
        ExpectTraits(MakeIntrusive<TVectorType>(element, 4), ELifetimeKind::Trivial, true, false);
    }
}

TEST(LifetimeTraits, NamedStringUsesUnderlyingType) {
    auto type = MakeIntrusive<TNamedType>("Text", MakeIntrusive<TStringType>());
    ExpectTraits(type, ELifetimeKind::RefCounted, true, true);
}

TEST(LifetimeTraits, StringReferenceIsBorrowed) {
    auto type = MakeIntrusive<TReferenceType>(MakeIntrusive<TStringType>());
    ExpectTraits(type, ELifetimeKind::Trivial, true, false);
}

TEST(LifetimeTraits, PlainArrayIsUnique) {
    auto type = MakeIntrusive<TArrayType>(MakeIntrusive<TIntegerType>(), 1);
    ExpectTraits(type, ELifetimeKind::Unique, false, true);
}

TEST(LifetimeTraits, StringArrayIsUnique) {
    auto type = MakeIntrusive<TArrayType>(MakeIntrusive<TStringType>(), 1);
    ExpectTraits(type, ELifetimeKind::Unique, false, true);
}

TEST(LifetimeTraits, TrivialStructIsCopyableAggregate) {
    auto type = MakeIntrusive<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"number", MakeIntrusive<TIntegerType>()},
            {"flag", MakeIntrusive<TBoolType>()},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, true, false);
}

TEST(LifetimeTraits, StructWithStringNeedsDestroy) {
    auto type = MakeIntrusive<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", MakeIntrusive<TStringType>()},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, true, true);
}

TEST(LifetimeTraits, StructWithArrayIsNotCopyable) {
    auto type = MakeIntrusive<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"items", MakeIntrusive<TArrayType>(MakeIntrusive<TStringType>(), 1)},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, false, true);
}

TEST(LifetimeTraits, NestedStructPropagatesFieldTraits) {
    auto inner = MakeIntrusive<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", MakeIntrusive<TStringType>()},
        });
    auto outer = MakeIntrusive<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"inner", std::move(inner)},
        });
    ExpectTraits(outer, ELifetimeKind::Aggregate, true, true);
}

TEST(LifetimeTraits, UnknownManagedShapeIsRejected) {
    auto type = MakeIntrusive<TUnknownManagedType>();
    EXPECT_THROW(GetLifetimeTraits(type), std::invalid_argument);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
