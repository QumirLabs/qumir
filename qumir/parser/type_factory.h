#pragma once

#include "type.h"

#include <unordered_map>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>

namespace NQumir {
namespace NAst {

// The kinds TTypeFactory interns: MakeCustom must not create a second, unequal object of one.
template<typename T>
inline constexpr bool IsInternedTypeKind =
    std::is_same_v<T, TIntegerType> || std::is_same_v<T, TFloatType> || std::is_same_v<T, TBoolType>
    || std::is_same_v<T, TStringType> || std::is_same_v<T, TSymbolType> || std::is_same_v<T, TVoidType>
    || std::is_same_v<T, TFunctionType> || std::is_same_v<T, TFutureType> || std::is_same_v<T, TArrayType>
    || std::is_same_v<T, TPointerType> || std::is_same_v<T, TReferenceType> || std::is_same_v<T, TNamedType>
    || std::is_same_v<T, TStructType> || std::is_same_v<T, TVectorType> || std::is_same_v<T, TTensorType>;

// Creates, owns and interns immutable types: two calls with equal arguments return the
// same object, so type equality is pointer equality. A type lives as long as its factory,
// so the factory must outlive every AST, pass, IR module and codegen that refers to its
// types. One factory is shared by every layer of a compilation through their contexts.
// A factory may have a frozen base: a type the base holds is that same object here, and
// only the types the base lacks are created here. The base is only read.
class TTypeFactory {
public:
    TTypeFactory() = default;

    explicit TTypeFactory(std::shared_ptr<const TTypeFactory> base)
        : Base_(std::move(base))
    {
        if (Base_ && !Base_->Frozen_) {
            throw std::runtime_error("TTypeFactory: the base factory is not frozen");
        }
    }

    // From now on the factory creates and changes no type, so it can be a base.
    void Freeze() {
        Frozen_ = true;
    }

    // Parameterless singleton types: bool, string, char, void.
    template<typename T>
    requires (!std::is_same_v<T, TIntegerType> && !std::is_same_v<T, TFloatType>)
    TConcreteTypePtr<T> Make() {
        return Intern<T, &TTypeFactory::Types_>(std::string_view(T::TypeId));
    }

    TConcreteTypePtr<TIntegerType> MakeInteger(TIntegerType::EKind kind = TIntegerType::I64) {
        return Intern<TIntegerType, &TTypeFactory::IntegerTypes_>(kind, kind);
    }

    TConcreteTypePtr<TFloatType> MakeFloat(TFloatType::EKind kind = TFloatType::F64) {
        return Intern<TFloatType, &TTypeFactory::FloatTypes_>(kind, kind);
    }

    TConcreteTypePtr<TFunctionType> MakeFunction(std::vector<TTypePtr>&& params, const TTypePtr& retType) {
        return Intern<TFunctionType, &TTypeFactory::FunctionTypes_>(std::pair(params, retType), std::move(params), retType);
    }

    TConcreteTypePtr<TFutureType> MakeFuture(const TTypePtr& type) {
        return Intern<TFutureType, &TTypeFactory::FutureTypes_>(type, type);
    }

    // `future T`, or the type itself if it is already a future.
    TTypePtr WrapFuture(const TTypePtr& type) {
        if (TMaybeType<TFutureType>(type)) {
            return type;
        }
        return MakeFuture(type);
    }

    TConcreteTypePtr<TArrayType> MakeArray(const TTypePtr& elementType, int arity) {
        return Intern<TArrayType, &TTypeFactory::ArrayTypes_>(std::pair(elementType, arity), elementType, arity);
    }

    TConcreteTypePtr<TPointerType> MakePointer(const TTypePtr& pointee) {
        return Intern<TPointerType, &TTypeFactory::PointerTypes_>(pointee, pointee);
    }

    TConcreteTypePtr<TReferenceType> MakeReference(const TTypePtr& referenced) {
        return Intern<TReferenceType, &TTypeFactory::ReferenceTypes_>(referenced, referenced);
    }

    // Interned by (name, typeArgs). UnderlyingType and Reference are not part of the key: a
    // named type is declared by name, so the same name completes with the same underlying
    // type and comes from the same module. An incomplete named type (no underlying yet) is
    // completed in place, once. A conflicting redefinition - the name is declared again with
    // a different underlying type, e.g. a reused resolver or a second program - gets a fresh
    // object and replaces the cached one. A base type is never changed: completing or
    // redefining it gives this factory its own object under that key.
    TConcreteTypePtr<TNamedType> MakeNamed(
        const std::string& name,
        const TTypePtr& underlying = nullptr,
        std::vector<TGenericArg>&& typeArgs = {},
        std::optional<std::string> reference = std::nullopt)
    {
        auto key = std::pair(name, typeArgs);
        if (auto it = NamedTypes_.find(key); it != NamedTypes_.end()) {
            auto* ret = it->second;
            if (underlying && ret->UnderlyingType() && ret->UnderlyingType() != underlying) {
                return it->second = New<TNamedType>(name, underlying, std::move(typeArgs), std::move(reference));
            }
            if (underlying && !ret->UnderlyingType()) {
                CheckNotFrozen();
                ret->SetUnderlyingType(underlying);
            }
            if (reference && !ret->Reference()) {
                CheckNotFrozen();
                ret->SetReference(std::move(reference));
            }
            return ret;
        }
        TConcreteTypePtr<TNamedType> base = Base_
            ? Base_->Find<&TTypeFactory::NamedTypes_>(key)
            : nullptr;
        if (base && underlying && base->UnderlyingType() && base->UnderlyingType() != underlying) {
            base = nullptr;
        }
        if (base && (!underlying || base->UnderlyingType()) && (!reference || base->Reference())) {
            return base;
        }
        // The copy of a base type keeps what the base type already has, as completing it would.
        auto completed = base && !underlying
            ? base->UnderlyingType()
            : underlying;
        if (base && base->Reference()) {
            reference = base->Reference();
        }
        return NamedTypes_[std::move(key)] = New<TNamedType>(name, completed, std::move(typeArgs), std::move(reference));
    }

