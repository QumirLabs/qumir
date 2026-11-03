#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/ir/eval.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/passes/transforms/inline_calls.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/semantics/transform/transform.h>

#include <gtest/gtest.h>

#include <set>
#include <sstream>
#include <string>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

namespace {

struct TProgram {
    TModule Module;
    TBuilder Builder{Module};
    TVMCompiler Compiler{Module};
    std::ostringstream Out;
    std::istringstream In;
    TInterpreter Interpreter{Module, Compiler, Out, In};
};

// Without definite assignment, as QumirDB lowers kernels: locals may be read before a store.
void Lower(TProgram& program, const std::string& source, bool optimize, TDebugOptions debug = {}) {
    std::istringstream input(source);
    NAst::NCore::TTokenStream tokens(input);
    auto parsed = NAst::NCore::TParser{}.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    NSemantics::TNameResolver resolver;
    auto annotated = NTransform::Pipeline(*parsed, resolver, {.RunDefiniteAssignment = false});
    ASSERT_TRUE(annotated) << annotated.error().ToString();
    TAstLowerer lowerer(program.Module, program.Builder, resolver, debug);
    auto lowered = lowerer.LowerTop(*parsed);
    ASSERT_TRUE(lowered) << lowered.error().ToString();
    if (optimize) {
        NPasses::Pipeline(program.Module);
    }
}

int CallsTo(TModule& module, const std::string& caller, const std::string& callee) {
    const int symId = module.GetFunctionByName(callee)->SymId;
    int calls = 0;
    for (const auto& block : module.GetFunctionByName(caller)->Blocks) {
        for (const auto& instr : block.Instrs) {
            calls += instr.Op == "call"_op && instr.Operands[0].Imm.Value == symId;
        }
    }
    return calls;
}

// PromoteLocalsToSSA must not leave a use of a phi it removed as trivial.
void ExpectDefinedTmps(const TFunction& function) {
    std::set<int> defined;
    for (const auto& block : function.Blocks) {
        for (const auto& phi : block.Phis) {
            defined.insert(phi.Dest.Idx);
        }
        for (const auto& instr : block.Instrs) {
            defined.insert(instr.Dest.Idx);
        }
    }
    auto check = [&](const TOperand& operand) {
        EXPECT_TRUE(operand.Type != TOperand::EType::Tmp || defined.contains(operand.Tmp.Idx))
            << function.Name << ": tmp " << operand.Tmp.Idx;
    };
    for (const auto& block : function.Blocks) {
        for (const auto& phi : block.Phis) {
            for (const auto& operand : phi.Operands) {
                check(operand);
            }
        }
        for (const auto& instr : block.Instrs) {
            for (int i = 0; i < instr.OperandCount; ++i) {
                check(instr.Operands[i]);
            }
        }
    }
}

int64_t RunVm(const std::string& source, bool optimize) {
    TProgram program;
    Lower(program, source, optimize);
    auto result = program.Interpreter.EvalRaw(*program.Module.GetFunctionByName("main"), {}, {});
    EXPECT_TRUE(result);
    return result.value_or(-1);
}

std::string RunLlvm(const std::string& source, int optLevel) {
    TLLVMRunner runner({
        .CoreInput = true,
        .ResolveCoreInput = true,
        .RunDefiniteAssignment = false,
        .OptLevel = optLevel,
    });
    std::istringstream input(source);
    auto result = runner.Run(input);
    if (!result) {
        return result.error().ToString();
    }
    return result->value_or("");
}

// The VM with and without the IR passes and LLVM at -O0 and -O3 agree on `expected`.
void CheckResult(const std::string& source, int64_t expected) {
    EXPECT_EQ(RunVm(source, false), expected);
    EXPECT_EQ(RunVm(source, true), expected);
    EXPECT_EQ(RunLlvm(source, 0), std::to_string(expected));
    EXPECT_EQ(RunLlvm(source, 3), std::to_string(expected));
}

const std::string Calls = R"((block
  (fun main () -> i64
    (block
      (var s i64)
      (= s 0)
      (var i i64)
      (= i 0)
      (while (< i 6)
        (block
          (if (&& (> i 0) (call odd i))
            (= s (+ s (call clamp i 2 4))))
          (= s (+ (* s 10) (call plain i)))
          (= i (+ i 1))))
      (return (+ s (call fact 5)))))
  (fun clamp ((var x i64) (var lo i64) (var hi i64)) -> i64 (attrs inline)
    (block
      (if (< x lo)
        (return lo))
      (if (> x hi)
        (return hi))
      (return x)))
  (fun odd ((var x i64)) -> bool (attrs inline)
    (block (return (== (& x 1) 1))))
  (fun plain ((var x i64)) -> i64
    (block (return (- x 1))))
  (fun fact ((var n i64)) -> i64 (attrs inline)
    (block
      (if (<= n 1)
        (return 1))
      (return (* n (call fact (- n 1))))))))";

} // namespace

