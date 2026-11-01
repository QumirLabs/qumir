#include <qumir/ir/eval.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/passes/transforms/const_fold.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/semantics/transform/transform.h>

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

namespace {

struct TVMTest {
    TModule Module;
    TBuilder Builder{Module};
    TVMCompiler Compiler{Module};
    std::ostringstream Out;
    std::istringstream In;
    TInterpreter Interpreter{Module, Compiler, Out, In};

    TTmp Emit(TOp op, std::initializer_list<TOperand> operands, int typeId) {
        auto tmp = Builder.Emit1(op, operands);
        Builder.SetType(tmp, typeId);
        return tmp;
    }
};

TEST(VMBytecode, PackedTagsPreserveFullWidthValuesAndTheHeader) {
    TVMInstr instr{{TTmp{-1}, TLocal{17}, TImm{std::numeric_limits<int64_t>::min()}},
        EVMOp::VSelect, TVMInstr::MakeFormat(3, 2)};
    instr.Mask = {std::numeric_limits<int32_t>::max()};
    EXPECT_EQ(sizeof(instr), 32);
    EXPECT_EQ(instr.GetOperandType(0), TVMOperand::EType::Tmp);
    EXPECT_EQ(instr.GetOperandType(1), TVMOperand::EType::Local);
    EXPECT_EQ(instr.GetOperandType(2), TVMOperand::EType::Imm);
    EXPECT_EQ(instr.Operands[0].Tmp.Idx, -1);
    EXPECT_EQ(instr.Operands[1].Local.Idx, 17);
    EXPECT_EQ(instr.Operands[2].Imm.Value, std::numeric_limits<int64_t>::min());
    for (size_t i = 0; i < 3; ++i) {
        instr.SetOperand(i, TSlot{std::numeric_limits<int32_t>::max()});
        EXPECT_EQ(instr.GetOperandType(i), TVMOperand::EType::Slot);
        EXPECT_EQ(instr.Operands[i].Slot.Idx, std::numeric_limits<int32_t>::max());
        instr.SetOperand(i, TImm{std::numeric_limits<int64_t>::max()});
        EXPECT_EQ(instr.GetOperandType(i), TVMOperand::EType::Imm);
        EXPECT_EQ(instr.Operands[i].Imm.Value, std::numeric_limits<int64_t>::max());
        EXPECT_EQ(instr.Op, EVMOp::VSelect);
        EXPECT_EQ(instr.Format, TVMInstr::MakeFormat(3, 2));
        EXPECT_EQ(instr.Mask.Idx, std::numeric_limits<int32_t>::max());
    }
}

TEST(VMBytecode, ScalarImmediatesKeepAllSixtyFourBits) {
    for (const auto value : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), int64_t{-1}}) {
        TVMTest vm;
        const int type = vm.Module.Types.I(EKind::I64);
        vm.Builder.NewFunction("constant", {}, 0);
        const auto tmp = vm.Emit("mov"_op, {TImm{value, type}}, type);
        vm.Builder.Emit0("ret"_op, {tmp});
        vm.Builder.SetReturnType(type);
        EXPECT_EQ(vm.Interpreter.EvalRaw(vm.Module.Functions[0], {}, {}), value);
    }
}

std::expected<TFunction*, TError> LowerCore(TVMTest& vm, const std::string& source, bool optimize) {
    std::istringstream input(source);
    NAst::NCore::TTokenStream tokens(input);
    auto parsed = NAst::NCore::TParser{}.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    NSemantics::TNameResolver resolver;
    auto annotated = NTransform::Pipeline(*parsed, resolver);
    if (!annotated) {
        return std::unexpected(annotated.error());
    }
    TAstLowerer lowerer(vm.Module, vm.Builder, resolver, {.EmitDebugInfo = true});
    auto lowered = lowerer.LowerTop(*parsed);
    if (!lowered) {
        return std::unexpected(lowered.error());
    }
    if (optimize) {
        NPasses::Pipeline(vm.Module);
    }
    return vm.Module.GetFunctionByName("main");
}

