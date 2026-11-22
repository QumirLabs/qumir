#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <string>

namespace {

constexpr const char* WordHashSource = R"(
(block
  (fun word_hash ((var data <ptr u8>) (var size i64)) -> i64
    (block
      (var hash = (cast (: -3750763034362895579 i64) u64))
      (var words = (cast data <ptr u64>))
      (var index i64)
      (= index (: 0 i64))
      (while (! (< size (+ index (: 8 i64))))
        (block
          (var word = (index words (>> index (: 3 i64))))
          (= hash (^ hash word))
          (= hash (* hash (: 1099511628211 u64)))
          (= index (+ index (: 8 i64)))))
      (while (< index size)
        (block
          (= hash (^ hash (cast (index data index) u64)))
          (= hash (* hash (: 1099511628211 u64)))
          (= index (+ index (: 1 i64)))))
      (return (cast hash i64))))))";

// kernel is the entry; keep is `used`; dead is reachable from neither.
constexpr const char* RootsSource = R"(
(block
  (fun kernel ((var x i64)) -> i64 (block (return (+ x 1))))
  (fun helper ((var x i64)) -> i64 (block (return (* x 3))))
  (fun keep ((var x i64)) -> i64 (attrs used) (block (return (call helper x))))
  (fun dead ((var x i64)) -> i64 (block (return (* x 5)))))
)";

// Compiles source to a kernel object with entry "kernel" and returns the final LLVM IR.
std::string CompileKernelObjectIr(const char* source, int optLevel)
{
    std::istringstream input(source);
    NQumir::NAst::NCore::TTokenStream tokens(input);
    NQumir::NAst::NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(tokens);
    EXPECT_TRUE(parsed) << parsed.error().ToString();
    if (!parsed) {
        return {};
    }
    NQumir::TLLVMRunner runner({
        .PrintLlvm = true,
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .OptLevel = optLevel,
    });
    std::string error;
    testing::internal::CaptureStderr();
    auto object = runner.CompileKernelAstToObject(*parsed, {"kernel"}, &error);
    auto ir = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(object && !object->empty()) << error;
    return ir;
}

} // namespace

TEST(KernelAst, LoadsU64FromPointerIndex) {
    std::istringstream input(WordHashSource);
    NQumir::NAst::NCore::TTokenStream tokens(input);
    NQumir::TLLVMRunner runner({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .EnablePerfJitEventListener = true,
        .OptLevel = 3,
    });
    NQumir::NAst::NCore::TParser parser(runner.TypeFactory());
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();


    std::string error;
    void* entry = runner.CompileKernelAst(*parsed, "word_hash", &error);
    ASSERT_NE(entry, nullptr) << error;

    auto wordHash = reinterpret_cast<int64_t(*)(uint8_t*, int64_t)>(entry);
    std::array<uint8_t, 8> data = {
        208, 186, 208, 187, 209, 142, 209, 135,
    };

    EXPECT_EQ(wordHash(data.data(), static_cast<int64_t>(data.size())),
        INT64_C(5922767304251906895));
}

TEST(KernelAst, UsedFunctionStaysExportedAfterO3) {
    auto ir = CompileKernelObjectIr(RootsSource, 3);
    EXPECT_NE(ir.find("define i64 @kernel("), std::string::npos) << ir;
    EXPECT_NE(ir.find("define i64 @keep("), std::string::npos) << ir;
}

TEST(KernelAst, LowersOnlyFunctionsReachableFromEntriesAndUsed) {
    auto ir = CompileKernelObjectIr(RootsSource, 0);
    EXPECT_NE(ir.find("define internal i64 @helper("), std::string::npos) << ir;
    EXPECT_EQ(ir.find("@dead("), std::string::npos) << ir;
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
