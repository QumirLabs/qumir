#include <qumir/ir/eval.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/semantics/transform/transform.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

using namespace NQumir;
using namespace NQumir::NIR;

// Resolved by the VM through dlsym (ENABLE_EXPORTS).
extern "C" int64_t qumir_prepared_twice(int64_t x) {
    return 2 * x;
}

namespace {

struct TPair {
    int64_t A;
    int64_t B;
};

struct TProgram {
    TModule Module;
    TBuilder Builder{Module};
    TVMCompiler Compiler{Module};

    explicit TProgram(const std::string& source) {
        std::istringstream input(source);
        NAst::NCore::TTokenStream tokens(input);
        auto parsed = NAst::NCore::TParser{}.Parse(tokens);
        EXPECT_TRUE(parsed) << parsed.error().ToString();
        NSemantics::TNameResolver resolver;
        auto annotated = NTransform::Pipeline(*parsed, resolver);
        EXPECT_TRUE(annotated) << annotated.error().ToString();
        TAstLowerer lowerer(Module, Builder, resolver, {});
        auto lowered = lowerer.LowerTop(*parsed);
        EXPECT_TRUE(lowered) << lowered.error().ToString();
    }

    TFunction& Get(const std::string& name) {
        auto* function = Module.GetFunctionByName(name);
        EXPECT_NE(function, nullptr);
        return *function;
    }
};

const char* const KernelSource = R"((block
  (type pair <struct (a i64) (b i64)>)
  (fun qumir_prepared_twice ((var x i64)) -> i64 (attrs extern) (block))
  (fun helper ((var x i64)) -> i64 (block (return (+ x 1))))
  (fun entry ((var p pair) (var n i64)) -> i64
    (block
      (var wide i128)
      (= wide (* (cast n i128) 3))
      (return (call qumir_prepared_twice
        (+ (call helper (+ (field p a) (field p b))) (cast wide i64)))))))
)";

int64_t Expected(const TPair& pair, int64_t n) {
    return 2 * (pair.A + pair.B + 1 + 3 * n);
}

struct TInstance {
    std::ostringstream Out;
    std::istringstream In;
    TInterpreter Interpreter;

    explicit TInstance(TProgram& program)
        : Interpreter(program.Module, program.Compiler, Out, In)
    {}
};

std::vector<const TExecFunc*> ExecPointers(const TModule& module) {
    std::vector<const TExecFunc*> res;
    for (const auto& function : module.Functions) {
        res.push_back(function.Exec);
    }
    return res;
}

} // namespace

TEST(VMPrepared, PrepareAndInvokeManyTimes) {
    TProgram program(KernelSource);
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry})) << "prepare failed";
    const auto before = ExecPointers(program.Module);
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    for (int64_t i = 0; i < 1000; ++i) {
        TPair pair{i, 2 * i};
        auto res = instance.Interpreter.Invoke(entry, {reinterpret_cast<int64_t>(&pair), i});
        ASSERT_TRUE(res) << res.error().ToString();
        ASSERT_EQ(*res, Expected(pair, i));
    }
    EXPECT_EQ(ExecPointers(program.Module), before);
}

TEST(VMPrepared, OneReusedInterpreterPerThread) {
    TProgram program(KernelSource);
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry}));
    const auto before = ExecPointers(program.Module);
    std::atomic<int> failures{0};
    // Each worker creates its interpreter once and reuses it for all calls.
    auto work = [&](int64_t seed) {
        TInstance instance(program);
        if (!instance.Interpreter.Initialize()) {
            ++failures;
        }
        for (int64_t i = 0; i < 2000; ++i) {
            TPair pair{seed + i, seed - i};
            auto res = instance.Interpreter.Invoke(entry, {reinterpret_cast<int64_t>(&pair), i});
            if (!res || *res != Expected(pair, i)) {
                ++failures;
            }
        }
        if (!instance.Interpreter.Finalize()) {
            ++failures;
        }
    };
    std::vector<std::thread> threads;
    for (int64_t seed = 0; seed < 4; ++seed) {
        threads.emplace_back(work, seed * 1000);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(ExecPointers(program.Module), before);
}

TEST(VMPrepared, EvalRawCompilesLazilyAndInvokeDoesNot) {
    TProgram program(KernelSource);
    auto& entry = program.Get("entry");
    auto& helper = program.Get("helper");
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    TPair pair{1, 2};
    const std::vector<int64_t> args{reinterpret_cast<int64_t>(&pair), 3};
    EXPECT_EQ(helper.Exec, nullptr);
    EXPECT_FALSE(instance.Interpreter.Invoke(entry, args));
    EXPECT_EQ(helper.Exec, nullptr);
    // The shared write that makes concurrent EvalRaw unsafe.
    EXPECT_EQ(instance.Interpreter.EvalRaw(entry, args, {}), Expected(pair, 3));
    EXPECT_NE(helper.Exec, nullptr);
}