template<typename T, size_t N>
void CheckCoreVector(const std::string& body, const std::array<T, N>& expected, int64_t result = 0) {
    for (bool optimize : {false, true}) {
        TVMTest vm;
        auto lowered = LowerCore(vm, "(block (fun main () -> i64 (block " + body + ")))", optimize);
        ASSERT_TRUE(lowered) << lowered.error().ToString();
        auto* function = *lowered;
        ASSERT_NE(function, nullptr);
        ASSERT_EQ(vm.Interpreter.EvalRaw(*function, {}, {}), result);
        int resultOffset = -1;
        for (const auto& instr : function->Exec->VMCode) {
            if (instr.LaneCount() == N && instr.GetOperandType(0) == TVMOperand::EType::Tmp
                && instr.Op != EVMOp::Store)
            {
                resultOffset = instr.Operands[0].Tmp.Idx;
            }
        }
        ASSERT_GE(resultOffset, 0);
        EXPECT_EQ(std::memcmp(vm.Interpreter.GetRuntime().Regs.Data() + resultOffset,
            expected.data(), sizeof(expected)), 0);
    }
}

template<typename T>
void CheckVectorArithmetic(EKind kind, int count = 4) {
    SCOPED_TRACE(static_cast<int>(kind));
    SCOPED_TRACE(count);
    TVMTest vm;
    const int elementType = vm.Module.Types.I(kind);
    const int vectorType = vm.Module.Types.Vec(elementType, count);
    const int i64 = vm.Module.Types.I(EKind::I64);
    vm.Builder.NewFunction("vector", {}, 0);
    for (int i = 0; i < 3; ++i) {
        vm.Builder.SetType(TLocal{i}, vectorType, {});
    }
    vm.Builder.SetType(TLocal{3}, i64, {});
    const auto left = vm.Emit("load"_op, {TLocal{0}}, vectorType);
    const auto right = vm.Emit("load"_op, {TLocal{1}}, vectorType);
    const auto sum = vm.Emit("+"_op, {left, right}, vectorType);
    const auto difference = vm.Emit("-"_op, {left, right}, vectorType);
    const auto product = vm.Emit("*"_op, {left, right}, vectorType);
    const int64_t five = std::is_floating_point_v<T>
        ? (sizeof(T) == 4 ? std::bit_cast<uint32_t>(5.0f) : std::bit_cast<int64_t>(5.0))
        : 5;
    const auto scalar = vm.Emit("mov"_op, {TImm{five, elementType}}, elementType);
    const auto broadcastLeft = vm.Emit("-"_op, {scalar, right}, vectorType);
    const auto broadcastRight = vm.Emit("+"_op, {left, TImm{five, elementType}}, vectorType);
    const auto copied = vm.Emit("mov"_op, {product}, vectorType);
    vm.Builder.Emit0("stre"_op, {TLocal{2}, copied});
    const auto reloaded = vm.Emit("load"_op, {TLocal{2}}, vectorType);
    const auto address = vm.Emit("lea"_op, {TLocal{2}}, vm.Module.Types.Ptr(vectorType));
    vm.Builder.Emit0("ste"_op, {address, copied});
    const auto indirect = vm.Emit("lde"_op, {address}, vectorType);
    const auto zero = vm.Emit("mov"_op, {TImm{0, vectorType}}, vectorType);
    const auto guard = vm.Emit("load"_op, {TLocal{3}}, i64);
    vm.Builder.Emit0("ret"_op, {guard});
    vm.Builder.SetReturnType(i64);
    auto& function = vm.Module.Functions[0];
    const auto& exec = vm.Compiler.Compile(function);
    std::vector<T> leftValues(count);
    std::vector<T> rightValues(count);
    for (int i = 0; i < count; ++i) {
        leftValues[i] = static_cast<T>(i * 7 + 1);
        rightValues[i] = static_cast<T>(i * 3 + 2);
    }
    if constexpr (!std::is_floating_point_v<T>) {
        leftValues[0] = static_cast<T>(-1);
        rightValues[0] = 1;
        leftValues[1] = static_cast<T>(-2);
        rightValues[1] = static_cast<T>(-3);
    }
    auto& runtime = vm.Interpreter.GetRuntime();
    runtime.Stack.resize(exec.NumLocals);
    const size_t size = count * sizeof(T);
    std::memcpy(runtime.Stack.data() + exec.LocalByteOffsets[0], leftValues.data(), size);
    std::memcpy(runtime.Stack.data() + exec.LocalByteOffsets[1], rightValues.data(), size);
    const int64_t marker = 73;
    std::memcpy(runtime.Stack.data() + exec.LocalByteOffsets[3], &marker, sizeof(marker));
    ASSERT_EQ(vm.Interpreter.EvalRaw(function, {}, {}), marker);
    using TCalc = std::conditional_t<std::is_floating_point_v<T>, T, uint64_t>;
    for (int i = 0; i < count; ++i) {
        auto lane = [&](TTmp tmp) {
            return runtime.Regs.Get<T>(exec.TmpByteOffsets.at(tmp.Idx) + i * sizeof(T));
        };
        const auto a = static_cast<TCalc>(leftValues[i]);
        const auto b = static_cast<TCalc>(rightValues[i]);
        EXPECT_EQ(lane(sum), static_cast<T>(a + b));
        EXPECT_EQ(lane(difference), static_cast<T>(a - b));
        EXPECT_EQ(lane(product), static_cast<T>(a * b));
        EXPECT_EQ(lane(copied), lane(product));
        EXPECT_EQ(lane(reloaded), lane(product));
        EXPECT_EQ(lane(indirect), lane(product));
        EXPECT_EQ(lane(broadcastLeft), static_cast<T>(static_cast<TCalc>(5) - b));
        EXPECT_EQ(lane(broadcastRight), static_cast<T>(a + 5));
        EXPECT_EQ(lane(zero), 0);
    }
    const auto& add = exec.VMCode[2];
    EXPECT_EQ(add.Op, std::is_floating_point_v<T> ? EVMOp::VFAdd : EVMOp::VIAdd);
    EXPECT_EQ(add.ElementSizeInBytes(), sizeof(T));
    EXPECT_EQ(add.LaneCount(), count);
    EXPECT_EQ(exec.TmpByteOffsets.at(sum.Idx) % alignof(T), 0);
}