    TConcreteTypePtr<TStructType> MakeStruct(std::vector<std::pair<std::string, TTypePtr>>&& fields) {
        return Intern<TStructType, &TTypeFactory::TStructTypes_>(fields, std::move(fields));
    }

    TConcreteTypePtr<TTensorType> MakeTensor(const TTypePtr& elementType, std::vector<int>&& shape) {
        return Intern<TTensorType, &TTypeFactory::TensorTypes_>(std::pair(elementType, shape), elementType, std::move(shape));
    }

    TConcreteTypePtr<TVectorType> MakeVector(const TTypePtr& elementType, int size) {
        return Intern<TVectorType, &TTypeFactory::VectorTypes_>(std::tuple(elementType, size, std::string{}), elementType, size);
    }

    // Size is a generic value parameter that is not bound yet.
    TConcreteTypePtr<TVectorType> MakeGenericVector(const TTypePtr& elementType, const std::string& sizeParam) {
        return Intern<TVectorType, &TTypeFactory::VectorTypes_>(std::tuple(elementType, 0, sizeParam), elementType, 0, sizeParam);
    }

    // Size from a bound generic value argument (0 if it is not an integer).
    TConcreteTypePtr<TVectorType> MakeVector(const TTypePtr& elementType, std::string_view sizeValue) {
        return MakeVector(elementType, TVectorType::ParseSize(sizeValue));
    }

    // A type kind Qumir does not know (tests); owned, not interned. The kind befriends
    // TTypeFactory. A host type is a named type with arguments instead, which is interned.
    template<typename T, typename... TArgs>
    requires (std::is_base_of_v<TType, T> && !IsInternedTypeKind<T>)
    TConcreteTypePtr<T> MakeCustom(TArgs&&... args) {
        return New<T>(std::forward<TArgs>(args)...);
    }

private:
    void CheckNotFrozen() const {
        if (Frozen_) {
            throw std::runtime_error("TTypeFactory: a frozen factory creates and changes no type");
        }
    }

    // `new`, not std::make_unique: the constructors are private to the factory.
    template<typename T, typename... TArgs>
    T* New(TArgs&&... args) {
        CheckNotFrozen();
        std::unique_ptr<T> type(new T(std::forward<TArgs>(args)...));
        auto* ret = type.get();
        Owned_.push_back(std::move(type));
        return ret;
    }

    // The type under `key` in this factory or its bases, else null. Reads only, so a
    // frozen base can be shared.
    template<auto Map, typename TKey>
    auto Find(const TKey& key) const -> typename std::remove_cvref_t<decltype(this->*Map)>::mapped_type {
        for (auto* types = this; types; types = types->Base_.get()) {
            const auto& map = types->*Map;
            if (auto it = map.find(key); it != map.end()) {
                return it->second;
            }
        }
        return nullptr;
    }

    // `key` is copied into the map before `args`, which may hold the same data, are moved.
    template<typename T, auto Map, typename TKey, typename... TArgs>
    TConcreteTypePtr<T> Intern(const TKey& key, TArgs&&... args) {
        if (auto found = Find<Map>(key)) {
            return static_cast<TConcreteTypePtr<T>>(found);
        }
        CheckNotFrozen();
        auto& slot = (this->*Map)[key];
        slot = New<T>(std::forward<TArgs>(args)...);
        return static_cast<TConcreteTypePtr<T>>(slot);
    }

    std::shared_ptr<const TTypeFactory> Base_;
    bool Frozen_ = false;
    std::vector<std::unique_ptr<TType>> Owned_;
    std::unordered_map<std::string_view, TTypePtr> Types_;
    std::unordered_map<TIntegerType::EKind, TConcreteTypePtr<TIntegerType>> IntegerTypes_;
    std::unordered_map<TFloatType::EKind, TConcreteTypePtr<TFloatType>> FloatTypes_;
    std::map<std::pair<std::vector<TTypePtr>, TTypePtr>, TConcreteTypePtr<TFunctionType>> FunctionTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TFutureType>> FutureTypes_;
    std::map<std::pair<TTypePtr, int>, TConcreteTypePtr<TArrayType>> ArrayTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TPointerType>> PointerTypes_;
    std::map<TTypePtr, TConcreteTypePtr<TReferenceType>> ReferenceTypes_;
    std::map<std::vector<std::pair<std::string, TTypePtr>>, TConcreteTypePtr<TStructType>> TStructTypes_;
    std::map<std::tuple<TTypePtr, int, std::string>, TConcreteTypePtr<TVectorType>> VectorTypes_;
    // Not const: MakeNamed completes these in place.
    std::map<std::pair<std::string, std::vector<TGenericArg>>, TNamedType*> NamedTypes_;
    std::map<std::pair<TTypePtr, std::vector<int>>, TConcreteTypePtr<TTensorType>> TensorTypes_;
};

} // namespace NAst
} // namespace NQumir
