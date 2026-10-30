#include "coercions.h"

#include <qumir/semantics/name_resolution/name_resolver.h>

namespace NQumir {
namespace NTypeAnnotation {

using namespace NAst;

namespace {

bool SameKind(const TTypePtr& a, const TTypePtr& b) {
    return TypeKey(a) == TypeKey(b);
}

bool IsGenericTypeParam(const TTypePtr& type, const std::unordered_set<std::string>* genericTypeParams) {
    auto named = TMaybeType<TNamedType>(type);
    return genericTypeParams
        && named
        && named.Cast()->TypeArgs.empty()
        && genericTypeParams->contains(named.Cast()->Name);
}

bool IsGenericValueParam(const std::string& value, const std::unordered_set<std::string>* genericValueParams) {
    return genericValueParams && genericValueParams->contains(value);
}

TArgCost GenericParamCost(bool topLevel) {
    return TArgCost{
        .GenericPenalty = 1,
        .TopLevelGenericPenalty = topLevel ? 1 : 0,
        .ConversionCost = 0,
        .FixedTypeNodes = 0,
        .GenericParamCount = 1,
    };
}

TArgCost StructuralGenericCost(TArgCost cost) {
    cost.GenericPenalty = 1;
    cost.TopLevelGenericPenalty = 0;
    ++cost.FixedTypeNodes;
    return cost;
}

std::optional<TArgCost> GenericParametricCost(
    const TTypePtr& paramType,
    const TTypePtr& argType,
    const std::unordered_set<std::string>* genericTypeParams,
    const std::unordered_set<std::string>* genericValueParams);

std::optional<TArgCost> CombineGenericCost(
    const TTypePtr& paramType,
    const TTypePtr& argType,
    const std::unordered_set<std::string>* genericTypeParams,
    const std::unordered_set<std::string>* genericValueParams,
    bool& matched)
{
    if (auto cost = GenericParametricCost(paramType, argType, genericTypeParams, genericValueParams)) {
        matched = true;
        return cost;
    }
    if (TypeKey(paramType) == TypeKey(argType)) {
        return TArgCost{};
    }
    return std::nullopt;
}

std::optional<TArgCost> GenericParametricCost(
    const TTypePtr& paramType,
    const TTypePtr& argType,
    const std::unordered_set<std::string>* genericTypeParams,
    const std::unordered_set<std::string>* genericValueParams)
{
    const bool hasTypeParams = genericTypeParams && !genericTypeParams->empty();
    const bool hasValueParams = genericValueParams && !genericValueParams->empty();
    if ((!hasTypeParams && !hasValueParams) || !paramType || !argType) {
        return std::nullopt;
    }
    if (IsGenericTypeParam(paramType, genericTypeParams)) {
        return GenericParamCost(false);
    }
    bool matched = false;
    TArgCost total;
    if (auto paramNamed = TMaybeType<TNamedType>(paramType)) {
        auto argNamed = TMaybeType<TNamedType>(argType);
        if (!argNamed || paramNamed.Cast()->Name != argNamed.Cast()->Name
            || paramNamed.Cast()->TypeArgs.size() != argNamed.Cast()->TypeArgs.size())
        {
            return std::nullopt;
        }
        for (size_t i = 0; i < paramNamed.Cast()->TypeArgs.size(); ++i) {
            const auto& paramArg = paramNamed.Cast()->TypeArgs[i];
            const auto& arg = argNamed.Cast()->TypeArgs[i];
            if (paramArg.Kind == TGenericArg::EKind::Type && arg.Kind == TGenericArg::EKind::Type) {
                auto cost = CombineGenericCost(paramArg.Type, arg.Type, genericTypeParams, genericValueParams, matched);
                if (!cost) {
                    return std::nullopt;
                }
                total = total + *cost;
            } else if (paramArg.Kind == TGenericArg::EKind::Value && arg.Kind == TGenericArg::EKind::Value) {
                if (IsGenericValueParam(paramArg.Value, genericValueParams)) {
                    matched = true;
                    total = total + GenericParamCost(false);
                } else if (paramArg.Value != arg.Value) {
                    return std::nullopt;
                }
            } else {
                return std::nullopt;
            }
        }
        return matched ? std::optional<TArgCost>{StructuralGenericCost(total)} : std::nullopt;
    }
    if (auto paramArray = TMaybeType<TArrayType>(paramType)) {
        auto argArray = TMaybeType<TArrayType>(argType);
        if (!argArray || paramArray.Cast()->Arity != argArray.Cast()->Arity) {
            return std::nullopt;
        }
        auto cost = CombineGenericCost(paramArray.Cast()->ElementType, argArray.Cast()->ElementType, genericTypeParams, genericValueParams, matched);
        return cost && matched ? std::optional<TArgCost>{StructuralGenericCost(*cost)} : std::nullopt;
    }
    if (auto paramVector = TMaybeType<TVectorType>(paramType)) {
        auto argVector = TMaybeType<TVectorType>(argType);
        if (!argVector) {
            return std::nullopt;
        }
        if (IsGenericValueParam(paramVector.Cast()->SizeParam, genericValueParams)) {
            matched = true;
            total = total + GenericParamCost(false);
        } else if (paramVector.Cast()->Size != argVector.Cast()->Size) {
            return std::nullopt;
        }
        auto cost = CombineGenericCost(paramVector.Cast()->ElementType, argVector.Cast()->ElementType,
            genericTypeParams, genericValueParams, matched);
        return cost && matched
            ? std::optional<TArgCost>{StructuralGenericCost(total + *cost)}
            : std::nullopt;
    }
    if (auto paramPtr = TMaybeType<TPointerType>(paramType)) {
        auto argPtr = TMaybeType<TPointerType>(argType);
        if (!argPtr) {
            return std::nullopt;
        }
        auto cost = CombineGenericCost(paramPtr.Cast()->PointeeType, argPtr.Cast()->PointeeType, genericTypeParams, genericValueParams, matched);
        return cost && matched ? std::optional<TArgCost>{StructuralGenericCost(*cost)} : std::nullopt;
    }
    if (auto paramRef = TMaybeType<TReferenceType>(paramType)) {
        auto argRef = TMaybeType<TReferenceType>(argType);
        auto argInner = argRef ? argRef.Cast()->ReferencedType : argType;
        auto cost = CombineGenericCost(paramRef.Cast()->ReferencedType, argInner, genericTypeParams, genericValueParams, matched);
        return cost && matched ? std::optional<TArgCost>{StructuralGenericCost(*cost)} : std::nullopt;
    }
    if (auto paramFuture = TMaybeType<TFutureType>(paramType)) {
        auto argFuture = TMaybeType<TFutureType>(argType);
        if (!argFuture) {
            return std::nullopt;
        }
        auto cost = CombineGenericCost(paramFuture.Cast()->ResultType, argFuture.Cast()->ResultType, genericTypeParams, genericValueParams, matched);
        return cost && matched ? std::optional<TArgCost>{StructuralGenericCost(*cost)} : std::nullopt;
    }
    if (auto paramFun = TMaybeType<TFunctionType>(paramType)) {
        auto argFun = TMaybeType<TFunctionType>(argType);
        if (!argFun || paramFun.Cast()->ParamTypes.size() != argFun.Cast()->ParamTypes.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < paramFun.Cast()->ParamTypes.size(); ++i) {
            auto cost = CombineGenericCost(paramFun.Cast()->ParamTypes[i], argFun.Cast()->ParamTypes[i], genericTypeParams, genericValueParams, matched);
            if (!cost) {
                return std::nullopt;
            }
            total = total + *cost;
        }
        auto retCost = CombineGenericCost(paramFun.Cast()->ReturnType, argFun.Cast()->ReturnType, genericTypeParams, genericValueParams, matched);
        if (!retCost) {
            return std::nullopt;
        }
        total = total + *retCost;
        return matched ? std::optional<TArgCost>{StructuralGenericCost(total)} : std::nullopt;
    }
    if (auto paramStruct = TMaybeType<TStructType>(paramType)) {
        auto argStruct = TMaybeType<TStructType>(argType);
        if (!argStruct || paramStruct.Cast()->Fields.size() != argStruct.Cast()->Fields.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < paramStruct.Cast()->Fields.size(); ++i) {
            if (paramStruct.Cast()->Fields[i].first != argStruct.Cast()->Fields[i].first) {
                return std::nullopt;
            }
            auto cost = CombineGenericCost(paramStruct.Cast()->Fields[i].second, argStruct.Cast()->Fields[i].second, genericTypeParams, genericValueParams, matched);
            if (!cost) {
                return std::nullopt;
            }
            total = total + *cost;
        }
        return matched ? std::optional<TArgCost>{StructuralGenericCost(total)} : std::nullopt;
    }
    return std::nullopt;
}

bool WideningInt(const TTypePtr& from, const TTypePtr& to) {
    auto s = TMaybeType<TIntegerType>(from).Cast();
    auto d = TMaybeType<TIntegerType>(to).Cast();
    if (!s || !d) {
        return false;
    }
    int ws = s->BitWidth(), wd = d->BitWidth();
    bool ss = s->IsSigned(), sd = d->IsSigned();
    if (ss == sd) {
        return wd >= ws;
    }
    if (!ss && sd) {
        return wd > ws; // u8->i16, u16->i32, u32->i64
    }
    return false;
}

bool WideningFloat(const TTypePtr& from, const TTypePtr& to) {
    auto s = TMaybeType<TFloatType>(from).Cast();
    auto d = TMaybeType<TFloatType>(to).Cast();
    return s && d && d->BitWidth() >= s->BitWidth();
}

} // namespace

std::optional<TArgCost> ArgCost(
    const TTypePtr& from,
    const TTypePtr& to,
    NSemantics::TNameResolver* ctx,
    const std::unordered_set<std::string>* genericTypeParams,
    const std::unordered_set<std::string>* genericValueParams)
{
    if (!from || !to) {
        return std::nullopt;
    }

    // Generic type parameter: matches any concrete argument type, but at a
    // higher cost than any concrete overload - the actual type bound to the
    // parameter is determined once this overload is chosen as the best match.
    if (IsGenericTypeParam(to, genericTypeParams)) {
        return GenericParamCost(true);
    }
    if (auto cost = GenericParametricCost(to, from, genericTypeParams, genericValueParams)) {
        return cost;
    }

    if (SameKind(from, to)) {
        return TArgCost{};
    }

    if (TMaybeType<TIntegerType>(from) && TMaybeType<TIntegerType>(to)) {
        return WideningInt(from, to) ? std::optional<TArgCost>{TArgCost{.ConversionCost = 1}} : std::nullopt;
    }

    if (TMaybeType<TIntegerType>(from) && TMaybeType<TFloatType>(to)) {
        return TArgCost{.ConversionCost = 2};
    }
    if (TMaybeType<TFloatType>(from) && TMaybeType<TFloatType>(to)) {
        return WideningFloat(from, to) ? std::optional<TArgCost>{TArgCost{.ConversionCost = 1}} : std::nullopt;
    }

    if (ctx && ctx->GetCast(from, to)) {
        return TArgCost{.ConversionCost = 1};
    }

    return std::nullopt;
}

} // namespace NTypeAnnotation
} // namespace NQumir