TEST(VMVector, ArithmeticCopiesAndBroadcastsUseTheElementWidth) {
    CheckVectorArithmetic<uint8_t>(EKind::U8);
    CheckVectorArithmetic<uint16_t>(EKind::U16);
    CheckVectorArithmetic<uint32_t>(EKind::U32);
    CheckVectorArithmetic<uint64_t>(EKind::U64);
    CheckVectorArithmetic<uint64_t>(EKind::U64, 32);
    CheckVectorArithmetic<float>(EKind::F32);
    CheckVectorArithmetic<double>(EKind::F64);
}

TEST(VMVector, ConstructorExpressionsAndSSAExecuteThroughTheSemanticPipeline) {
    const std::string source =
        "(block (fun main () -> i64 (block"
        " (var a = 1)"
        " (var v = (vec a (+ a 2) (if #t 5 9) 7))"
        " (var copy = (if (< a 2) v (vec 99 99 99 99)))"
        " (var result = (* (- (+ copy 1) 2) (vec 2 3 4 5)))"
        " (return 0))))";
    for (bool optimize : {false, true}) {
        SCOPED_TRACE(optimize);
        TVMTest vm;
        auto lowered = LowerCore(vm, source, optimize);
        ASSERT_TRUE(lowered) << lowered.error().ToString();
        auto* function = *lowered;
        ASSERT_NE(function, nullptr);
        ASSERT_EQ(vm.Interpreter.EvalRaw(*function, {}, {}), 0);
        int productOffset = -1;
        for (const auto& instr : function->Exec->VMCode) {
            if (instr.Op == EVMOp::VIMul) {
                productOffset = instr.Operands[0].Tmp.Idx;
            }
        }
        ASSERT_GE(productOffset, 0);
        const std::array<int64_t, 4> expected = {0, 6, 16, 30};
        EXPECT_EQ(std::memcmp(vm.Interpreter.GetRuntime().Regs.Data() + productOffset,
            expected.data(), sizeof(expected)), 0);
        EXPECT_EQ(function->Exec->InstrDebugInfo.size(), function->Exec->VMCode.size());
    }
}

TEST(VMVector, ConstructorsPreservePackedIntegersBooleansAndFloats) {
    CheckCoreVector("(var v = (: (vec -1 127) <vec i8 2>)) (var copy = v) (return 0)",
        std::array<int8_t, 2>{-1, 127});
    CheckCoreVector("(var v = (vec #t #f)) (var copy = v) (return 0)",
        std::array<uint8_t, 2>{1, 0});
    CheckCoreVector("(var v = (vec -1.25 (+ 2.5 3.0))) (var r = (* 3.0 v)) (return 0)",
        std::array<double, 2>{-3.75, 16.5});
    CheckCoreVector(
        "(var a = 0)"
        " (var v = (vec (block (= a (+ a 1)) a) (block (= a (+ a 1)) a)"
        " (block (= a (+ a 1)) a) (block (= a (+ a 1)) a))) (return a)",
        std::array<int64_t, 4>{1, 2, 3, 4}, 4);
}

