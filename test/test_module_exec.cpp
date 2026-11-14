#include <gtest/gtest.h>

#include <qumir/runner/runner_ir.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/runtime/io.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace NQumir;
namespace fs = std::filesystem;

namespace {

enum class EBackend { IR, LLVM };

struct TBackendCase {
    EBackend Backend;
    int OptLevel;
};

class ModuleExecTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        Dir = fs::temp_directory_path() / ("qumir_modexec_" + std::to_string(++counter));
        fs::remove_all(Dir);
        fs::create_directories(Dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(Dir, ec);
    }
    void WriteModule(const std::string& name, const std::string& content) {
        std::ofstream out(Dir / (name + ".oz"));
        out << content;
    }

    std::string Run(const std::string& main, TBackendCase bc) {
        std::ostringstream out;
        NRuntime::SetOutputStream(&out);
        NRuntime::SetInputStream(nullptr);
        std::istringstream src(main);
        std::expected<std::optional<std::string>, TError> res;
        if (bc.Backend == EBackend::IR) {
            std::istringstream in;
            TIRRunner runner(out, in, TIRRunnerOptions{
                .CoreInput = true,
                .ResolveCoreInput = true,
                .OptLevel = bc.OptLevel,
                .Prelude = {"System"},
                .ModuleSearchPaths = {Dir.string()},
            });
            res = runner.Run(src);
        } else {
            TLLVMRunner runner(TLLVMRunnerOptions{
                .CoreInput = true,
                .ResolveCoreInput = true,
                .OptLevel = bc.OptLevel,
                .Prelude = {"System"},
                .ModuleSearchPaths = {Dir.string()},
            });
            res = runner.Run(src);
        }
        EXPECT_TRUE(res) << (res ? "" : res.error().ToString());
        return out.str();
    }

    std::string RunKumir(const std::string& main, EBackend backend) {
        std::ostringstream out;
        NRuntime::SetOutputStream(&out);
        NRuntime::SetInputStream(nullptr);
        std::istringstream src(main);
        std::expected<std::optional<std::string>, TError> res;
        if (backend == EBackend::IR) {
            std::istringstream in;
            TIRRunner runner(out, in, TIRRunnerOptions{
                .ModuleSearchPaths = {Dir.string()},
            });
            res = runner.Run(src);
        } else {
            TLLVMRunner runner(TLLVMRunnerOptions{
                .ModuleSearchPaths = {Dir.string()},
            });
            res = runner.Run(src);
        }
        EXPECT_TRUE(res) << (res ? "" : res.error().ToString());
        return out.str();
    }

    // Runs across VM, LLVM JIT and optimization levels; all must agree.
    void ExpectAll(const std::string& main, const std::string& expected) {
        static const std::vector<TBackendCase> cases = {
            {EBackend::IR, 0}, {EBackend::IR, 1},
            {EBackend::LLVM, 0}, {EBackend::LLVM, 1}, {EBackend::LLVM, 2}, {EBackend::LLVM, 3},
        };
        for (const auto& bc : cases) {
            EXPECT_EQ(Run(main, bc), expected)
                << "backend=" << (bc.Backend == EBackend::IR ? "IR" : "LLVM")
                << " opt=" << bc.OptLevel;
        }
    }

    fs::path Dir;
};

TEST_F(ModuleExecTest, MainCallsImportedFunction) {
    WriteModule("mod",
        "(block (fun add ((var a i64) (var b i64)) -> i64 (block (return (+ a b)))))");

    ExpectAll(
        "(block (use mod) (fun <main> () (block (output (call add (: 2 i64) (: 3 i64)) \"\\n\"))))",
        "5\n");
}

TEST_F(ModuleExecTest, GenericModuleFunctionIndexesArrayParameterWithoutBounds) {
    WriteModule("arrays",
        "(block"
        " (fun bump_first [Value] ((var values <array Value 1>) (var delta i64)) -> i64"
        "   (block"
        "     (var item = (index values (: 0 i64)))"
        "     (= values [(: 0 i64)] (+ item delta))"
        "     (return (index values (: 0 i64))))))");

    ExpectAll(
        "(block (use arrays) (fun <main> () (block"
        "   (var a <array i64 1> [0 1])"
        "   (= a [0] (: 5 i64))"
        "   (output (call bump_first a (: 7 i64)) \" \" (index a (: 0 i64)) \"\\n\"))))",
        "12 12\n");
}

TEST_F(ModuleExecTest, TransitiveImport) {
    WriteModule("low", "(block (fun base () -> i64 (block (return (: 40 i64)))))");
    WriteModule("mid",
        "(block (use low) (fun bump () -> i64 (block (return (+ (call base) (: 2 i64))))))");

    ExpectAll(
        "(block (use mid) (fun <main> () (block (output (call bump) \"\\n\"))))",
        "42\n");
}

