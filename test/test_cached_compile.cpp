#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <filesystem>
#include <regex>
#include <sstream>
#include <string>

using namespace NQumir;
namespace fs = std::filesystem;

namespace {

struct TCacheDir {
    fs::path Dir = fs::temp_directory_path() / fs::path("qdbcompile-" + std::to_string(::getpid()) + "-" + std::to_string(rand()));
    TCacheDir() { fs::create_directories(Dir); }
    ~TCacheDir() { std::error_code ec; fs::remove_all(Dir, ec); }
    std::string Str() const { return Dir.string(); }
};

int CountObjects(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() == ".o") {
            ++n;
        }
    }
    return n;
}

// dep() = 40 is a cacheable dependency; kernel() = dep() + 2 links against it.
constexpr const char* Source =
    "(block"
    "  (fun dep () -> i64 (attrs cacheable) (block (return (: 40 i64))))"
    "  (fun kernel () -> i64 (block (return (+ (call dep) (: 2 i64))))))";

NCodeGen::TLlvmRunner::TLinkedModule Compile(
    const std::string& cacheDir, const char* source, const std::string& entry,
    std::string* err, int optLevel = 0, bool printLlvm = false)
{
    std::istringstream in(source);
    NAst::NCore::TTokenStream tokens(in);
    TLLVMRunner runner({
        .PrintLlvm = printLlvm,
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .OptLevel = optLevel,
    });
    NAst::NCore::TParser parser(runner.TypeFactory());
    auto parsed = parser.Parse(tokens);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    if (!parsed) {
        return {};
    }
    return runner.CompileFusedKernelsCached(*parsed, {entry}, cacheDir, "v1", "k1", err);
}

} // namespace

TEST(CachedCompile, MissCompilesAndPersists) {
    TCacheDir cache;
    std::string err;
    auto linked = Compile(cache.Str(), Source, "kernel", &err);
    ASSERT_FALSE(linked.Entries.empty()) << err;

    auto* kernel = reinterpret_cast<int64_t (*)()>(linked.Entries["kernel"]);
    EXPECT_EQ(kernel(), 42); // dep() + 2, dep linked from a separate object
    EXPECT_EQ(CountObjects(cache.Dir), 1); // the dependency object was persisted
}

TEST(CachedCompile, HitReusesObjectAcrossRunners) {
    TCacheDir cache;
    std::string err;

    auto first = Compile(cache.Str(), Source, "kernel", &err);
    ASSERT_FALSE(first.Entries.empty()) << err;
    ASSERT_EQ(CountObjects(cache.Dir), 1);

    // Fresh runner, same cache dir: dep is a hit, no new object is compiled.
    auto second = Compile(cache.Str(), Source, "kernel", &err);
    ASSERT_FALSE(second.Entries.empty()) << err;
    auto* kernel = reinterpret_cast<int64_t (*)()>(second.Entries["kernel"]);
    EXPECT_EQ(kernel(), 42);
    EXPECT_EQ(CountObjects(cache.Dir), 1); // still one object: cache hit
}

// A cacheable function that itself calls a cacheable function, where the callee
// is a cache hit and the caller a miss: the miss object references the hit
// object across the split. Works because each compile re-derives the full
// (monomorphized) cacheable set, so Resolve pulls both objects.
TEST(CachedCompile, TransitiveCacheableAcrossObjects) {
    TCacheDir cache;
    std::string err;

    // Run 1: only B, so B lands in its own cached object.
    constexpr const char* onlyB =
        "(block"
        "  (fun B () -> i64 (attrs cacheable) (block (return (: 40 i64))))"
        "  (fun kb () -> i64 (block (return (call B)))))";
    auto b = Compile(cache.Str(), onlyB, "kb", &err);
    ASSERT_FALSE(b.Entries.empty()) << err;
    const int afterB = CountObjects(cache.Dir);
    ASSERT_EQ(afterB, 1);

    // Run 2: A calls B; B is a hit, A a miss -> A's object references B's object.
    constexpr const char* aCallsB =
        "(block"
        "  (fun B () -> i64 (attrs cacheable) (block (return (: 40 i64))))"
        "  (fun A () -> i64 (attrs cacheable) (block (return (call B))))"
        "  (fun kernel () -> i64 (block (return (+ (call A) (: 2 i64))))))";
    auto k = Compile(cache.Str(), aCallsB, "kernel", &err);
    ASSERT_FALSE(k.Entries.empty()) << err; // empty would mean the cross-object link failed
    auto* kernel = reinterpret_cast<int64_t (*)()>(k.Entries["kernel"]);
    EXPECT_EQ(kernel(), 42); // A() = B() = 40, + 2
    EXPECT_EQ(CountObjects(cache.Dir), 2); // B.o reused, A.o added
}

// Two cacheable overloads (distinct signature-mangled symbols) are persisted
// and reused: a fresh runner hits both, adding no new objects.
TEST(CachedCompile, OverloadedCacheableReused) {
    TCacheDir cache;
    std::string err;
    constexpr const char* src =
        "(block"
        "  (fun h ((var x i64)) -> i64 (attrs cacheable) (block (return (+ x (: 1 i64)))))"
        "  (fun h ((var x bool)) -> i64 (attrs cacheable) (block (return (: 2 i64))))"
        "  (fun kernel () -> i64 (block"
        "    (return (+ (call h (: 40 i64)) (call h (< (: 0 i64) (: 1 i64))))))))";

    auto first = Compile(cache.Str(), src, "kernel", &err);
    ASSERT_FALSE(first.Entries.empty()) << err;
    EXPECT_EQ(reinterpret_cast<int64_t (*)()>(first.Entries["kernel"])(), 43);
    const int after = CountObjects(cache.Dir);
    ASSERT_GE(after, 1);

    auto second = Compile(cache.Str(), src, "kernel", &err);
    ASSERT_FALSE(second.Entries.empty()) << err;
    EXPECT_EQ(reinterpret_cast<int64_t (*)()>(second.Entries["kernel"])(), 43);
    EXPECT_EQ(CountObjects(cache.Dir), after); // both overloads were cache hits
}