TEST(VMVector, ConstantFoldingPreservesVectorOperations) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    const int vectorType = vm.Module.Types.Vec(i64, 4);
    vm.Builder.NewFunction("fold", {}, 0);
    vm.Builder.SetType(TLocal{0}, vectorType, {});
    const auto value = vm.Emit("load"_op, {TLocal{0}}, vectorType);
    const auto product = vm.Emit("*"_op, {value, TImm{0, i64}}, vectorType);
    vm.Builder.Emit0("stre"_op, {TLocal{0}, product});
    auto& function = vm.Module.Functions[0];
    NPasses::ConstFold(function, vm.Module);
    EXPECT_EQ(function.Blocks[0].Instrs[1].Op, "*"_op);
    EXPECT_EQ(function.Blocks[0].Instrs[2].Operands[1], TOperand{product});
}

TEST(VMVector, IndexChecksBoundsBeforeReadingLanes) {
    for (bool optimize : {false, true}) {
        for (const std::string index : {"-1", "2", "9223372036854775807"}) {
            SCOPED_TRACE(index);
            TVMTest vm;
            auto lowered = LowerCore(vm,
                "(block (fun main () -> i64 (block (var i = " + index
                    + ") (return (index (vec 7 9) i)))))", optimize);
            ASSERT_TRUE(lowered) << lowered.error().ToString();
            EXPECT_THROW(vm.Interpreter.EvalRaw(**lowered, {}, {}), std::runtime_error);
        }
    }
}

TEST(VMVector, UnsupportedOperationsAndGlobalStorageAreRejected) {
    for (bool global : {false, true}) {
        TVMTest vm;
        const int i64 = vm.Module.Types.I(EKind::I64);
        const int vectorType = vm.Module.Types.Vec(i64, 4);
        vm.Module.GlobalTypes = {vectorType};
        vm.Builder.NewFunction("unsupported", {}, 0);
        vm.Builder.SetType(TLocal{0}, vectorType, {});
        const TOperand storage = global
            ? TOperand{TSlot{0}}
            : TOperand{TLocal{0}};
        const auto value = vm.Emit("load"_op, {storage}, vectorType);
        vm.Emit("/"_op, {value, value}, vectorType);
        EXPECT_THROW(vm.Compiler.Compile(vm.Module.Functions[0]), std::runtime_error);
    }
}

TEST(VMRegisterFile, AlignedTypedAccessAndBufferGrowth) {
    TRegisterFile registers;
    registers.Reset(48, 16);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(registers.Data()) % 16, 0);
    const __int128_t wide = static_cast<__int128_t>(1) << 100;
    registers.Get<int64_t>(0) = 17;
    registers.Get<__int128_t>(16) = wide;
    registers.Get<int64_t>(32) = std::bit_cast<int64_t>(3.25);
    const auto& values = registers;
    EXPECT_EQ(values.Get<int64_t>(0), 17);
    EXPECT_EQ(values.Get<__int128_t>(16), wide);
    EXPECT_EQ(std::bit_cast<double>(values.Get<int64_t>(32)), 3.25);

    struct alignas(64) TWideValue {
        std::array<uint64_t, 8> Lanes;
    };
    registers.Reset(128, alignof(TWideValue));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(registers.Data()) % 64, 0);
    EXPECT_EQ(registers.Size(), 128);
    registers.Get<TWideValue>(64) = {{1, 2, 3, 4, 5, 6, 7, 8}};
    EXPECT_EQ(values.Get<TWideValue>(64).Lanes.back(), 8);
    registers.Reset(8, 8);
    EXPECT_EQ(values.Get<int64_t>(0), 0);
}