TEST(InlineCalls, InlinesMarkedCallsOnly) {
    TProgram program;
    Lower(program, Calls, true);
    EXPECT_EQ(CallsTo(program.Module, "main", "clamp"), 0);
    EXPECT_EQ(CallsTo(program.Module, "main", "odd"), 0);
    EXPECT_EQ(CallsTo(program.Module, "main", "plain"), 1);
    EXPECT_EQ(CallsTo(program.Module, "main", "fact"), 1);
    EXPECT_EQ(CallsTo(program.Module, "fact", "fact"), 1);
}

TEST(InlineCalls, CycleKeepsCalls) {
    // `c` reaches the cycle through `b`, which the walk has already finished.
    const std::string source = R"((block
      (fun main () -> i64
        (block (return (call a 5))))
      (fun a ((var n i64)) -> i64 (attrs inline)
        (block
          (if (<= n 0)
            (return 0))
          (return (+ (call b (- n 1)) (call c n)))))
      (fun b ((var n i64)) -> i64 (attrs inline)
        (block (return (+ (call a n) 1))))
      (fun c ((var n i64)) -> i64 (attrs inline)
        (block
          (if (<= n 1)
            (return 1))
          (return (call b (- n 2)))))))";
    TProgram program;
    Lower(program, source, true);
    EXPECT_EQ(CallsTo(program.Module, "main", "a"), 1);
    EXPECT_EQ(CallsTo(program.Module, "a", "b"), 1);
    EXPECT_EQ(CallsTo(program.Module, "a", "c"), 1);
    EXPECT_EQ(CallsTo(program.Module, "c", "b"), 1);
    CheckResult(source, RunVm(source, false));
}

TEST(InlineCalls, MultipleReturnsAndPhis) {
    CheckResult(Calls, 104274 + 120);
}

TEST(InlineCalls, ReturnFromNestedLoops) {
    // The copied `probe` leaves both loops for the tail in the middle of the caller's loop.
    const std::string source = R"((block
      (fun main () -> i64
        (block
          (var total i64)
          (= total 0)
          (var row i64)
          (= row 0)
          (while (< row 6)
            (block
              (= total (+ (* total 10) (call lookup row)))
              (= row (+ row 1))))
          (return total)))
      (fun lookup ((var h i64)) -> i64 (attrs inline)
        (block
          (var slot = (call probe h))
          (if (>= slot 0)
            (return slot))
          (return 5)))
      (fun probe ((var h i64)) -> i64 (attrs inline)
        (block
          (var g = (& h 3))
          (var probes i64)
          (= probes 0)
          (while (< probes 4)
            (block
              (var m = (+ g 1))
              (while (!= m 0)
                (block
                  (if (== (+ g m) 3)
                    (return (+ g m)))
                  (= m (- m 1))))
              (= g (& (+ g 1) 3))
              (= probes (+ probes 1))))
          (return -1)))))";
    TProgram program;
    Lower(program, source, true);
    ExpectDefinedTmps(*program.Module.GetFunctionByName("main"));
    ASSERT_FALSE(testing::Test::HasFailure());
    CheckResult(source, 333333);
}

