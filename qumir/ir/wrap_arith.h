#pragma once

#include <cstdint>
#include <type_traits>

namespace NQumir {
namespace NIR {

// Integer add/sub/mul/neg wrap around like LLVM's non-nsw ops; signed C++
// arithmetic would be undefined behaviour on overflow.
template<typename T>
using TWrapUnsigned = std::conditional_t<sizeof(T) == 16, __uint128_t, uint64_t>;

template<typename T>
struct TWrapPlus {
    T operator()(T a, T b) const { return static_cast<T>(static_cast<TWrapUnsigned<T>>(a) + static_cast<TWrapUnsigned<T>>(b)); }
};

template<typename T>
struct TWrapMinus {
    T operator()(T a, T b) const { return static_cast<T>(static_cast<TWrapUnsigned<T>>(a) - static_cast<TWrapUnsigned<T>>(b)); }
};

template<typename T>
struct TWrapMultiplies {
    T operator()(T a, T b) const { return static_cast<T>(static_cast<TWrapUnsigned<T>>(a) * static_cast<TWrapUnsigned<T>>(b)); }
};

template<typename T>
T WrapNeg(T a) {
    return static_cast<T>(TWrapUnsigned<T>{0} - static_cast<TWrapUnsigned<T>>(a));
}

} // namespace NIR
} // namespace NQumir
