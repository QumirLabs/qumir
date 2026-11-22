#include <qumir/codegen/llvm/llvm_initializer.h>
#include "test_types.h"
#include <qumir/modules/builtins/builtins.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_ir.h>
#include <qumir/runner/runner_llvm.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace NQumir;
namespace fs = std::filesystem;

namespace {

constexpr const char* Source = R"(
(block
  (fun match8 ((var data <ptr u8>) (var byte u8)) -> u64
    (block (return (call builtin::byte_match8 data byte))))
  (fun match16 ((var data <ptr u8>) (var byte u8)) -> u32
    (block (return (call builtin::byte_match16 data byte))))
  (fun backend () -> i64
    (block (return (call builtin::byte_match_backend)))))
)";

NAst::TExprPtr Parse(const std::string& source) {
    std::istringstream input(source);
    NAst::NCore::TTokenStream tokens(input);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    return parsed
        ? *parsed
        : nullptr;
}

template<class TMatch8, class TMatch16>
void CheckEveryByte(TMatch8 match8, TMatch16 match16) {
    alignas(64) std::array<uint8_t, 48> storage;
    for (unsigned offset = 0; offset < 16; ++offset) {
        auto* bytes = storage.data() + offset;
        for (unsigned needle = 0; needle < 256; ++needle) {
            SCOPED_TRACE("offset=" + std::to_string(offset) + " byte=" + std::to_string(needle));
            storage.fill(static_cast<uint8_t>(needle ^ 0xff));
            ASSERT_EQ(match8(bytes, needle), 0u);
            ASSERT_EQ(match16(bytes, needle), 0u);
            for (unsigned lane = 0; lane < 16; ++lane) {
                bytes[lane] = static_cast<uint8_t>(needle);
                const uint64_t expected8 = lane < 8
                    ? UINT64_C(0xff) << (8 * lane)
                    : 0;
                ASSERT_EQ(match8(bytes, needle), expected8) << "lane=" << lane;
                ASSERT_EQ(match16(bytes, needle), uint32_t{1} << lane) << "lane=" << lane;
                bytes[lane] = static_cast<uint8_t>(needle ^ 0xff);
            }
        }
    }
}

template<class TMatch8, class TMatch16>
void CheckAllSubsets(TMatch8 match8, TMatch16 match16) {
    std::array<uint8_t, 17> storage;
    auto* bytes = storage.data() + 1;
    for (uint32_t subset = 0; subset < 65536; ++subset) {
        uint64_t expected8 = 0;
        for (unsigned lane = 0; lane < 16; ++lane) {
            const bool hit = (subset & (uint32_t{1} << lane)) != 0;
            bytes[lane] = hit
                ? 0x80
                : 0x81;
            if (hit && lane < 8) {
                expected8 |= UINT64_C(0xff) << (8 * lane);
            }
        }
        const uint64_t actual8 = match8(bytes, 0x80);
        const uint32_t actual16 = match16(bytes, 0x80);
        ASSERT_EQ(actual8, expected8) << "subset=" << subset;
        ASSERT_EQ(actual16, subset);

        // One flag per matching byte lets mask &= mask - 1 visit each lane once.
        uint32_t visited8 = 0;
        for (uint64_t mask = actual8 & UINT64_C(0x8080808080808080); mask; mask &= mask - 1) {
            visited8 |= uint32_t{1} << (std::countr_zero(mask) / 8);
        }
        EXPECT_EQ(visited8, subset & 0xff);
        uint32_t visited16 = 0;
        for (uint32_t mask = actual16; mask; mask &= mask - 1) {
            visited16 |= uint32_t{1} << std::countr_zero(mask);
        }
        EXPECT_EQ(visited16, subset);
    }
}

class TByteMatchJit : public testing::TestWithParam<std::tuple<int, bool>> {
protected:
    using TMatch8 = uint64_t (*)(const uint8_t*, uint8_t);
    using TMatch16 = uint32_t (*)(const uint8_t*, uint8_t);
    using TBackend = int64_t (*)();

    void SetUp() override {
        Runner = std::make_unique<TLLVMRunner>(TLLVMRunnerOptions{
            .NativeCode = std::get<1>(GetParam()),
            .CoreInput = true,
            .AllowOverloads = true,
            .OptLevel = std::get<0>(GetParam()),
        });
        std::string error;
        auto entries = Runner->CompileKernelAst(
            Parse(Source),
            {"match8", "match16", "backend"},
            &error);
        ASSERT_EQ(entries.size(), 3u) << error;
        Match8 = reinterpret_cast<TMatch8>(entries.at("match8"));
        Match16 = reinterpret_cast<TMatch16>(entries.at("match16"));
        Backend = reinterpret_cast<TBackend>(entries.at("backend"));
    }

