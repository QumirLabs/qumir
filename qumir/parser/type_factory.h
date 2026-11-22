#pragma once

#include "type.h"

#include <unordered_map>
#include <map>

namespace NQumir {
namespace NAst {

class TTypeFactory {
public:
    template<typename T>
    TConcreteTypePtr<T> Make() {
        auto& ret = Types_[T::TypeId];
        if (!ret) {
            ret = MakeIntrusive<T>();
        }
        return StaticPointerCast<T>(ret);
    }

    TConcreteTypePtr<TIntegerType> MakeInteger(TIntegerType::EKind kind) {
        auto& ret = IntegerTypes_[kind];
        return ret ? ret : (ret = MakeIntrusive<TIntegerType>(kind));
    }

    TConcreteTypePtr<TFloatType> MakeFloat(TFloatType::EKind kind) {
        auto& ret = FloatTypes_[kind];
        return ret ? ret : (ret = MakeIntrusive<TFloatType>(kind));
    }

    TConcreteTypePtr<TFunctionType> MakeFunction(std::vector<TTypePtr>&& params, const TTypePtr& retType) {
        auto& ret = FunctionTypes_[{params, retType}];
        return ret ? ret : (ret = MakeIntrusive<TFunctionType>(std::move(params), retType));
    }

    TConcreteTypePtr<TFutureType> MakeFuture(const TTypePtr& type) {
        auto& ret = FutureTypes_[type];
        return ret ? ret : (ret = MakeIntrusive<TFutureType>(type));
    }

    TConcreteTypePtr<TArrayType> MakeArray(const TTypePtr& elementType, int arity) {
        auto& ret = ArrayTypes_[{elementType, arity}];
        return ret ? ret : (ret = MakeIntrusive<TArrayType>(elementType, arity));
    }

    TConcreteTypePtr<TPointerType> MakePointer(const TTypePtr& pointee) {
        auto& ret = PointerTypes_[pointee];
        return ret ? ret : (ret = MakeIntrusive<TPointerType>(pointee));
    }

    TConcreteTypePtr<TReferenceType> MakeReference(const TTypePtr& referenced) {
        auto& ret = ReferenceTypes_[referenced];
        return ret ? ret : (ret = MakeIntrusive<TReferenceType>(referenced));
    }

    // Not interned: TNamedType is completed after creation (UnderlyingType, Reference
    // are assigned during name resolution), so every call returns a fresh object.
    TConcreteTypePtr<TNamedType> MakeNamed(
        const std::string& name,
        const TTypePtr& underlying,
        std::vector<TGenericArg>&& typeArgs = {})
    {
        return MakeIntrusive<TNamedType>(name, underlying, std::move(typeArgs));
    }

    TConcreteTypePtr<TStructType> MakeStruct(std::vector<std::pair<std::string, TTypePtr>>&& fields) {
        auto& ret = TStructTypes_[fields];
        return ret ? ret : (ret = MakeIntrusive<TStructType>(std::move(fields)));
    }

    TConcreteTypePtr<TVectorType> MakeVector(const TTypePtr& elementType, int size) {
        auto& ret = VectorTypes_[{elementType, size}];
        return ret ? ret : (ret = MakeIntrusive<TVectorType>(elementType, size));
    }

private:
    std::unordered_map<std::string_view, TTypePtr> Types_;
    std::unordered_map<TIntegerType::EKind, TConcreteTypePtr<TIntegerType>> IntegerTypes_;
    std::unordered_map<TFloatType::EKind, TConcreteTypePtr<TFloatType>> FloatTypes_;
    std::map<std::pair<std::vector<TTypePtr>, TTypePtr>, TConcreteTypePtr<TFunctionType>> FunctionTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TFutureType>> FutureTypes_;
    std::map<std::pair<TTypePtr, int>, TConcreteTypePtr<TArrayType>> ArrayTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TPointerType>> PointerTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TReferenceType>> ReferenceTypes_;
    std::map<std::vector<std::pair<std::string, TTypePtr>>, TConcreteTypePtr<TStructType>> TStructTypes_;
    std::map<std::pair<TTypePtr, int>, TConcreteTypePtr<TVectorType>> VectorTypes_;
};

} // namespace NAst
} // namespace NQumir
