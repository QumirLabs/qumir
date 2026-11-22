#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace NQumir {

// simple refcounted base class for intrusive smart pointers
// if you need atomic refcountingm create TAtomicRefCounted<TDerived>
template<typename TDerived>
struct TRefCounted {
    TRefCounted() noexcept = default;

    TRefCounted(const TRefCounted&) noexcept {}
    TRefCounted& operator=(const TRefCounted&) noexcept { return *this; }

    // const: the count is bookkeeping, not part of the object's value, so pointers to
    // const objects (TIntrusivePtr<const T>) can still share and release them.
    void Ref() const noexcept {
        ++Count;
    }

    void Unref() const noexcept {
        if (--Count == 0) {
            delete static_cast<const TDerived*>(this);
        }
    }

    mutable int Count = 0;
};

template<typename T>
struct TIntrusivePtr {
    constexpr TIntrusivePtr() noexcept : Ptr(nullptr) {
    }

    constexpr TIntrusivePtr(std::nullptr_t) noexcept : Ptr(nullptr) {
    }

    explicit TIntrusivePtr(T* ptr) noexcept : Ptr(ptr) {
        if (Ptr) {
            Ptr->Ref();
        }
    }

    TIntrusivePtr(const TIntrusivePtr& other) noexcept : Ptr(other.Ptr) {
        if (Ptr) {
            Ptr->Ref();
        }
    }

    TIntrusivePtr(TIntrusivePtr&& other) noexcept : Ptr(other.Ptr) {
        other.Ptr = nullptr;
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    TIntrusivePtr(const TIntrusivePtr<U>& other) noexcept : Ptr(other.Ptr) {
        if (Ptr) {
            Ptr->Ref();
        }
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    TIntrusivePtr(TIntrusivePtr<U>&& other) noexcept : Ptr(other.Ptr) {
        other.Ptr = nullptr;
    }

    constexpr ~TIntrusivePtr() {
        if (Ptr) {
            Ptr->Unref();
        }
    }

    TIntrusivePtr& operator=(TIntrusivePtr other) noexcept {
        std::swap(Ptr, other.Ptr);
        return *this;
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    TIntrusivePtr& operator=(const TIntrusivePtr<U>& other) noexcept {
        TIntrusivePtr(other).swap(*this);
        return *this;
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    TIntrusivePtr& operator=(TIntrusivePtr<U>&& other) noexcept {
        TIntrusivePtr(std::move(other)).swap(*this);
        return *this;
    }

    void swap(TIntrusivePtr& other) noexcept {
        std::swap(Ptr, other.Ptr);
    }

    bool operator==(const TIntrusivePtr& other) const noexcept {
        return Ptr == other.Ptr;
    }

    std::strong_ordering operator<=>(const TIntrusivePtr& other) const noexcept {
        return std::compare_three_way{}(Ptr, other.Ptr);
    }

    bool operator==(const T* other) const noexcept {
        return Ptr == other;
    }

    std::strong_ordering operator<=>(const T* other) const noexcept {
        return std::compare_three_way{}(Ptr, other);
    }

    T* Get() const noexcept {
        return Ptr;
    }

    T* operator->() const noexcept {
        return Ptr;
    }

    T& operator*() const noexcept {
        return *Ptr;
    }

    explicit operator bool() const noexcept {
        return Ptr != nullptr;
    }

    T* Ptr;
};

template<typename T, typename... Args>
TIntrusivePtr<T> MakeIntrusive(Args&&... args) {
    return TIntrusivePtr<T>(new T(std::forward<Args>(args)...));
}

template<typename To, typename From>
TIntrusivePtr<To> StaticPointerCast(const TIntrusivePtr<From>& from) noexcept {
    return TIntrusivePtr<To>(static_cast<To*>(from.Get()));
}

} // namespace NQumir

template<typename T>
struct std::hash<NQumir::TIntrusivePtr<T>> {
    size_t operator()(const NQumir::TIntrusivePtr<T>& ptr) const noexcept {
        return std::hash<T*>{}(ptr.Get());
    }
};