    std::unique_ptr<TLLVMRunner> Runner;
    TMatch8 Match8 = nullptr;
    TMatch16 Match16 = nullptr;
    TBackend Backend = nullptr;
};

TEST_P(TByteMatchJit, EveryByteAndUnalignedAddress) {
    CheckEveryByte(Match8, Match16);
}

TEST_P(TByteMatchJit, AllSubsetsAndMaskIteration) {
    CheckAllSubsets(Match8, Match16);
}

TEST_P(TByteMatchJit, NativeBackend) {
    NRegistry::BuiltinsModule builtins(NQumir::NTest::TestTypes());
    for (const auto& function : builtins.ExternalFunctions()) {
        if (function.Name == "builtin::byte_match_backend") {
            EXPECT_EQ(Backend(), function.Packed(nullptr, 0));
            return;
        }
    }
    FAIL() << "backend builtin was not registered";
}

TEST_P(TByteMatchJit, ReadsExactlyTheGroupWidth) {
#if defined(__unix__) || defined(__APPLE__)
    const auto pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    void* mapping = ::mmap(
        nullptr,
        2 * pageSize,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    ASSERT_NE(mapping, MAP_FAILED);
    struct TMapping {
        void* Address;
        size_t Size;
        ~TMapping() {
            ::munmap(Address, Size);
        }
    } guard{mapping, 2 * pageSize};
    auto* end = static_cast<uint8_t*>(mapping) + pageSize;
    ASSERT_EQ(::mprotect(end, pageSize, PROT_NONE), 0);
    std::fill(end - 16, end, uint8_t{0x80});
    EXPECT_EQ(Match8(end - 8, 0x80), UINT64_MAX);
    EXPECT_EQ(Match16(end - 16, 0x80), 0xffffu);
#else
    GTEST_SKIP() << "guard pages require mmap";
#endif
}

INSTANTIATE_TEST_SUITE_P(
    OptimizationAndCpu,
    TByteMatchJit,
    testing::Combine(testing::Values(0, 3), testing::Bool()));

TEST(ByteMatch, PackedInterpreterFallback) {
    NRegistry::BuiltinsModule builtins(NQumir::NTest::TestTypes());
    std::unordered_map<std::string, NRegistry::TExternalFunction::TPacked> packed;
    for (const auto& function : builtins.ExternalFunctions()) {
        packed.emplace(function.Name, function.Packed);
    }
    const auto match8 = packed.at("builtin::byte_match8");
    const auto match16 = packed.at("builtin::byte_match16");
    const auto call = [](auto match, const uint8_t* bytes, uint8_t byte) {
        const uint64_t args[]{std::bit_cast<uint64_t>(bytes), byte};
        return match(args, 2);
    };
    const auto call8 = [&](const uint8_t* bytes, uint8_t byte) {
        return call(match8, bytes, byte);
    };
    const auto call16 = [&](const uint8_t* bytes, uint8_t byte) {
        return call(match16, bytes, byte);
    };
    CheckEveryByte(call8, call16);
    CheckAllSubsets(call8, call16);
}

TEST(ByteMatch, CoreInterpreterCallsBuiltins) {
    std::array<uint8_t, 16> bytes{};
    bytes[1] = 0x80;
    const auto address = reinterpret_cast<uintptr_t>(bytes.data());
    const std::string source = "(block (fun <main> () -> i64"
        " (block (var data = (cast (: " + std::to_string(address) + " u64) <ptr u8>))"
        " (return (+ (cast (call builtin::byte_match8 data (: 128 u8)) i64)"
        " (cast (call builtin::byte_match16 data (: 128 u8)) i64))))))";
    std::istringstream input(source);
    std::istringstream stdinInput;
    std::ostringstream output;
    TIRRunner runner(output, stdinInput, {.CoreInput = true});
    const auto result = runner.Run(input);
    ASSERT_TRUE(result) << result.error().ToString();
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(**result, "65282");
}

TEST(ByteMatch, PortableVectorIrAndWasmBackend) {
    for (const auto& triple : {std::string{}, std::string{"wasm32-unknown-unknown"}}) {
        TLLVMRunner runner({
            .PrintLlvm = true,
            .NativeCode = false,
            .CoreInput = true,
            .AllowOverloads = true,
            .OptLevel = 0,
            .TargetTriple = triple,
        });
        std::string error;
        testing::internal::CaptureStderr();
        auto object = runner.CompileKernelAstToObject(
            Parse(Source),
            {"match8", "match16", "backend"},
            &error);
        const auto ir = testing::internal::GetCapturedStderr();
        ASSERT_TRUE(object) << error;
        EXPECT_FALSE(object->empty());
        EXPECT_NE(ir.find("load <8 x i8>, ptr"), std::string::npos);
        EXPECT_NE(ir.find("load <16 x i8>, ptr"), std::string::npos);
        EXPECT_NE(ir.find("icmp eq <8 x i8>"), std::string::npos);
        EXPECT_NE(ir.find("icmp eq <16 x i8>"), std::string::npos);
        EXPECT_EQ(ir.find("@qumir_builtin_byte_match"), std::string::npos);
        if (!triple.empty()) {
            std::smatch backend;
            ASSERT_TRUE(std::regex_search(
                ir,
                backend,
                std::regex("define[^\\n]*backend[^\\n]*\\{[\\s\\S]*?\\n\\}")));
            EXPECT_NE(backend.str().find("store i64 0, ptr"), std::string::npos) << backend.str();
        }
    }
}

TEST(ByteMatch, NativeSimdAssembly) {
    TLLVMRunner runner({
        .PrintAsm = true,
        .NativeCode = false,
        .CoreInput = true,
        .AllowOverloads = true,
        .OptLevel = 3,
    });
    std::string error;
    testing::internal::CaptureStderr();
    auto object = runner.CompileKernelAstToObject(
        Parse(Source),
        {"match8", "match16", "backend"},
        &error);
    const auto assembly = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(object) << error;
#if defined(__SSE2__)
    EXPECT_NE(assembly.find("pcmpeqb"), std::string::npos);
    EXPECT_NE(assembly.find("pmovmskb"), std::string::npos);
#elif defined(__aarch64__) && defined(__ARM_NEON)
    EXPECT_NE(assembly.find("cmeq"), std::string::npos);
#endif
    EXPECT_EQ(assembly.find("qumir_builtin_byte_match"), std::string::npos);
}

TEST(ByteMatch, ColdAndWarmInlineObjectCache) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = fs::temp_directory_path() / ("qumir-byte-match-" + std::to_string(nonce));
    fs::create_directories(directory);
    struct TCacheDirectory {
        fs::path Directory;
        ~TCacheDirectory() {
            std::error_code error;
            fs::remove_all(Directory, error);
        }
    } guard{directory};
    constexpr const char* source = R"(
    (block
      (fun match8 ((var data <ptr u8>) (var byte u8)) -> u64 (attrs cacheable inline)
        (block (return (call builtin::byte_match8 data byte))))
      (fun match16 ((var data <ptr u8>) (var byte u8)) -> u32 (attrs cacheable inline)
        (block (return (call builtin::byte_match16 data byte))))
      (fun backend () -> i64 (attrs cacheable inline)
        (block (return (call builtin::byte_match_backend))))
      (fun kernel8 ((var data <ptr u8>) (var byte u8)) -> u64
        (block (return (call match8 data byte))))
      (fun kernel16 ((var data <ptr u8>) (var byte u8)) -> u32
        (block (return (call match16 data byte))))
      (fun kernel_backend () -> i64 (block (return (call backend))))
    ))";
    std::array<uint8_t, 16> bytes{};
    bytes[7] = 0x80;
    bytes[15] = 0x80;
    size_t cachedObjects = 0;
    for (int pass = 0; pass < 2; ++pass) {
        TLLVMRunner runner({.NativeCode = true, .CoreInput = true, .AllowOverloads = true, .OptLevel = 3});
        std::string error;
        auto linked = runner.CompileFusedKernelsCached(
            Parse(source),
            {"kernel8", "kernel16", "kernel_backend"},
            directory.string(),
            "v1",
            "byte-match",
            &error);
        ASSERT_EQ(linked.Entries.size(), 3u) << error;
        const auto match8 = reinterpret_cast<uint64_t (*)(const uint8_t*, uint8_t)>(linked.Entries.at("kernel8"));
        const auto match16 = reinterpret_cast<uint32_t (*)(const uint8_t*, uint8_t)>(linked.Entries.at("kernel16"));
        const auto backend = reinterpret_cast<int64_t (*)()>(linked.Entries.at("kernel_backend"));
        EXPECT_EQ(match8(bytes.data(), 0x80), UINT64_C(0xff00000000000000));
        EXPECT_EQ(match16(bytes.data(), 0x80), 0x8080u);
        EXPECT_GE(backend(), 0);
        EXPECT_LE(backend(), 2);
        size_t objectCount = 0;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            objectCount += entry.path().extension() == ".o";
        }
        if (pass == 0) {
            cachedObjects = objectCount;
            EXPECT_EQ(cachedObjects, 3u);
        } else {
            EXPECT_EQ(objectCount, cachedObjects);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
