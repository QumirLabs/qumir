#include <qumir/ir/eval.h>
#include <qumir/ir/passes/analysis/cfg.h>
#include <qumir/ir/passes/transforms/lower_allocated_phis.h>

#include <gtest/gtest.h>

#include <cstring>
#include <sstream>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;
using namespace NQumir::NIR::NPasses;

TEST(VMAllocation, PhiInputsAreLiveOnlyOnTheirOwnEdges) {
    TFunction function{.Blocks = {
        {.Label = {0}, .Instrs = {{.Op = "cmp"_op, .Operands = {TImm{1}, TLabel{1}, TLabel{2}}, .OperandCount = 3}}},
        {.Label = {1}, .Instrs = {
            {.Op = "mov"_op, .Dest = {0}, .Operands = {TImm{7}}, .OperandCount = 1},
            {.Op = "jmp"_op, .Operands = {TLabel{3}}, .OperandCount = 1}}},
        {.Label = {2}, .Instrs = {
            {.Op = "mov"_op, .Dest = {1}, .Operands = {TImm{9}}, .OperandCount = 1},
            {.Op = "jmp"_op, .Operands = {TLabel{3}}, .OperandCount = 1}}},
        {.Label = {3}, .Phis = {{.Op = "phi"_op, .Dest = {2},
            .Operands = {TTmp{0}, TLabel{1}, TTmp{1}, TLabel{2}}}},
            .Instrs = {{.Op = "ret"_op, .Operands = {TTmp{2}}, .OperandCount = 1}}},
    }};
    BuildCfg(function);
    const auto live = ComputeLiveOut(function);
    EXPECT_TRUE(live[0].empty());
    EXPECT_EQ(live[1], TLiveSet{0});
    EXPECT_EQ(live[2], TLiveSet{1});
    EXPECT_TRUE(live[3].empty());
}

TEST(VMAllocation, BackEdgesPreserveLoopCarriedValues) {
    TModule module;
    const int type = module.Types.I(EKind::I64);
    module.Functions.push_back({.Name = "loop", .Blocks = {
        {.Label = {0}, .Instrs = {
            {.Op = "mov"_op, .Dest = {0}, .Operands = {TImm{0, type}}, .OperandCount = 1},
            {.Op = "jmp"_op, .Operands = {TLabel{1}}, .OperandCount = 1}}},
        {.Label = {1}, .Phis = {{.Op = "phi"_op, .Dest = {1},
            .Operands = {TTmp{0}, TLabel{0}, TTmp{2}, TLabel{2}}}}, .Instrs = {
            {.Op = "<"_op, .Dest = {3}, .Operands = {TTmp{1}, TImm{17, type}}, .OperandCount = 2},
            {.Op = "cmp"_op, .Operands = {TTmp{3}, TLabel{2}, TLabel{3}}, .OperandCount = 3}}},
        {.Label = {2}, .Instrs = {
            {.Op = "+"_op, .Dest = {2}, .Operands = {TTmp{1}, TImm{1, type}}, .OperandCount = 2},
            {.Op = "jmp"_op, .Operands = {TLabel{1}}, .OperandCount = 1}}},
        {.Label = {3}, .Instrs = {{.Op = "ret"_op, .Operands = {TTmp{1}}, .OperandCount = 1}}},
    }, .TmpTypes = {type, type, type, module.Types.I(EKind::I1)},
       .ReturnTypeId = type, .SymId = 0, .UniqueId = 0, .NextTmpIdx = 4, .NextLabelIdx = 4});
    auto& function = module.Functions[0];
    BuildCfg(function);
    const auto live = ComputeLiveOut(function);
    EXPECT_EQ(live[0], TLiveSet{0});
    EXPECT_EQ(live[1], TLiveSet{1});
    EXPECT_EQ(live[2], TLiveSet{2});
    TVMCompiler compiler(module);
    std::ostringstream out;
    std::istringstream in;
    TInterpreter interpreter(module, compiler, out, in);
    EXPECT_EQ(interpreter.EvalRaw(function, {}, {}), 17);
    ASSERT_EQ(function.Blocks[1].Phis.size(), 1);
    TVMCompiler recompiled(module);
    function.Exec = &recompiled.Compile(function);
    EXPECT_EQ(interpreter.EvalRaw(function, {}, {}), 17);
}

