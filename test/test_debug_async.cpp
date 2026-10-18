#include <qumir/ir/builder.h>
#include <qumir/ir/passes/transforms/debug_async.h>

#include <gtest/gtest.h>

using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

TEST(DebugAsync, RecursiveCallerClosureAndIdempotence) {
    TModule module;
    module.DebugOptions.EmitDebugPoints = true;
    const int resultType = module.Types.I(EKind::I64);
    const int voidType = module.Types.I(EKind::Void);
    const int futureType = module.Types.Ptr(voidType);
    module.ExternalFunctions.push_back({
        .Name = "__qumir_debug_point",
        .ReturnTypeId = voidType,
        .SymId = 100,
    });
    TBuilder builder(module);
    // Only the leaf contains a checkpoint. The other functions exercise
    // propagation through forward references and a mutually recursive cycle.
    for (int id = 1; id <= 4; ++id) {
        builder.NewFunction(std::to_string(id), {}, id);
        builder.SetReturnType(resultType);
        auto value = builder.Emit1("call"_op, {TImm{id == 1 ? 2 : id == 2 ? 3 : 1}}, {{7, 1, 1}, 9});
        builder.SetType(value, resultType);
        if (id == 3) {
            builder.Emit0("call"_op, {TImm{100}});
        }
        builder.Emit0("ret"_op, {value});
    }
    // An unrelated function must retain its synchronous ABI.
    builder.NewFunction("sync", {}, 5);
    builder.SetReturnType(resultType);
    builder.Emit0("ret"_op, {TImm{7, resultType}});

    NPasses::LowerDebugAsync(module);
    for (int i = 0; i < 4; ++i) {
        const auto& function = module.Functions[i];
        EXPECT_TRUE(function.IsCoroutine);
        EXPECT_EQ(function.ReturnTypeId, futureType);
        EXPECT_EQ(function.CoroutineResultTypeId, resultType);
        const auto& instructions = function.Blocks[0].Instrs;
        ASSERT_EQ(function.Blocks[0].InstrDebugInfo.size(), instructions.size());
        EXPECT_EQ(function.Blocks[0].InstrDebugInfo[0].Location.Line, 7);
        EXPECT_EQ(function.Blocks[0].InstrDebugInfo[1].Location.Line, 7);
        EXPECT_EQ(function.Blocks[0].InstrDebugInfo[1].ScopeId, 9);
        EXPECT_EQ(instructions[0].Op, "call"_op);
        EXPECT_EQ(function.GetType(instructions[0].Dest), futureType);
        EXPECT_EQ(instructions[1].Op, "await"_op);
        EXPECT_EQ(function.GetType(instructions[1].Dest), resultType);
    }
    EXPECT_FALSE(module.Functions[4].IsCoroutine);
    EXPECT_EQ(module.Functions[4].ReturnTypeId, resultType);
    const auto size = module.Functions[2].Blocks[0].Instrs.size();
    NPasses::LowerDebugAsync(module);
    EXPECT_EQ(module.Functions[2].Blocks[0].Instrs.size(), size);
}

TEST(DebugAsync, ExistingFutureCallIsNotAwaitedTwice) {
    TModule module;
    module.DebugOptions.EmitDebugPoints = true;
    const int voidType = module.Types.I(EKind::Void);
    const int futureType = module.Types.Ptr(voidType);
    module.ExternalFunctions.push_back({
        .Name = "__qumir_debug_point", .ReturnTypeId = voidType, .SymId = 100,
    });
    TBuilder builder(module);
    builder.NewFunction("explicit", {}, 1);
    builder.SetReturnType(futureType);
    module.Functions.back().IsCoroutine = true;
    module.Functions.back().CoroutineResultTypeId = voidType;
    builder.Emit0("call"_op, {TImm{100}});
    auto future = builder.Emit1("call"_op, {TImm{1}});
    builder.SetType(future, futureType);
    builder.Emit0("await"_op, {future});
    builder.Emit0("ret"_op, {});
    NPasses::LowerDebugAsync(module);
    const auto& instructions = module.Functions[0].Blocks[0].Instrs;
    ASSERT_EQ(instructions.size(), 5);
    EXPECT_EQ(instructions[2].Dest, future);
    EXPECT_EQ(instructions[3].Operands[0].Tmp, future);
}

TEST(DebugAsync, AwaitPromotesLifecycleAndCallersWithoutDebugPoints) {
    TModule module;
    const int voidType = module.Types.I(EKind::Void);
    const int futureType = module.Types.Ptr(voidType);
    TBuilder builder(module);
    module.ModuleConstructorFunctionId = builder.NewFunction("$$module_constructor", {}, -1);
    builder.SetReturnType(voidType);
    auto future = builder.Emit1("call"_op, {TImm{100}});
    builder.SetType(future, futureType);
    builder.Emit0("await"_op, {future});
    builder.Emit0("ret"_op, {});
    builder.NewFunction("caller", {}, 1);
    builder.SetReturnType(voidType);
    builder.Emit0("call"_op, {TImm{-1}});
    builder.Emit0("ret"_op, {});

    NPasses::LowerDebugAsync(module);
    const auto& constructor = module.Functions[0];
    EXPECT_TRUE(constructor.IsCoroutine);
    EXPECT_EQ(constructor.CoroutineResultTypeId, voidType);
    EXPECT_EQ(constructor.ReturnTypeId, futureType);
    ASSERT_EQ(constructor.Blocks[0].Instrs.size(), 3);
    EXPECT_EQ(constructor.Blocks[0].Instrs[0].Dest, future);
    const auto& caller = module.Functions[1];
    EXPECT_TRUE(caller.IsCoroutine);
    ASSERT_EQ(caller.Blocks[0].Instrs.size(), 3);
    EXPECT_EQ(caller.Blocks[0].Instrs[1].Op, "await"_op);
    NPasses::LowerDebugAsync(module);
    EXPECT_EQ(module.Functions[1].Blocks[0].Instrs.size(), 3);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