TEST_F(ModuleExecTest, ModuleGlobalReadByMain) {
    WriteModule("g", "(block (var base = (: 100 i64)))");

    ExpectAll(
        "(block (use g) (fun <main> () (block (output base \"\\n\"))))",
        "100\n");
}

TEST_F(ModuleExecTest, ModuleGlobalInitializationOrder) {
    WriteModule("low", "(block (var lowval = (: 40 i64)))");
    // highval's initializer reads low's global; low must initialize first.
    WriteModule("high", "(block (use low) (var highval = (+ lowval (: 2 i64))))");

    ExpectAll(
        "(block (use high) (fun <main> () (block (output highval \"\\n\"))))",
        "42\n");
}

TEST_F(ModuleExecTest, ModuleStringGlobal) {
    WriteModule("s", "(block (var greeting = \"hi\"))");

    ExpectAll(
        "(block (use s) (fun <main> () (block (output greeting \"\\n\"))))",
        "hi\n");
}

TEST_F(ModuleExecTest, ModuleOperatorAndCast) {
    // A source module exports a struct type plus a binary operator and a cast,
    // marked with (operator "...") attributes; main uses operator/coercion syntax.
    WriteModule("vec",
        "(block"
        " (type точка <struct (x i64) (y i64)>)"
        " (fun слож ((var a точка) (var b точка)) -> точка (attrs (operator \"+\"))"
        "   (block (return (: (struct ((x (+ (field a x) (field b x)))"
        "                              (y (+ (field a y) (field b y))))) точка))))"
        " (fun изцел ((var n i64)) -> точка (attrs (operator \"cast\"))"
        "   (block (return (: (struct ((x n) (y n))) точка)))))");

    ExpectAll(
        "(block (use vec) (fun <main> () (block"
        "   (var p точка) (= p (: 3 i64))"          // implicit cast i64 -> точка via изцел
        "   (var q точка) (= q (+ p p))"            // operator + via слож
        "   (output (field q x) \" \" (field q y) \"\\n\"))))",
        "6 6\n");
}

TEST_F(ModuleExecTest, ModulePrintOperator) {
    // A source module supplies a custom printer via the simple `print` attribute;
    // outputting a value of the module's type dispatches to it.
    WriteModule("pt",
        "(block"
        " (type точка <struct (x i64) (y i64)>)"
        " (fun печать ((var p точка)) (attrs print)"
        "   (block (output \"(\" (field p x) \";\" (field p y) \")\"))))");

    ExpectAll(
        "(block (use pt) (fun <main> () (block"
        "   (var p точка) (= p (: (struct ((x (: 1 i64)) (y (: 2 i64)))) точка))"
        "   (output p \"\\n\"))))",
        "(1;2)\n");
}

TEST_F(ModuleExecTest, GenericFunctionInModule) {
    WriteModule("gen",
        "(block (pragma language overloads)"
        " (fun id [T] ((var x T)) -> T (block (return x))))");

    ExpectAll(
        "(block (pragma language overloads) (use gen)"
        " (fun <main> () (block (output (call id (: 7 i64)) \"\\n\"))))",
        "7\n");
}

TEST_F(ModuleExecTest, KumirCallsImportedOzFunction) {
    WriteModule("dbl",
        "(block (fun udvoit ((var x i64)) -> i64 (block (return (+ x x)))))");

    const std::string main =
        "использовать dbl\n"
        "алг\n"
        "нач\n"
        "    цел x\n"
        "    x := udvoit(21)\n"
        "    вывод x\n"
        "кон\n";

    EXPECT_EQ(RunKumir(main, EBackend::IR), "42");
    EXPECT_EQ(RunKumir(main, EBackend::LLVM), "42");
}

// Kumir's entry point is the first no-arg function. A module exporting a
// no-arg function must not be picked over the main program's algorithm.
TEST_F(ModuleExecTest, KumirEntryNotShadowedByModuleNoArgFunction) {
    WriteModule("ent",
        "(block (fun base () -> i64 (block (return (: 7 i64)))))");

    const std::string main =
        "использовать ent\n"
        "алг\n"
        "нач\n"
        "    цел x\n"
        "    x := base()\n"
        "    вывод x * 10\n"
        "кон\n";

    EXPECT_EQ(RunKumir(main, EBackend::IR), "70");
    EXPECT_EQ(RunKumir(main, EBackend::LLVM), "70");
}

TEST_F(ModuleExecTest, KumirUsesImportedOzType) {
    WriteModule("pt",
        "(block"
        " (type точка <struct (x i64) (y i64)>)"
        " (fun создать ((var a i64) (var b i64)) -> точка"
        "   (block (return (: (struct ((x a) (y b))) точка))))"
        " (fun сумма ((var p точка)) -> i64"
        "   (block (return (+ (field p x) (field p y))))))");

    const std::string main =
        "использовать pt\n"
        "алг\n"
        "нач\n"
        "    точка p\n"
        "    p := создать(20, 22)\n"
        "    вывод сумма(p)\n"
        "кон\n";

    EXPECT_EQ(RunKumir(main, EBackend::IR), "42");
    EXPECT_EQ(RunKumir(main, EBackend::LLVM), "42");
}

