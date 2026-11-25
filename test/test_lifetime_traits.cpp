#include <gtest/gtest.h>
#include "test_types.h"

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
    static constexpr TTypeId TypeId{"Unknown"};

private:
    friend class NQumir::NAst::TTypeFactory;

    TUnknownManagedType()
        : TType(TypeId)
    {}
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
    ExpectTraits(NQumir::NTest::TestTypes().MakeInteger(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(NQumir::NTest::TestTypes().MakeFloat(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(NQumir::NTest::TestTypes().Make<TBoolType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(NQumir::NTest::TestTypes().Make<TSymbolType>(), ELifetimeKind::Trivial, true, false);
    ExpectTraits(
        NQumir::NTest::TestTypes().MakePointer(NQumir::NTest::TestTypes().Make<TStringType>()),
        ELifetimeKind::Trivial,
        true,
        false);
}

TEST(LifetimeTraits, StringIsRefCounted) {
    ExpectTraits(NQumir::NTest::TestTypes().Make<TStringType>(), ELifetimeKind::RefCounted, true, true);
}

TEST(LifetimeTraits, NumericVectorsAreTrivial) {
    const std::vector<TTypePtr> elements = {
        NQumir::NTest::TestTypes().MakeInteger(),
        NQumir::NTest::TestTypes().MakeFloat(),
        NQumir::NTest::TestTypes().Make<TBoolType>(),
    };
    for (const auto& element : elements) {
        ExpectTraits(NQumir::NTest::TestTypes().MakeVector(element, 4), ELifetimeKind::Trivial, true, false);
    }
}

TEST(LifetimeTraits, NamedStringUsesUnderlyingType) {
    auto type = NQumir::NTest::TestTypes().MakeNamed("Text", NQumir::NTest::TestTypes().Make<TStringType>());
    ExpectTraits(type, ELifetimeKind::RefCounted, true, true);
}

TEST(LifetimeTraits, StringReferenceIsBorrowed) {
    auto type = NQumir::NTest::TestTypes().MakeReference(NQumir::NTest::TestTypes().Make<TStringType>());
    ExpectTraits(type, ELifetimeKind::Trivial, true, false);
}

TEST(LifetimeTraits, PlainArrayIsUnique) {
    auto type = NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().MakeInteger(), 1);
    ExpectTraits(type, ELifetimeKind::Unique, false, true);
}

TEST(LifetimeTraits, StringArrayIsUnique) {
    auto type = NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 1);
    ExpectTraits(type, ELifetimeKind::Unique, false, true);
}

TEST(LifetimeTraits, TrivialStructIsCopyableAggregate) {
    auto type = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"number", NQumir::NTest::TestTypes().MakeInteger()},
            {"flag", NQumir::NTest::TestTypes().Make<TBoolType>()},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, true, false);
}

TEST(LifetimeTraits, StructWithStringNeedsDestroy) {
    auto type = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", NQumir::NTest::TestTypes().Make<TStringType>()},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, true, true);
}

TEST(LifetimeTraits, StructWithArrayIsNotCopyable) {
    auto type = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"items", NQumir::NTest::TestTypes().MakeArray(NQumir::NTest::TestTypes().Make<TStringType>(), 1)},
        });
    ExpectTraits(type, ELifetimeKind::Aggregate, false, true);
}

TEST(LifetimeTraits, NestedStructPropagatesFieldTraits) {
    auto inner = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", NQumir::NTest::TestTypes().Make<TStringType>()},
        });
    auto outer = NQumir::NTest::TestTypes().MakeStruct(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"inner", std::move(inner)},
        });
    ExpectTraits(outer, ELifetimeKind::Aggregate, true, true);
}

TEST(LifetimeTraits, UnknownManagedShapeIsRejected) {
    auto type = NQumir::NTest::TestTypes().MakeCustom<TUnknownManagedType>();
    EXPECT_THROW(GetLifetimeTraits(type), std::invalid_argument);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