TEST(VMRegisterFile, SparseIRIdsBecomeAlignedByteOffsets) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    const int i128 = vm.Module.Types.I(EKind::I128);
    const int f64 = vm.Module.Types.I(EKind::F64);
    vm.Builder.NewFunction("sparse", {}, 0);
    vm.Module.Functions[0].NextTmpIdx = 100;
    const auto narrow = vm.Emit("mov"_op, {TImm{7, i64}}, i64);
    const auto wide = vm.Emit("mov"_op, {TImm{13, i128}}, i128);
    const auto floating = vm.Emit("mov"_op, {TImm{std::bit_cast<int64_t>(2.5), f64}}, f64);
    const auto sum = vm.Emit('+'_op, {narrow, TImm{4, i64}}, i64);
    vm.Builder.Emit0("ret"_op, {sum});
    vm.Builder.SetReturnType(i64);
    vm.Builder.SetType(TTmp{10000}, i128);
    vm.Builder.SetType(TTmp{20000}, vm.Module.Types.Struct({i64, i64, i64}));

    auto& function = vm.Module.Functions[0];
    const auto originalTypes = function.TmpTypes;
    const auto originalNextTmp = function.NextTmpIdx;
    const auto& code = vm.Compiler.Compile(function);
    EXPECT_EQ(code.RegisterFileSize, 48);
    EXPECT_EQ(code.RegisterFileAlignment, 16);
    EXPECT_EQ(code.NumLocals, 0);
    EXPECT_EQ(code.TmpByteOffsets.size(), 4);
    EXPECT_EQ(code.TmpByteOffsets.at(narrow.Idx), 0);
    EXPECT_EQ(code.TmpByteOffsets.at(wide.Idx), 16);
    EXPECT_EQ(code.TmpByteOffsets.at(floating.Idx), 32);
    EXPECT_EQ(code.TmpByteOffsets.at(sum.Idx), 40);
    EXPECT_EQ(code.VMCode[3].Operands[1].Tmp.Idx, code.TmpByteOffsets.at(narrow.Idx));
    EXPECT_EQ(function.Blocks[0].Instrs[0].Dest.Idx, narrow.Idx);
    EXPECT_EQ(function.Blocks[0].Instrs[3].Operands[0].Tmp.Idx, narrow.Idx);
    EXPECT_EQ(function.TmpTypes, originalTypes);
    EXPECT_EQ(function.NextTmpIdx, originalNextTmp);
    EXPECT_EQ(vm.Interpreter.EvalRaw(function, {}, {}), 11);
}

TEST(VMRegisterFile, IRPipelinePreservesTemporaryIds) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    vm.Builder.NewFunction("sparse", {}, 0);
    vm.Module.Functions[0].NextTmpIdx = 100;
    const auto tmp = vm.Emit("call"_op, {TImm{123}}, i64);
    vm.Builder.Emit0("ret"_op, {tmp});
    vm.Builder.SetReturnType(i64);
    auto& function = vm.Module.Functions[0];
    NPasses::Pipeline(function, vm.Module);
    EXPECT_EQ(function.Blocks[0].Instrs[0].Dest.Idx, tmp.Idx);
    EXPECT_EQ(function.Blocks[0].Instrs[1].Operands[0].Tmp.Idx, tmp.Idx);
    EXPECT_EQ(function.GetTmpType(tmp.Idx), i64);
    EXPECT_EQ(function.NextTmpIdx, 101);
}

TEST(VMRegisterFile, MovCopiesTheComplete128BitPayload) {
    TVMTest vm;
    const int i128 = vm.Module.Types.I(EKind::I128);
    vm.Builder.NewFunction("wide_move", {}, 0);
    const auto one = vm.Emit("mov"_op, {TImm{1, i128}}, i128);
    const auto source = vm.Emit("<<"_op, {one, TImm{100, i128}}, i128);
    const auto copied = vm.Emit("mov"_op, {source}, i128);
    vm.Builder.Emit0("ret"_op, {copied});
    vm.Builder.SetReturnType(i128);
    auto& function = vm.Module.Functions[0];
    ASSERT_EQ(vm.Interpreter.EvalRaw(function, {}, {}), 0);
    const auto& exec = *function.Exec;
    EXPECT_EQ(exec.VMCode[2].Op, EVMOp::Mov);
    EXPECT_EQ(exec.VMCode[2].SizeInBytes(), 16);
    EXPECT_EQ(vm.Interpreter.GetRuntime().Regs.Get<__int128_t>(exec.TmpByteOffsets.at(copied.Idx)),
        static_cast<__int128_t>(1) << 100);
}

