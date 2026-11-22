#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <sstream>
#include <string>

using namespace NQumir;

// Two cacheable overloads sharing a source name but differing in parameter type
// must resolve independently (distinct signature-mangled symbols) - the string
// key scenario where rh_hash exists for both the lookup and stored key types.
TEST(CacheableMangle, OverloadedCacheableResolveByType) {
    std::istringstream in(
        "(block"
        "  (fun h ((var x i64)) -> i64 (attrs cacheable) (block (return (+ x (: 1 i64)))))"
        "  (fun h ((var x bool)) -> i64 (attrs cacheable) (block (return (: 2 i64))))"
        "  (fun caller () -> i64 (block"
        "    (return (+ (call h (: 40 i64)) (call h (< (: 0 i64) (: 1 i64))))))))");
    NAst::NCore::TTokenStream tokens(in);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    TLLVMRunner runner({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .OptLevel = 0,
    });
    std::string err;
    void* entry = runner.CompileKernelAst(*parsed, "caller", &err);
    ASSERT_NE(entry, nullptr) << err;
    auto* caller = reinterpret_cast<int64_t (*)()>(entry);
    EXPECT_EQ(caller(), 43); // h(40:i64)=41 + h(true:bool)=2
}

// Cacheable mangling must not depend on AllowOverloads: a sole cacheable
// function still compiles and links under its mangled emit symbol.
TEST(CacheableMangle, CacheableWithoutOverloads) {
    std::istringstream in(
        "(block"
        "  (fun g ((var x i64)) -> i64 (attrs cacheable) (block (return (+ x (: 2 i64)))))"
        "  (fun caller () -> i64 (block (return (call g (: 40 i64))))))");
    NAst::NCore::TTokenStream tokens(in);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    TLLVMRunner runner({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = false,
        .OptLevel = 0,
    });
    std::string err;
    void* entry = runner.CompileKernelAst(*parsed, "caller", &err);
    ASSERT_NE(entry, nullptr) << err;
    auto* caller = reinterpret_cast<int64_t (*)()>(entry);
    EXPECT_EQ(caller(), 42);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
