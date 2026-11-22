#include "builtins.h"

#include <qumir/runtime/runtime.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NQumir {
namespace NRegistry {

BuiltinsModule::BuiltinsModule() {
    auto i64Type = MakeIntrusive<NAst::TIntegerType>();
    auto i32Type = MakeIntrusive<NAst::TIntegerType>(NAst::TIntegerType::I32);
    auto u8Type = MakeIntrusive<NAst::TIntegerType>(NAst::TIntegerType::U8);
    auto u32Type = MakeIntrusive<NAst::TIntegerType>(NAst::TIntegerType::U32);
    auto u64Type = MakeIntrusive<NAst::TIntegerType>(NAst::TIntegerType::U64);
    auto ptrU8Type = MakeIntrusive<NAst::TPointerType>(u8Type);
    auto voidType = MakeIntrusive<NAst::TVoidType>();

    ExternalFunctions_ = {
        {
            .Name = "builtin::memcpy",
            .MangledName = "memcpy",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                std::memcpy(
                    std::bit_cast<void*>(args[0]),
                    std::bit_cast<const void*>(args[1]),
                    static_cast<size_t>(args[2]));
                return args[0];
            },
            .ArgTypes = { ptrU8Type, ptrU8Type, i64Type },
            .ReturnType = ptrU8Type,
        },
        {
            .Name = "builtin::memmove",
            .MangledName = "memmove",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                std::memmove(
                    std::bit_cast<void*>(args[0]),
                    std::bit_cast<const void*>(args[1]),
                    static_cast<size_t>(args[2]));
                return args[0];
            },
            .ArgTypes = { ptrU8Type, ptrU8Type, i64Type },
            .ReturnType = ptrU8Type,
        },
        {
            .Name = "builtin::memcmp",
            .MangledName = "memcmp",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                const int result = std::memcmp(
                    std::bit_cast<const void*>(args[0]),
                    std::bit_cast<const void*>(args[1]),
                    static_cast<size_t>(args[2]));
                return static_cast<uint64_t>(static_cast<int64_t>(result));
            },
            .ArgTypes = { ptrU8Type, ptrU8Type, i64Type },
            .ReturnType = i32Type,
        },
        {
            // LLVM folds this using target features; the interpreter uses the host.
            .Name = "builtin::byte_match_backend",
            .MangledName = "qumir_builtin_byte_match_backend",
            .Packed = +[](const uint64_t*, size_t) -> uint64_t {
#if defined(__SSE2__)
                return 2;
#elif defined(__aarch64__) && defined(__ARM_NEON)
                return 1;
#else
                return 0;
#endif
            },
            .ArgTypes = {},
            .ReturnType = i64Type,
        },
        {
            .Name = "builtin::byte_match8",
            .MangledName = "qumir_builtin_byte_match8",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                const auto* bytes = std::bit_cast<const uint8_t*>(args[0]);
                const auto byte = static_cast<uint8_t>(args[1]);
                uint64_t mask = 0;
                for (unsigned i = 0; i < 8; ++i) {
                    if (bytes[i] == byte) {
                        mask |= uint64_t{0xff} << (8 * i);
                    }
                }
                return mask;
            },
            .ArgTypes = {ptrU8Type, u8Type},
            .ReturnType = u64Type,
        },
        {
            .Name = "builtin::byte_match16",
            .MangledName = "qumir_builtin_byte_match16",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                const auto* bytes = std::bit_cast<const uint8_t*>(args[0]);
                const auto byte = static_cast<uint8_t>(args[1]);
                uint64_t mask = 0;
                for (unsigned i = 0; i < 16; ++i) {
                    if (bytes[i] == byte) {
                        mask |= uint64_t{1} << i;
                    }
                }
                return mask;
            },
            .ArgTypes = {ptrU8Type, u8Type},
            .ReturnType = u32Type,
        },
        {
            .Name = "builtin::cttz",
            .MangledName = "qumir_builtin_cttz",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(std::countr_zero(args[0]));
            },
            .ArgTypes = { u64Type },
            .ReturnType = i64Type,
        },
        {
            .Name = "builtin::ctlz",
            .MangledName = "qumir_builtin_ctlz",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(std::countl_zero(args[0]));
            },
            .ArgTypes = { u64Type },
            .ReturnType = i64Type,
        },
        {
            .Name = "builtin::ctpop",
            .MangledName = "qumir_builtin_ctpop",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(std::popcount(args[0]));
            },
            .ArgTypes = { u64Type },
            .ReturnType = i64Type,
        },
        {
            .Name = "builtin::record_locator",
            .MangledName = "__record_locator",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                __record_locator(static_cast<int64_t>(args[0]), static_cast<int64_t>(args[1]), static_cast<int64_t>(args[2]));
                return 0;
            },
            .ArgTypes = { i64Type, i64Type, i64Type },
            .ReturnType = voidType,
        },
    };
}

} // namespace NRegistry
} // namespace NQumir