TEST(VMRegisterFile, CallsPreserveMixedRegistersAcrossBufferGrowth) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    const int i128 = vm.Module.Types.I(EKind::I128);
    const int f64 = vm.Module.Types.I(EKind::F64);
    const int callee = vm.Builder.NewFunction("callee", {TLocal{0}, TLocal{1}}, 1);
    vm.Builder.SetType(TLocal{0}, i128, {});
    vm.Builder.SetType(TLocal{1}, f64, {});
    const auto arg = vm.Emit("load"_op, {TLocal{0}}, i128);
    auto floatingArg = vm.Emit("load"_op, {TLocal{1}}, f64);
    for (int i = 0; i < 64; ++i) {
        floatingArg = vm.Emit('+'_op, {floatingArg, TImm{std::bit_cast<int64_t>(0.125), f64}}, f64);
    }
    const auto incremented = vm.Emit('+'_op, {arg, TImm{1, i128}}, i128);
    vm.Builder.Emit0("ret"_op, {incremented});
    vm.Builder.SetReturnType(i128);

    const int caller = vm.Builder.NewFunction("caller", {}, 2);
    const auto narrow = vm.Emit("mov"_op, {TImm{7, i64}}, i64);
    const auto one = vm.Emit("mov"_op, {TImm{1, i128}}, i128);
    const auto wide = vm.Emit("<<"_op, {one, TImm{80, i128}}, i128);
    const auto floating = vm.Emit("mov"_op, {TImm{std::bit_cast<int64_t>(3.25), f64}}, f64);
    vm.Builder.Emit0("arg"_op, {wide});
    vm.Builder.Emit0("arg"_op, {floating});
    const auto returned = vm.Emit("call"_op, {TImm{1}}, i128);
    const auto originalHigh = vm.Emit(">>"_op, {wide, TImm{64, i128}}, i128);
    const auto returnedHigh = vm.Emit(">>"_op, {returned, TImm{64, i128}}, i128);
    const auto low = vm.Emit("mov"_op, {returned}, i64);
    const auto high = vm.Emit("mov"_op, {returnedHigh}, i64);
    const auto original = vm.Emit("mov"_op, {originalHigh}, i64);
    const auto fraction = vm.Emit('+'_op, {floating, TImm{std::bit_cast<int64_t>(1.5), f64}}, f64);
    const auto integral = vm.Emit("f2i"_op, {fraction}, i64);
    auto sum = vm.Emit('+'_op, {high, low}, i64);
    sum = vm.Emit('+'_op, {sum, original}, i64);
    sum = vm.Emit('+'_op, {sum, integral}, i64);
    sum = vm.Emit('+'_op, {sum, narrow}, i64);
    vm.Builder.Emit0("ret"_op, {sum});
    vm.Builder.SetReturnType(i64);

    EXPECT_EQ(vm.Interpreter.EvalRaw(vm.Module.Functions[caller], {}, {}), 131084);
    EXPECT_GT(vm.Module.Functions[callee].Exec->RegisterFileSize, vm.Module.Functions[caller].Exec->RegisterFileSize);
    EXPECT_EQ(vm.Interpreter.GetRuntime().Regs.Size(), vm.Module.Functions[caller].Exec->RegisterFileSize);
}

TEST(VMRegisterFile, CallsWithoutRegistersCanDiscardAValue) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    vm.Builder.NewFunction("callee", {}, 1);
    vm.Builder.Emit0("ret"_op, {TImm{42, i64}});
    vm.Builder.SetReturnType(i64);
    const int caller = vm.Builder.NewFunction("caller", {}, 2);
    vm.Builder.Emit0("call"_op, {TImm{1}});
    vm.Builder.Emit0("ret"_op, {TImm{7, i64}});
    vm.Builder.SetReturnType(i64);
    EXPECT_EQ(vm.Interpreter.EvalRaw(vm.Module.Functions[caller], {}, {}), 7);
    EXPECT_EQ(vm.Interpreter.GetRuntime().Regs.Size(), 0);
}