TEST(CachedCompile, OptimizedKernelInlinesCachedDependenciesOnMissAndHit) {
    TCacheDir cache;
    constexpr const char* source = R"(
      (block
        (fun leaf ((var x i64)) -> i64 (attrs cacheable)
          (block (return (+ (* x 3) 1))))
        (fun dep ((var x i64)) -> i64 (attrs cacheable)
          (block (return (+ (call leaf x) 7))))
        (fun kernel ((var data <ptr i64>) (var n i64)) -> i64
          (block
            (var i = 0)
            (var sum = 0)
            (while (< i n)
              (block
                (= sum (+ sum (call dep (index data i))))
                (= i (+ i 1))))
            (return sum)))))";
    for (int run = 0; run < 2; ++run) {
        std::string err;
        testing::internal::CaptureStderr();
        auto linked = Compile(cache.Str(), source, "kernel", &err, 3, true);
        const auto ir = testing::internal::GetCapturedStderr();
        ASSERT_FALSE(linked.Entries.empty()) << err;
        int64_t values[] = {2, -5, 19, 0};
        auto* kernel = reinterpret_cast<int64_t (*)(int64_t*, int64_t)>(
            linked.Entries.at("kernel"));
        EXPECT_EQ(kernel(values, 4), 80);
        EXPECT_EQ(kernel(values, 0), 0);
        EXPECT_EQ(CountObjects(cache.Dir), 2);
        // Inspect the final query module, not a separately compiled dependency.
        const auto entry = ir.rfind("@kernel(");
        ASSERT_NE(entry, std::string::npos) << ir;
        const auto end = ir.find("\n}", entry);
        ASSERT_NE(end, std::string::npos);
        const auto body = ir.substr(entry, end - entry);
        EXPECT_FALSE(std::regex_search(body,
            std::regex(R"(call[^\n]*@[^\n(]*(dep|leaf))"))) << body;
    }
}

TEST(CachedCompile, GenericInlineAttributeReachesLlvmAndCachedCode) {
    TCacheDir cache;
    const char* source = R"((block
      (fun twice [T] ((var x T)) -> T (attrs inline)
        (block (return (+ x x))))
      (fun kernel ((var x i64)) -> i64
        (block (return (call twice x))))))";
    for (int optLevel : {0, 3}) {
        for (int run = 0; run < 2; ++run) {
            std::string err;
            testing::internal::CaptureStderr();
            auto linked = Compile(cache.Str(), source, "kernel", &err,
                optLevel, true);
            const auto ir = testing::internal::GetCapturedStderr();
            ASSERT_FALSE(linked.Entries.empty()) << err;
            EXPECT_EQ(reinterpret_cast<int64_t (*)(int64_t)>(
                linked.Entries.at("kernel"))(21), 42);
            if (optLevel == 0) {
                EXPECT_NE(ir.find("alwaysinline"), std::string::npos) << ir;
            }
            if (optLevel == 3) {
                const auto entry = ir.rfind("@kernel(");
                ASSERT_NE(entry, std::string::npos);
                const auto end = ir.find("\n}", entry);
                ASSERT_NE(end, std::string::npos);
                EXPECT_EQ(ir.substr(entry, end - entry).find("__generic_twice"),
                    std::string::npos) << ir;
            }
        }
    }
}

TEST(CachedCompile, LargeDependencyStaysOutOfLineAndImportsItsOwnLeaf) {
    TCacheDir cache;
    std::string source = R"((block
      (fun leaf ((var x i64)) -> i64 (attrs cacheable inline)
        (block (return (+ x 7))))
      (fun cold ((var x i64)) -> i64 (attrs cacheable)
        (block (var value = x))";
    for (int i = 0; i < 80; ++i) {
        source += "(= value (+ (^ value (>> value 5)) 17))";
    }
    source += R"((return (call leaf value))))
      (fun kernel ((var x i64)) -> i64
        (block (return (call cold x))))))";
    for (int run = 0; run < 2; ++run) {
        std::string error;
        testing::internal::CaptureStderr();
        auto linked = Compile(cache.Str(), source.c_str(), "kernel", &error, 3, true);
        const auto ir = testing::internal::GetCapturedStderr();
        ASSERT_FALSE(linked.Entries.empty()) << error;
        int64_t expected = 21;
        for (int i = 0; i < 80; ++i) {
            expected = (expected ^ (expected >> 5)) + 17;
        }
        EXPECT_EQ(reinterpret_cast<int64_t (*)(int64_t)>(linked.Entries.at("kernel"))(21), expected + 7);
        EXPECT_EQ(CountObjects(cache.Dir), 2);
        const auto begin = ir.rfind("@kernel(");
        ASSERT_NE(begin, std::string::npos) << ir;
        const auto end = ir.find("\n}", begin);
        ASSERT_NE(end, std::string::npos);
        EXPECT_TRUE(std::regex_search(ir.substr(begin, end - begin),
            std::regex(R"(call[^\n]*@[^\n(]*cold)"))) << ir;
        if (run == 0) {
            const std::regex definition(R"(define[^\n]*@[^\n(]*cold[^\n]*\{([\s\S]*?)\n\})");
            std::smatch match;
            ASSERT_TRUE(std::regex_search(ir, match, definition)) << ir;
            EXPECT_FALSE(std::regex_search(match[1].str(),
                std::regex(R"(call[^\n]*@[^\n(]*leaf)"))) << ir;
        }
    }
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
