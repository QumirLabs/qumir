#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <type_traits>

namespace NQumir {
namespace NAst {

struct TOperator {
    constexpr TOperator(uint64_t v)
        : Value(v)
    {}

    constexpr TOperator(char ch)
        : Value(static_cast<uint64_t>(ch))
    {}

    constexpr TOperator(const std::string_view& s)
        : Value(0)
    {
        // Only the last 8 bytes fit: a longer constant label (NodeId, TypeId) must not compile.
        if (std::is_constant_evaluated() && s.size() > sizeof(Value)) {
            throw std::length_error("TOperator packs at most 8 bytes");
        }
        for (char ch : s) {
            Value = (Value << 8) | static_cast<uint64_t>(ch);
        }
    }

    constexpr operator uint64_t() const {
        return Value;
    }

    // The implicit uint64_t would let a stale `type->Kind == TFloatType::F32` compile as always false.
    template<typename E>
    requires std::is_enum_v<E>
    bool operator==(E) const = delete;

    constexpr bool operator==(const char* s) const {
        uint64_t v = 0;
        for (const char* p = s; *p != '\0'; ++p) {
            v = (v << 8) | static_cast<uint64_t>(*p);
        }
        return v == Value;
    }

    std::string ToString() const {
        std::string s;
        uint64_t v = Value;
        while (v != 0) {
            s = static_cast<char>(v & 0xFF) + s;
            v >>= 8;
        }
        return s;
    }

    uint64_t Value;
};

namespace NLiterals {

inline TOperator operator""_op(const char* s, size_t) {
    return TOperator(s);
}

} // namespace NLiterals

} // namespace NAst
} // namespace NQumir

template<>
struct std::hash<NQumir::NAst::TOperator> {
    size_t operator()(NQumir::NAst::TOperator op) const noexcept {
        return std::hash<uint64_t>{}(op.Value);
    }
};