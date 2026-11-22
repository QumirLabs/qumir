#include "type_traits.h"

#include <stdexcept>
#include <unordered_set>

namespace NQumir {
namespace NSemantics {
namespace {

TLifetimeTraits Classify(
    const NAst::TTypePtr& type,
    std::unordered_set<const NAst::TType*>& activeTypes)
{
    if (!type) {
        throw std::invalid_argument("cannot classify lifetime of a null type");
    }

    if (auto named = NAst::TMaybeType<NAst::TNamedType>(type)) {
        if (!activeTypes.insert(type).second) {
            throw std::invalid_argument("cyclic named type in lifetime classification");
        }
        auto result = Classify(named.Cast()->UnderlyingType(), activeTypes);
        activeTypes.erase(type);
        return result;
    }

    if (NAst::TMaybeType<NAst::TReferenceType>(type)) {
        // A reference is a borrowed handle. It never inherits ownership of the
        // referenced value and therefore has no destruction responsibility.
        return {
            .Kind = ELifetimeKind::Trivial,
            .CanCopy = true,
            .NeedsDestroy = false,
        };
    }

    if (NAst::TMaybeType<NAst::TIntegerType>(type)
        || NAst::TMaybeType<NAst::TFloatType>(type)
        || NAst::TMaybeType<NAst::TBoolType>(type)
        || NAst::TMaybeType<NAst::TSymbolType>(type)
        || NAst::TMaybeType<NAst::TPointerType>(type)
        || NAst::TMaybeType<NAst::TVectorType>(type))
    {
        return {
            .Kind = ELifetimeKind::Trivial,
            .CanCopy = true,
            .NeedsDestroy = false,
        };
    }

    if (NAst::TMaybeType<NAst::TStringType>(type)) {
        return {
            .Kind = ELifetimeKind::RefCounted,
            .CanCopy = true,
            .NeedsDestroy = true,
        };
    }

    if (NAst::TMaybeType<NAst::TArrayType>(type)) {
        return {
            .Kind = ELifetimeKind::Unique,
            .CanCopy = false,
            .NeedsDestroy = true,
        };
    }

    if (auto structType = NAst::TMaybeType<NAst::TStructType>(type)) {
        if (!activeTypes.insert(type).second) {
            throw std::invalid_argument("cyclic struct type in lifetime classification");
        }

        TLifetimeTraits result {
            .Kind = ELifetimeKind::Aggregate,
            .CanCopy = true,
            .NeedsDestroy = false,
        };
        for (const auto& field : structType.Cast()->Fields) {
            const auto fieldTraits = Classify(field.second, activeTypes);
            result.CanCopy = result.CanCopy && fieldTraits.CanCopy;
            result.NeedsDestroy = result.NeedsDestroy || fieldTraits.NeedsDestroy;
        }
        activeTypes.erase(type);
        return result;
    }

    throw std::invalid_argument(
        "unsupported type in lifetime classification: " + std::string(type->TypeName()));
}

} // namespace

TLifetimeTraits GetLifetimeTraits(const NAst::TTypePtr& type) {
    std::unordered_set<const NAst::TType*> activeTypes;
    return Classify(type, activeTypes);
}

} // namespace NSemantics
} // namespace NQumir
