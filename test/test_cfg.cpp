#include <gtest/gtest.h>

#include <qumir/parser/parser.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/type.h>
#include <qumir/ir/passes/analysis/cfg.h>
#include <qumir/ir/passes/transforms/locals2ssa.h>
#include <qumir/ir/passes/transforms/de_ssa.h>

#include <memory>
#include <sstream>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NPasses;

namespace {

// TODO: move to utils
std::string BuildIR(NAst::TTokenStream& ts, NIR::TModule& module, bool emitDebugInfo = false) {
    NSemantics::TNameResolver resolver;
    NRegistry::SystemModule sys;
    resolver.RegisterModule(&sys);
    resolver.ImportModule(sys.Name());

    NAst::TParser p;
    auto parsed = p.parse(ts, &resolver);
    if (!parsed) {
        return "Error: " + parsed.error().ToString() + "\n";
    }

    auto expr = parsed.value();
    auto error = NTransform::Pipeline(expr, resolver);
    if (!error) {
        return "Error: " + error.error().ToString() + "\n";
    }

    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver, {.EmitDebugInfo = emitDebugInfo});
    auto lowerRes = lowerer.LowerTop(expr);
    if (!lowerRes) {
        return "Error: " + lowerRes.error().ToString() + "\n";
    }

    std::ostringstream out;
    module.Print(out);
    return out.str();
}

} // namespace

TEST(IRType, VectorInterningPreservesElementTypeAndCount) {
    TTypeTable types;
    const int i64 = types.I(EKind::I64);
    const int f64 = types.I(EKind::F64);
    const int vec4 = types.Vec(i64, 4);
    EXPECT_EQ(types.Vec(i64, 4), vec4);
    EXPECT_NE(types.Vec(i64, 8), vec4);
    EXPECT_NE(types.Vec(f64, 4), vec4);
    EXPECT_EQ(types.GetKind(vec4), EKind::Vec);
    EXPECT_EQ(types.UnderlyingType(vec4), i64);
    EXPECT_EQ(types.VectorSize(vec4), 4);
}

TEST(IRType, VectorPrintingIncludesElementTypeAndCount) {
    TTypeTable types;
    types.I(EKind::I8);
    const int i64 = types.I(EKind::I64);
    const int vec4 = types.Vec(i64, 4);
    const int vec8 = types.Vec(i64, 8);
    std::ostringstream out;
    types.Print(out, vec4);
    out << " ";
    types.Print(out, vec8);
    EXPECT_EQ(out.str(), "vec<i64, 4> vec<i64, 8>");
}

TEST(IRType, VectorsAreDistinctFromScalarTypes) {
    TTypeTable types;
    const int i64 = types.I(EKind::I64);
    const int f64 = types.I(EKind::F64);
    EXPECT_TRUE(types.IsPrimitive(i64));
    EXPECT_TRUE(types.IsPrimitive(f64));
    EXPECT_FALSE(types.IsVector(i64));
    EXPECT_FALSE(types.IsVector(-1));
    for (int vector : {types.Vec(i64, 4), types.Vec(f64, 4)}) {
        EXPECT_TRUE(types.IsVector(vector));
        EXPECT_FALSE(types.IsPrimitive(vector));
        EXPECT_FALSE(types.IsInteger(vector));
        EXPECT_FALSE(types.IsFloat(vector));
        EXPECT_FALSE(types.IsSigned(vector));
        EXPECT_FALSE(types.IsUnsigned(vector));
    }
}

TEST(IRType, VectorPayloadUsesElementWidthWithoutPadding) {
    TTypeTable types;
    const int f32 = types.I(EKind::F32);
    EXPECT_EQ(types.SizeInBytes(f32), 4);
    EXPECT_EQ(types.SizeInBytes(types.Vec(f32, 4)), 16);
    EXPECT_EQ(types.SizeInBytes(types.Vec(types.I(EKind::F64), 4)), 32);
    EXPECT_EQ(types.SizeInBytes(types.Vec(types.I(EKind::I1), 2)), 2);
    EXPECT_EQ(types.SizeInBytes(types.Vec(types.I(EKind::I8), 32)), 32);
    EXPECT_EQ(types.SizeInBytes(types.Vec(types.I(EKind::I128), 32)), 512);
}

TEST(IRType, NestedStructAlignsLikeItsWidestField) {
    TTypeTable types;
    const int wide = types.Struct({types.I(EKind::I128)});
    const int pair = types.Struct({types.I(EKind::I64), types.I(EKind::I64)});
    EXPECT_EQ(types.AlignInBytes(wide), 16);
    EXPECT_EQ(types.AlignInBytes(pair), 8);

    const int flagged = types.Struct({types.I(EKind::I1), wide});
    EXPECT_EQ(types.FieldOffset(flagged, 1), 16);
    EXPECT_EQ(types.SizeInBytes(flagged), 32);
    const int flaggedPair = types.Struct({types.I(EKind::I1), pair});
    EXPECT_EQ(types.FieldOffset(flaggedPair, 1), 8);
    EXPECT_EQ(types.SizeInBytes(flaggedPair), 24);
}