TEST(VMAllocation, ParallelCyclesUseOneScratchAndSplitTheConditionalEdge) {
    for (int width : {8, 16, 32}) {
        for (int count : {2, 3}) {
            for (int alignment : {width == 16 ? 16 : 8, 64}) {
                SCOPED_TRACE(width);
                SCOPED_TRACE(count);
                SCOPED_TRACE(alignment);
                TTypeTable types;
                const int type = width == 8
                    ? types.I(EKind::I64)
                    : (width == 16 ? types.I(EKind::I128) : types.Vec(types.I(EKind::U8), 32));
                TFunction function{.Blocks = {
                    {.Label = {0}, .Instrs = {{.Op = "cmp"_op,
                        .Operands = {TImm{1}, TLabel{1}, TLabel{2}}, .OperandCount = 3}}},
                    {.Label = {1}, .Instrs = {{.Op = "ret"_op}}},
                    {.Label = {2}, .Instrs = {{.Op = "ret"_op}}},
                }, .TmpTypes = std::vector<int>(2 * count, type),
                   .NextTmpIdx = 2 * count, .NextLabelIdx = 3};
                TRegisterAllocation allocation;
                for (int i = 0; i < count; ++i) {
                    allocation.Reserve(i, width, alignment);
                }
                for (int i = 0; i < count; ++i) {
                    allocation.Slots[count + i] = allocation.Slots.at((i + 1) % count);
                    function.Blocks[1].Phis.push_back({.Op = "phi"_op, .Dest = {count + i},
                        .Operands = {TTmp{i}, TLabel{0}}});
                }
                BuildCfg(function);
                LowerAllocatedPhis(function, allocation);
                ASSERT_EQ(function.Blocks.size(), 4);
                EXPECT_EQ(function.Blocks[0].Instrs.back().Operands[2].Label.Idx, 2);
                EXPECT_EQ(function.Blocks[0].Instrs.back().Operands[1].Label.Idx, 3);
                EXPECT_EQ(allocation.Size, count * std::max(width, alignment) + width);
                for (const auto& [tmp, slot] : allocation.Slots) {
                    EXPECT_EQ(slot.Offset % slot.Alignment, 0);
                }
                std::vector<char> registers(allocation.Size);
                for (int i = 0; i < count; ++i) {
                    std::memset(registers.data() + allocation.Slots.at(i).Offset, i + 1, width);
                }
                for (const auto& instr : function.Blocks[3].Instrs) {
                    if (instr.Op == "mov"_op) {
                        std::memcpy(registers.data() + allocation.Slots.at(instr.Dest.Idx).Offset,
                            registers.data() + allocation.Slots.at(instr.Operands[0].Tmp.Idx).Offset, width);
                    }
                }
                for (int i = 0; i < count; ++i) {
                    const std::vector<char> expected(width, i + 1);
                    EXPECT_EQ(std::memcmp(registers.data() + allocation.Slots.at(count + i).Offset,
                        expected.data(), width), 0);
                }
            }
        }
    }
}

TEST(VMAllocation, LongChainsReuseSlotsWithoutChangingVirtualIds) {
    TModule module;
    TBuilder builder(module);
    const int type = module.Types.I(EKind::I64);
    builder.NewFunction("chain", {}, 0);
    auto tmp = builder.Emit1("mov"_op, {TImm{0, type}});
    builder.SetType(tmp, type);
    for (int i = 0; i < 1000; ++i) {
        tmp = builder.Emit1("+"_op, {tmp, TImm{1, type}});
        builder.SetType(tmp, type);
    }
    builder.Emit0("ret"_op, {tmp});
    builder.SetReturnType(type);
    TVMCompiler compiler(module);
    std::ostringstream out;
    std::istringstream in;
    TInterpreter interpreter(module, compiler, out, in);
    auto& function = module.Functions[0];
    EXPECT_EQ(interpreter.EvalRaw(function, {}, {}), 1000);
    EXPECT_LE(function.Exec->RegisterFileSize, 16);
    EXPECT_EQ(function.Blocks[0].Instrs[1000].Dest.Idx, tmp.Idx);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