TEST(VMPrepared, PrepareReportsUnresolvedExtern) {
    TProgram program(R"((block
      (fun qumir_prepared_no_such_symbol ((var x i64)) -> i64 (attrs extern) (block))
      (fun entry ((var x i64)) -> i64 (block (return (call qumir_prepared_no_such_symbol x))))))");
    auto& entry = program.Get("entry");
    auto res = program.Compiler.Prepare({&entry});
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("qumir_prepared_no_such_symbol"), std::string::npos)
        << res.error().ToString();
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    EXPECT_FALSE(instance.Interpreter.Invoke(entry, {1}));
}

TEST(VMPrepared, InvokeRejectsWrongArgumentCount) {
    TProgram program(KernelSource);
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry}));
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    EXPECT_FALSE(instance.Interpreter.Invoke(entry, {1}));
}

TEST(VMPrepared, InitializeOnceAndFinalizeIsIdempotent) {
    TProgram program(R"((block
      (var counter = (: 5 i64))
      (fun bump () -> i64 (block (= counter (+ counter 1)) (return counter)))))");
    auto& bump = program.Get("bump");
    ASSERT_NE(program.Module.ModuleConstructorFunctionId, -1);
    ASSERT_TRUE(program.Compiler.Prepare({&bump}));
    TInstance instance(program);
    EXPECT_FALSE(instance.Interpreter.Invoke(bump, {}));
    ASSERT_TRUE(instance.Interpreter.Initialize());
    ASSERT_TRUE(instance.Interpreter.Initialize());
    EXPECT_EQ(instance.Interpreter.Invoke(bump, {}), 6);
    EXPECT_EQ(instance.Interpreter.Invoke(bump, {}), 7);
    EXPECT_TRUE(instance.Interpreter.Finalize());
    EXPECT_TRUE(instance.Interpreter.Finalize());
}

TEST(VMPrepared, FailedPrepareLeavesNothingPrepared) {
    TProgram program(R"((block
      (fun qumir_prepared_no_such_symbol ((var x i64)) -> i64 (attrs extern) (block))
      (fun bad ((var x i64)) -> i64 (block (return (call qumir_prepared_no_such_symbol x))))
      (fun entry ((var x i64)) -> i64 (block (return (call bad x))))))");
    auto& entry = program.Get("entry");
    ASSERT_FALSE(program.Compiler.Prepare({&entry}));
    EXPECT_EQ(entry.Exec, nullptr);
    EXPECT_EQ(program.Get("bad").Exec, nullptr);
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    for (int i = 0; i < 2; ++i) {
        EXPECT_FALSE(instance.Interpreter.Invoke(entry, {1}));
    }
    EXPECT_EQ(entry.Exec, nullptr);
}

TEST(VMPrepared, InvokeRejectsWideArgument) {
    TProgram program(R"((block
      (fun entry ((var x i128)) -> i64 (block (return (cast (>> x 64) i64))))))");
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry}));
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    EXPECT_FALSE(instance.Interpreter.Invoke(entry, {-1}));
}

TEST(VMPrepared, InvokeRejectsVectorArgument) {
    TProgram program(R"((block
      (fun entry ((var x <vec u64 2>)) -> u64 (block (return (index x 0))))))");
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry}));
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    auto res = instance.Interpreter.Invoke(entry, {1});
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("vector argument"), std::string::npos) << res.error().ToString();
}

TEST(VMPrepared, InvokeRejectsVectorResult) {
    TProgram program(R"((block
      (fun entry () -> <vec u64 2> (block (return (: (vec 1 2) <vec u64 2>))))))");
    auto& entry = program.Get("entry");
    ASSERT_TRUE(program.Compiler.Prepare({&entry}));
    TInstance instance(program);
    ASSERT_TRUE(instance.Interpreter.Initialize());
    auto res = instance.Interpreter.Invoke(entry, {});
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("returns a vector"), std::string::npos) << res.error().ToString();
}

TEST(VMPrepared, FailedConstructorIsNotFinalized) {
    TProgram program(R"((block
      (var counter = (: 5 i64))
      (fun bump () -> i64 (block (= counter (+ counter 1)) (return counter)))))");
    TInstance instance(program);
    // Not prepared: the constructor cannot run.
    EXPECT_FALSE(instance.Interpreter.Initialize());
    EXPECT_FALSE(instance.Interpreter.Invoke(program.Get("bump"), {}));
    EXPECT_TRUE(instance.Interpreter.Finalize());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