TEST(IRType, AstVectorAliasesUseTheSameIRType) {
    TTypeTable types;
    auto vector = std::make_shared<NAst::TVectorType>(
        std::make_shared<NAst::TIntegerType>(NAst::TIntegerType::I32), 4);
    auto alias = std::make_shared<NAst::TNamedType>("V", vector);
    const int expected = types.Vec(types.I(EKind::I32), 4);
    EXPECT_EQ(FromAstType(vector, types), expected);
    EXPECT_EQ(FromAstType(alias, types), expected);
    EXPECT_EQ(FromAstType(std::make_shared<NAst::TPointerType>(alias), types), types.Ptr(expected));
}

TEST(CfgTest, InstructionDebugInfoIsOptional) {
    const std::string source = "алг main\nнач\n  цел x\n  x := 7\nкон\n";

    std::istringstream plainInput(source);
    NAst::TTokenStream plainTokens(plainInput);
    NIR::TModule plainModule;
    BuildIR(plainTokens, plainModule);
    ASSERT_EQ(plainModule.Functions.size(), 1);
    EXPECT_FALSE(plainModule.Functions[0].DebugInfo);
    EXPECT_TRUE(plainModule.Functions[0].LocalDebugInfo.empty());
    for (const auto& block : plainModule.Functions[0].Blocks) {
        EXPECT_TRUE(block.InstrDebugInfo.empty());
    }

    std::istringstream debugInput(source);
    NAst::TTokenStream debugTokens(debugInput);
    NIR::TModule debugModule;
    BuildIR(debugTokens, debugModule, true);
    ASSERT_EQ(debugModule.Functions.size(), 1);
    const auto& functionInfo = debugModule.Functions[0].DebugInfo;
    ASSERT_TRUE(functionInfo);
    EXPECT_EQ(functionInfo->Name, "main");
    EXPECT_EQ(functionInfo->Location.Line, 1);
    EXPECT_GE(functionInfo->ScopeId, 0);
    EXPECT_TRUE(NAst::TMaybeType<NAst::TFunctionType>(functionInfo->AstType));
    bool hasSourceLocation = false;
    bool hasNamedLocal = false;
    for (const auto& info : debugModule.Functions[0].LocalDebugInfo) {
        hasNamedLocal |= info.Name == "x";
    }
    EXPECT_TRUE(hasNamedLocal);
    for (const auto& block : debugModule.Functions[0].Blocks) {
        ASSERT_EQ(block.InstrDebugInfo.size(), block.Instrs.size());
        for (const auto& info : block.InstrDebugInfo) {
            EXPECT_GE(info.ScopeId, 0);
            hasSourceLocation |= info.Location.Line > 0;
        }
    }
    EXPECT_TRUE(hasSourceLocation);
}

TEST(CfgTest, DeSSAPreservesInstructionDebugInfo) {
    using namespace NIR::NLiterals;

    NIR::TModule module;
    NIR::TBuilder builder(module);
    builder.NewFunction("f", {}, 0);
    const int i64 = module.Types.I(EKind::I64);
    const TLabel entryLabel = builder.CurrentBlockLabel();
    const TLabel thenLabel = builder.NewLabel();
    const TLabel joinLabel = builder.NewLabel();
    const TInstrDebugInfo entryInfo{TLocation{1, 0, 0}, 1};
    const TInstrDebugInfo thenInfo{TLocation{2, 0, 0}, 1};
    const TInstrDebugInfo joinInfo{TLocation{3, 0, 0}, 1};

    builder.Emit0("cmp"_op, {TImm{1, i64}, thenLabel, joinLabel}, entryInfo);
    builder.NewBlock(thenLabel);
    builder.Emit0("jmp"_op, {joinLabel}, thenInfo);
    builder.NewBlock(joinLabel);
    auto phi = builder.Emit1(
        "phi"_op,
        {TImm{11, i64}, entryLabel, TImm{22, i64}, thenLabel},
        joinInfo);
    builder.SetType(phi, i64);
    builder.Emit0("ret"_op, {phi}, joinInfo);

    auto& function = module.Functions.front();
    BuildCfg(function);
    DeSSA(function, module);

    ASSERT_EQ(function.Blocks.size(), 4);
    for (const auto& block : function.Blocks) {
        ASSERT_EQ(block.InstrDebugInfo.size(), block.Instrs.size());
    }
    EXPECT_EQ(function.Blocks[1].InstrDebugInfo.back().Location.Line, 2);
    EXPECT_EQ(function.Blocks[2].InstrDebugInfo.back().Location.Line, 3);
    const auto& splitBlock = function.Blocks.back();
    ASSERT_EQ(splitBlock.Instrs.size(), 3);
    EXPECT_FALSE(splitBlock.InstrDebugInfo[0]);
    EXPECT_FALSE(splitBlock.InstrDebugInfo[1]);
    EXPECT_EQ(splitBlock.InstrDebugInfo.back().Location.Line, 1);
}

