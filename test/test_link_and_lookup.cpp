#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/codegen/llvm/llvm_runner.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <sstream>
#include <string>

using namespace NQumir;

TEST(LinkAndLookup, LoadsPrebuiltObjectAndRuns) {
    std::istringstream in("(block (fun f () -> i64 (block (return (: 42 i64)))))");
    NAst::NCore::TTokenStream tokens(in);
    NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    TLLVMRunner compiler({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .OptLevel = 0,
    });
    std::string err;
    auto obj = compiler.CompileKernelAstToObject(*parsed, {"f"}, &err);
    ASSERT_TRUE(obj.has_value()) << err;

    NCodeGen::TLlvmRunner jit;
    auto linked = jit.LinkAndLookup({}, {*obj}, nullptr, /*nativeCode=*/true, {"f"}, &err);
    ASSERT_EQ(linked.Entries.size(), 1u) << err;
    auto* f = reinterpret_cast<int64_t (*)()>(linked.Entries["f"]);
    EXPECT_EQ(f(), 42);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