TEST(VMRegisterFile, StructReturnUsesMetadataAtRegisterByteOffset) {
    TVMTest vm;
    const int i64 = vm.Module.Types.I(EKind::I64);
    const int i128 = vm.Module.Types.I(EKind::I128);
    const int structType = vm.Module.Types.Struct({i64, i64});
    const int pointer = vm.Module.Types.Ptr(i64);
    vm.Builder.NewFunction("callee", {}, 1);
    vm.Builder.SetType(TLocal{0}, structType, {});
    const auto address = vm.Emit("lea"_op, {TLocal{0}}, pointer);
    vm.Builder.Emit0("ste"_op, {address, TImm{19, i64}});
    const auto value = vm.Emit("load"_op, {TLocal{0}}, structType);
    vm.Builder.Emit0("ret"_op, {value});
    vm.Builder.SetReturnType(structType);

    const int caller = vm.Builder.NewFunction("caller", {}, 2);
    vm.Emit("mov"_op, {TImm{1, i128}}, i128);
    const auto returned = vm.Emit("call"_op, {TImm{1}}, structType);
    const auto firstField = vm.Emit("lde"_op, {returned}, i64);
    vm.Builder.Emit0("ret"_op, {firstField});
    vm.Builder.SetReturnType(i64);
    auto& function = vm.Module.Functions[caller];
    EXPECT_EQ(vm.Interpreter.EvalRaw(function, {}, {}), 19);
    const int byteOffset = function.Exec->TmpByteOffsets.at(returned.Idx);
    EXPECT_NE(byteOffset, returned.Idx);
    EXPECT_EQ(function.Exec->StructRegisters.at(byteOffset).Size, 16);
}

TEST(VMMemory, ScalarFormatsRoundTripLocalsAndGlobals) {
    for (EKind kind : {EKind::I64, EKind::U64, EKind::F64, EKind::I128, EKind::U128}) {
        for (bool useImmediate : {false, true}) {
            SCOPED_TRACE(static_cast<int>(kind));
            SCOPED_TRACE(useImmediate);
            TVMTest vm;
            const int type = vm.Module.Types.I(kind);
            const int i64 = vm.Module.Types.I(EKind::I64);
            const int boolean = vm.Module.Types.I(EKind::I1);
            const bool isWide = kind == EKind::I128 || kind == EKind::U128;
            const int64_t raw = kind == EKind::F64
                ? std::bit_cast<int64_t>(-3.25)
                : -17;
            __int128_t expected = raw;
            TOperand source = TImm{raw, type};
            vm.Module.GlobalTypes = {type, i64, i64};
            vm.Builder.NewFunction("memory", {}, 0);
            vm.Builder.SetType(TLocal{0}, i64, {});
            vm.Builder.SetType(TLocal{1}, type, {});
            vm.Builder.Emit0("stre"_op, {TLocal{0}, TImm{73, i64}});
            vm.Builder.Emit0("stre"_op, {TSlot{2}, TImm{73, i64}});
            if (!useImmediate) {
                if (isWide) {
                    const auto one = vm.Emit("mov"_op, {TImm{1, type}}, type);
                    const auto high = vm.Emit("<<"_op, {one, TImm{100, type}}, type);
                    source = vm.Emit('|'_op, {high, TImm{19, type}}, type);
                    expected = (static_cast<__int128_t>(1) << 100) | 19;
                } else {
                    source = vm.Emit("mov"_op, {source}, type);
                }
            }
            vm.Builder.Emit0("stre"_op, {TLocal{1}, source});
            const auto local = vm.Emit("load"_op, {TLocal{1}}, type);
            vm.Builder.Emit0("stre"_op, {TSlot{0}, local});
            const auto global = vm.Emit("load"_op, {TSlot{0}}, type);
            const auto matches = vm.Emit("=="_op, {global, source}, boolean);
            const auto neighbor = vm.Emit("load"_op, {TLocal{0}}, i64);
            const auto neighborMatches = vm.Emit("=="_op, {neighbor, TImm{73, i64}}, boolean);
            const auto result = vm.Emit('&'_op, {matches, neighborMatches}, boolean);
            vm.Builder.Emit0("ret"_op, {result});
            vm.Builder.SetReturnType(boolean);

            auto& function = vm.Module.Functions[0];
            ASSERT_EQ(vm.Interpreter.EvalRaw(function, {}, {}), 1);
            const auto& globals = vm.Interpreter.GetRuntime().Globals;
            ASSERT_EQ(globals.size(), 24);
            const size_t size = isWide
                ? 16
                : 8;
            const void* expectedData = isWide
                ? static_cast<const void*>(&expected)
                : static_cast<const void*>(&raw);
            EXPECT_EQ(std::memcmp(globals.data(), expectedData, size), 0);
            int64_t globalNeighbor = 0;
            std::memcpy(&globalNeighbor, globals.data() + 16, sizeof(globalNeighbor));
            EXPECT_EQ(globalNeighbor, 73);

            int wideOperations = 0;
            for (const auto& instr : function.Exec->VMCode) {
                if (instr.Op == EVMOp::Load || instr.Op == EVMOp::Store) {
                    EXPECT_EQ(instr.LaneCount(), 1);
                    if (instr.ElementSizeInBytes() == 16) {
                        ++wideOperations;
                    } else {
                        EXPECT_EQ(instr.ElementSizeInBytes(), 8);
                    }
                }
            }
            EXPECT_EQ(wideOperations, isWide ? 4 : 0);
        }
    }
}