TEST(CfgTest, Basic) {
    std::string s = R"(
алг
нач
    цел ф
    ф := 0
    нц пока ф < 10
        ф := ф + 1
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);
    auto& function = module.Functions[0];
    BuildCfg(function);
    function.Print(std::cout, module);
    EXPECT_EQ(function.Blocks.size(), 5);
    EXPECT_EQ(function.Blocks[0].Succ.size(), 1);
    EXPECT_EQ(function.Blocks[1].Succ.size(), 2);
    EXPECT_EQ(function.Blocks[2].Succ.size(), 1);
    EXPECT_EQ(function.Blocks[3].Succ.size(), 1);

    EXPECT_EQ(function.Blocks[0].Pred.size(), 0);
    EXPECT_EQ(function.Blocks[1].Pred.size(), 2);
    EXPECT_EQ(function.Blocks[2].Pred.size(), 1);
    EXPECT_EQ(function.Blocks[3].Pred.size(), 1);

    EXPECT_EQ(function.Blocks[0].Succ, std::list<TLabel>{function.Blocks[1].Label});
    EXPECT_EQ(function.Blocks[1].Succ, (std::list<TLabel>{function.Blocks[2].Label, function.Blocks[3].Label}));
    EXPECT_EQ(function.Blocks[2].Succ, std::list<TLabel>{function.Blocks[1].Label});

    EXPECT_EQ(function.Blocks[1].Pred, (std::list<TLabel>{function.Blocks[0].Label, function.Blocks[2].Label}));
    EXPECT_EQ(function.Blocks[2].Pred, std::list<TLabel>{function.Blocks[1].Label});
    EXPECT_EQ(function.Blocks[3].Pred, std::list<TLabel>{function.Blocks[1].Label});
}

TEST(CfgTest, RPO) {
std::string s = R"(
алг
нач
    цел ф
    ф := 0
    нц пока ф < 10
        ф := ф + 1
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);

    BuildCfg(module.Functions[0]);
    std::vector<TLabel> rpo = ComputeRPO(module.Functions[0]);
    EXPECT_EQ(rpo, (std::vector<TLabel>{{0}, {2}, {4}, {1}, {3}}));
}

TEST(CfgTest, PromoteLocalsToSSAWhile) {
std::string s = R"(
алг
нач
    цел ф
    ф := 0
    нц пока ф < 10
        ф := ф + 1
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);

    auto& function = module.Functions[0];

    function.Print(std::cout, module);

    PromoteLocalsToSSA(function, module);

    function.Print(std::cout, module);
}

TEST(CfgTest, PromoteLocalsToSSAFor) {
std::string s = R"(
алг
нач
    цел ф, i
    ф := 0
    нц для i от 0 до 10 шаг 1
        ф := ф + 1
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);

    auto& function = module.Functions[0];

    function.Print(std::cout, module);

    PromoteLocalsToSSA(function, module);

    function.Print(std::cout, module);
}

TEST(CfgTest, PromoteLocalsToSSAFactorial) {
std::string s = R"(
алг цел факториал(цел число)
нач
    цел i
    знач := 1
    нц для i от 1 до число
        знач := знач * i
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);

    auto& function = module.Functions[0];

    function.Print(std::cout, module);

    PromoteLocalsToSSA(function, module);

    function.Print(std::cout, module);

    DeSSA(function, module);

    function.Print(std::cout, module);

}

TEST(CfgTest, PromoteLocalsToSSAContinue) {
std::string s = R"(
алг
нач
    цел i, s
    i := 0
    s := 0
    нц пока i < 4
        если mod(i, 2) = 0 то
            i := i + 1
            далее
        все
        s := s + i
        i := i + 1
    кц
кон
    )";

    std::istringstream ss(s);
    NAst::TTokenStream ts(ss);
    NIR::TModule module;
    std::string got = BuildIR(ts, module);
    ASSERT_TRUE(module.Functions.size() == 1);

    auto& function = module.Functions[0];

    function.Print(std::cout, module);

    PromoteLocalsToSSA(function, module);

    function.Print(std::cout, module);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