TEST(InlineCalls, CopiedBranchesInLoop) {
    // Removing the trivial phis of `group_shift` leads back to the phi being removed.
    const std::string source = R"((block
      (fun main () -> i64
        (block (return (call find 64 (: 300 u64)))))
      (fun backend () -> i64
        (block (return 0)))
      (fun shift () -> i64 (attrs inline)
        (block
          (if (== (call backend) 2)
            (return 4))
          (return 3)))
      (fun group ((var base i64) (var g i64)) -> i64 (attrs inline)
        (block (return (+ base (<< g (call shift))))))
      (fun find ((var capacity i64) (var hash u64)) -> i64
        (block
          (var group_shift = (call shift))
          (var num_groups = (>> capacity group_shift))
          (var g = (& (cast (>> hash (: 7 u64)) i64) (- num_groups 1)))
          (var probes i64)
          (= probes 0)
          (while (< probes num_groups)
            (block
              (var found = (& (call group 1000 g) 64))
              (if (!= found 0)
                (return (+ (<< g group_shift) found)))
              (= g (& (+ g 1) (- num_groups 1)))
              (= probes (+ probes 1))))
          (return -1)))))";
    TProgram program;
    Lower(program, source, true);
    ExpectDefinedTmps(*program.Module.GetFunctionByName("find"));
    ASSERT_FALSE(testing::Test::HasFailure());
    CheckResult(source, RunVm(source, false));
}

TEST(InlineCalls, ZeroesLocalsReadBeforeStore) {
    // A call frame starts zeroed; an inlined body in a loop must not see the previous iteration.
    const std::string source = R"((block
      (type Pair <struct (A i64) (B i64)>)
      (fun main () -> i64
        (block
          (var s i64)
          (= s 0)
          (var i i64)
          (= i 0)
          (while (< i 5)
            (block
              (= s (+ (* s 10) (call pick i)))
              (= i (+ i 1))))
          (return s)))
      (fun pick ((var x i64)) -> i64 (attrs inline)
        (block
          (var n i64)
          (var p <named Pair>)
          (var w i128)
          (if (== x 2)
            (block
              (= n 1)
              (= p (: (struct ((A 2) (B 0))) <named Pair>))
              (= w (: 3 i128))))
          (return (+ (+ n (field p A)) (cast w i64)))))))";
    CheckResult(source, 600);
}

TEST(InlineCalls, ZeroesPointerLocals) {
    // VM only: LLVM codegen rejects the typed undef that PromoteLocalsToSSA leaves in `pick` itself.
    const std::string source = R"((block
      (fun main () -> i64
        (block
          (var s i64)
          (= s 0)
          (var i i64)
          (= i 0)
          (while (< i 5)
            (block
              (= s (+ (* s 10) (call pick i)))
              (= i (+ i 1))))
          (return s)))
      (fun pick ((var x i64)) -> i64 (attrs inline)
        (block
          (var q <ptr i64>)
          (if (== x 2)
            (= q (cast 7 <ptr i64>)))
          (return (cast q i64))))))";
    EXPECT_EQ(RunVm(source, false), 700);
    EXPECT_EQ(RunVm(source, true), 700);
}

TEST(InlineCalls, LargeBodyStaysCall) {
    std::string body;
    for (int i = 0; i < 400; ++i) {
        body += "(= x (+ x " + std::to_string(i) + "))\n";
    }
    const std::string source = "(block (fun main () -> i64 (block (return (call big 1))))"
        "(fun big ((var x i64)) -> i64 (attrs inline) (block " + body + " (return x))))";
    TProgram program;
    Lower(program, source, true);
    EXPECT_EQ(CallsTo(program.Module, "main", "big"), 1);
    auto result = program.Interpreter.EvalRaw(*program.Module.GetFunctionByName("main"), {}, {});
    EXPECT_EQ(result.value_or(-1), 1 + 399 * 400 / 2);
}

TEST(InlineCalls, KeepsCalleeLocations) {
    TProgram program;
    Lower(program, Calls, false, {.EmitDebugInfo = true});
    std::set<int> clampLines;
    for (const auto& block : program.Module.GetFunctionByName("clamp")->Blocks) {
        for (const auto& info : block.InstrDebugInfo) {
            clampLines.insert(info.Location.Line);
        }
    }
    NPasses::InlineCalls(program.Module);
    ASSERT_EQ(CallsTo(program.Module, "main", "clamp"), 0);
    int inlined = 0;
    for (const auto& block : program.Module.GetFunctionByName("main")->Blocks) {
        ASSERT_LE(block.InstrDebugInfo.size(), block.Instrs.size());
        for (const auto& info : block.InstrDebugInfo) {
            inlined += clampLines.contains(info.Location.Line);
        }
    }
    EXPECT_GT(inlined, 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