TEST(VMMemory, ImmediateStoreOverwritesTheWholeDestination) {
    for (int64_t value : {0, 19, -17}) {
        SCOPED_TRACE(value);
        TVMTest vm;
        const int i64 = vm.Module.Types.I(EKind::I64);
        vm.Builder.NewFunction("immediate", {}, 0);
        vm.Builder.SetReturnType(i64);
        TExecFunc exec{
            .UniqueId = 0,
            .VMCode = {
                {{TLocal{8}, TImm{value}}, EVMOp::Store, TVMInstr::MakeFormat(4)},
                {{TSlot{1}, TImm{value}}, EVMOp::Store, TVMInstr::MakeFormat(4)},
                {{TImm{0}}, EVMOp::Ret},
            },
            .NumLocals = 32,
        };
        vm.Module.Functions[0].Exec = &exec;
        auto& runtime = vm.Interpreter.GetRuntime();
        runtime.Globals.assign(32, '\x5a');
        runtime.Stack.assign(32, '\x5a');
        ASSERT_EQ(vm.Interpreter.EvalRaw(vm.Module.Functions[0], {}, {}), 0);
        std::vector<char> expected(32, '\x5a');
        const __int128_t wide = value;
        std::memcpy(expected.data() + 8, &wide, sizeof(wide));
        EXPECT_EQ(runtime.Globals, expected);
        EXPECT_EQ(runtime.Stack, expected);
    }
}

TEST(VMMemory, PackedFormatsCopyTheCompletePayload) {
    struct TCase {
        uint8_t Format;
        int Size;
        const char* Spelling;
    };
    for (const auto& test : {
        TCase{TVMInstr::MakeFormat(2, 2), 16, "Load<4x4>"},
        TCase{TVMInstr::MakeFormat(0, 5), 32, "Load<1x32>"},
        TCase{TVMInstr::MakeFormat(4, 5), 512, "Load<16x32>"},
    }) {
        SCOPED_TRACE(test.Size);
        TVMTest vm;
        const int i64 = vm.Module.Types.I(EKind::I64);
        vm.Builder.NewFunction("packed", {}, 0);
        vm.Builder.SetReturnType(i64);
        const int destSlot = test.Size / 8 + 1;
        TExecFunc exec{
            .UniqueId = 0,
            .VMCode = {
                {{TTmp{test.Size}, TImm{73}}, EVMOp::Mov},
                {{TTmp{0}, TSlot{0}}, EVMOp::Load, test.Format},
                {{TSlot{destSlot}, TTmp{0}}, EVMOp::Store, test.Format},
                {{TTmp{test.Size}}, EVMOp::Ret},
            },
            .RegisterFileSize = test.Size + 8,
            .RegisterFileAlignment = 16,
        };
        vm.Module.Functions[0].Exec = &exec;
        std::vector<char> source(test.Size);
        for (int i = 0; i < test.Size; ++i) {
            source[i] = static_cast<char>(i * 37 + 11);
        }
        vm.Interpreter.GetRuntime().Globals = source;
        ASSERT_EQ(vm.Interpreter.EvalRaw(vm.Module.Functions[0], {}, {}), 73);
        const auto& globals = vm.Interpreter.GetRuntime().Globals;
        ASSERT_EQ(globals.size(), test.Size * 2 + 8);
        EXPECT_EQ(std::memcmp(globals.data(), source.data(), test.Size), 0);
        EXPECT_EQ(std::memcmp(globals.data() + destSlot * 8, source.data(), test.Size), 0);
        std::ostringstream printed;
        printed << exec.VMCode[1];
        EXPECT_TRUE(printed.str().starts_with(test.Spelling));
    }
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