// `outer` references a type defined in its dependency `inner`; the transitive
// type must resolve in the composed unit.
TEST_F(ModuleExecTest, KumirUsesTransitiveOzType) {
    WriteModule("inner",
        "(block"
        " (type ед <struct (v i64)>)"
        " (fun обернуть ((var n i64)) -> ед"
        "   (block (return (: (struct ((v n))) ед)))))");
    WriteModule("outer",
        "(block"
        " (use inner)"
        " (fun развернуть ((var u ед)) -> i64 (block (return (field u v)))))");

    const std::string main =
        "использовать outer\n"
        "использовать inner\n"
        "алг\n"
        "нач\n"
        "    ед u\n"
        "    u := обернуть(42)\n"
        "    вывод развернуть(u)\n"
        "кон\n";

    EXPECT_EQ(RunKumir(main, EBackend::IR), "42");
    EXPECT_EQ(RunKumir(main, EBackend::LLVM), "42");
}

// A module supplies a custom print operator for its type; the Kumir program
// also declares its own ordinary function named `print`. Both must coexist:
// outputting the type dispatches to the operator, while `print(...)` calls the
// user function (the "print" operator key lives in a separate table).
TEST_F(ModuleExecTest, CustomPrintOperatorAndUserPrintFunction) {
    WriteModule("pt",
        "(block"
        " (type точка <struct (x i64) (y i64)>)"
        " (fun создать ((var a i64) (var b i64)) -> точка"
        "   (block (return (: (struct ((x a) (y b))) точка))))"
        " (fun печать ((var p точка)) (attrs print)"
        "   (block (output \"(\" (field p x) \";\" (field p y) \")\"))))");

    const std::string main =
        "использовать pt\n"
        "алг\n"
        "нач\n"
        "    точка p\n"
        "    p := создать(1, 2)\n"
        "    вывод p, \" \", print(21), нс\n"
        "кон\n"
        "алг цел print(цел n)\n"
        "нач\n"
        "    знач := n + n\n"
        "кон\n";

    EXPECT_EQ(RunKumir(main, EBackend::IR), "(1;2) 42\n");
    EXPECT_EQ(RunKumir(main, EBackend::LLVM), "(1;2) 42\n");
}

// Out-of-range float to integer conversion is platform-defined: x86-64 cvttsd2si gives
// INT64_MIN for NaN and out-of-range values, AArch64 fcvtzs saturates and maps NaN to 0.
// The VM and the JIT must agree on it.
TEST_F(ModuleExecTest, FloatToIntOutOfRangeIsPlatformDefined) {
#if defined(__x86_64__) || defined(_M_X64)
    const std::string big = "-9223372036854775808", neg = "-9223372036854775808", nan = "-9223372036854775808";
#elif defined(__aarch64__)
    const std::string big = "9223372036854775807", neg = "-9223372036854775808", nan = "0";
#else
    GTEST_SKIP() << "no native float to integer conversion on this platform";
#endif
    const std::string main =
        "(block"
        " (fun d64 ((var x f64)) -> i64 (block (return (cast x i64))))"
        " (fun d32 ((var x f32)) -> i64 (block (return (cast x i64))))"
        " (fun <main> () (block"
        "   (var z f64) (= z (: 0.0 f64))"
        "   (var h f64) (= h (: 1e30 f64))"
        "   (var nz f32) (= nz (: 0.0 f32))"
        "   (var h32 f32) (= h32 (: 1e30 f32))"
        "   (output (call d64 (/ z z)) \" \" (call d64 h) \" \" (call d64 (- h)) \" \" (call d64 (: 2.9 f64)) \" \" (call d64 (: -2.9 f64)) \"\\n\")"
        "   (output (call d32 (/ nz nz)) \" \" (call d32 h32) \" \" (call d32 (- h32)) \" \" (call d32 (: 2.9 f32)) \"\\n\"))))";
    ExpectAll(main,
        nan + " " + big + " " + neg + " 2 -2\n" +
        nan + " " + big + " " + neg + " 2\n");
}

// 128-bit results have no native conversion: they saturate and map NaN to 0 everywhere.
TEST_F(ModuleExecTest, FloatToInt128Saturates) {
    ExpectAll(
        "(block"
        " (fun top ((var x f64)) -> i64 (block (return (cast (>> (cast x i128) (: 64 i128)) i64))))"
        " (fun <main> () (block"
        "   (var z f64) (= z (: 0.0 f64))"
        "   (var h f64) (= h (: 1e40 f64))"
        "   (output (call top (/ z z)) \" \" (call top h) \" \" (call top (- h)) \" \" (call top (: 3.0 f64)) \"\\n\"))))",
        "0 9223372036854775807 -9223372036854775808 0\n");
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
