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
#include <set>
#include <sstream>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NPasses;
using namespace NQumir::NIR::NLiterals;

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

// PromoteLocalsToSSA must not leave a use of a phi it removed as trivial.
void ExpectDefinedTmps(const NIR::TFunction& function) {
    std::set<int> defined;
    for (const auto& block : function.Blocks) {
        for (const auto& phi : block.Phis) {
            defined.insert(phi.Dest.Idx);
        }
        for (const auto& instr : block.Instrs) {
            defined.insert(instr.Dest.Idx);
        }
    }
    auto check = [&](const NIR::TOperand& operand) {
        EXPECT_TRUE(operand.Type != NIR::TOperand::EType::Tmp || defined.contains(operand.Tmp.Idx))
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

// Emits straight-line IR over i64 locals into the current block of `Builder`.
struct TLocalsFunction {
    TLocalsFunction(int localCount, int argCount = 0) {
        std::vector<TLocal> args;
        for (int i = 0; i < argCount; ++i) {
            args.push_back(TLocal{i});
        }
        Builder.NewFunction("f", args, 0);
        Builder.SetReturnType(I64);
        for (int i = 0; i < localCount; ++i) {
            Builder.SetType(TLocal{i}, I64, {});
        }
    }

    TTmp Load(int local) {
        auto tmp = Builder.Emit1("load"_op, {TLocal{local}});
        Builder.SetType(tmp, I64);
        return tmp;
    }

    TTmp Op(TOp op, TOperand left, TOperand right, bool isCompare = false) {
        auto tmp = Builder.Emit1(op, {left, right});
        Builder.SetType(tmp, isCompare ? I1 : I64);
        return tmp;
    }

    void Store(int local, TOperand value) {
        Builder.Emit0("stre"_op, {TLocal{local}, value});
    }

    TImm Imm(int64_t value) const {
        return TImm{value, I64};
    }

    // Starts a block with a preallocated label.
    void Begin(TLabel label) {
        Builder.NewBlock(label);
    }

    void Jmp(TLabel label) {
        Builder.Emit0("jmp"_op, {label});
    }

    void Cmp(TOperand cond, TLabel ifTrue, TLabel ifFalse) {
        Builder.Emit0("cmp"_op, {cond, ifTrue, ifFalse});
    }

    NIR::TModule Module;
    NIR::TBuilder Builder{Module};
    const int I64 = Module.Types.I(EKind::I64);
    const int I1 = Module.Types.I(EKind::I1);
};

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

TEST(CfgTest, PromoteLocalsToSSASelfReferencingPhiInUnreachableLoop) {
    // A loop that cannot be entered from the entry gives a phi made only of its own value.
    NIR::TModule module;
    NIR::TBuilder builder(module);
    const int i64 = module.Types.I(EKind::I64);
    const int i1 = module.Types.I(EKind::I1);
    builder.NewFunction("f", {}, 0);
    builder.SetReturnType(i64);
    builder.SetType(TLocal{0}, i64, {});
    const TLabel join = builder.NewLabel(), exit = builder.NewLabel(), loop = builder.NewLabel();
    builder.Emit0("jmp"_op, {join});
    builder.NewBlock(join);
    auto value = builder.Emit1("load"_op, {TLocal{0}});
    builder.SetType(value, i64);
    builder.Emit0("ret"_op, {value});
    builder.NewBlock(exit);
    auto exitCond = builder.Emit1("=="_op, {TImm{0, i64}, TImm{0, i64}});
    builder.SetType(exitCond, i1);
    builder.Emit0("cmp"_op, {exitCond, join, loop});
    builder.NewBlock(loop);
    auto loopCond = builder.Emit1("=="_op, {TImm{0, i64}, TImm{0, i64}});
    builder.SetType(loopCond, i1);
    builder.Emit0("cmp"_op, {loopCond, exit, loop});

    auto& function = module.Functions.front();
    BuildCfg(function);
    PromoteLocalsToSSA(function, module);
    ExpectDefinedTmps(function);
}

TEST(CfgTest, PromoteLocalsToSSAExitFromNestedLoopsInsideLoop) {
    // A search loop nested in an outer loop leaves both loops for the code after the search.
    // Locals: 0 total, 1 row, 2 g, 3 probes, 4 m, 5 found.
    TLocalsFunction f(6);
    auto& b = f.Builder;
    const TLabel outerCond = b.NewLabel(), outerBody = b.NewLabel(), probeCond = b.NewLabel();
    const TLabel probeBody = b.NewLabel(), mCond = b.NewLabel(), mBody = b.NewLabel();
    const TLabel mStep = b.NewLabel(), mEnd = b.NewLabel(), probeEnd = b.NewLabel();
    const TLabel found = b.NewLabel(), after = b.NewLabel(), end = b.NewLabel();

    f.Store(0, f.Imm(0));
    f.Store(1, f.Imm(0));
    f.Jmp(outerCond);

    f.Begin(outerCond);
    f.Cmp(f.Op("<"_op, f.Load(1), f.Imm(6), true), outerBody, end);

    f.Begin(outerBody);
    f.Store(2, f.Op("&"_op, f.Load(1), f.Imm(3)));
    f.Store(3, f.Imm(0));
    f.Jmp(probeCond);

    f.Begin(probeCond);
    f.Cmp(f.Op("<"_op, f.Load(3), f.Imm(4), true), probeBody, probeEnd);

    f.Begin(probeBody);
    f.Store(4, f.Op("+"_op, f.Load(2), f.Imm(1)));
    f.Jmp(mCond);

    f.Begin(mCond);
    f.Cmp(f.Op("!="_op, f.Load(4), f.Imm(0), true), mBody, mEnd);

    f.Begin(mBody);
    f.Cmp(f.Op("=="_op, f.Op("+"_op, f.Load(2), f.Load(4)), f.Imm(3), true), found, mStep);

    f.Begin(mStep);
    f.Store(4, f.Op("-"_op, f.Load(4), f.Imm(1)));
    f.Jmp(mCond);

    f.Begin(mEnd);
    f.Store(2, f.Op("&"_op, f.Op("+"_op, f.Load(2), f.Imm(1)), f.Imm(3)));
    f.Store(3, f.Op("+"_op, f.Load(3), f.Imm(1)));
    f.Jmp(probeCond);

    f.Begin(found);
    f.Store(5, f.Op("+"_op, f.Load(2), f.Load(4)));
    f.Jmp(after);

    f.Begin(probeEnd);
    f.Store(5, f.Imm(-1));
    f.Jmp(after);

    f.Begin(after);
    f.Store(0, f.Op("+"_op, f.Op("*"_op, f.Load(0), f.Imm(10)), f.Load(5)));
    f.Store(1, f.Op("+"_op, f.Load(1), f.Imm(1)));
    f.Jmp(outerCond);

    f.Begin(end);
    b.Emit0("ret"_op, {f.Load(0)});

    auto& function = f.Module.Functions.front();
    BuildCfg(function);
    PromoteLocalsToSSA(function, f.Module);
    ExpectDefinedTmps(function);
}

TEST(CfgTest, PromoteLocalsToSSATrivialPhisInsideLoop) {
    // Two diamonds join a value in front of and inside a loop; some of their phis are trivial,
    // and removing one leads to the phi that is being removed.
    // Locals: 0 flag (argument), 1 shift, 2 capacity, 3 g, 4 probes, 5 step, 6 found.
    TLocalsFunction f(7, 1);
    auto& b = f.Builder;
    const TLabel shiftA = b.NewLabel(), shiftB = b.NewLabel(), shiftJoin = b.NewLabel();
    const TLabel cond = b.NewLabel(), body = b.NewLabel(), stepA = b.NewLabel(), stepB = b.NewLabel();
    const TLabel stepJoin = b.NewLabel(), hit = b.NewLabel(), next = b.NewLabel(), end = b.NewLabel();

    f.Cmp(f.Op("=="_op, f.Load(0), f.Imm(2), true), shiftA, shiftB);

    f.Begin(shiftA);
    f.Store(1, f.Imm(4));
    f.Jmp(shiftJoin);

    f.Begin(shiftB);
    f.Store(1, f.Imm(3));
    f.Jmp(shiftJoin);

    f.Begin(shiftJoin);
    f.Store(2, f.Imm(64));
    f.Store(3, f.Op("&"_op, f.Op(">>"_op, f.Load(2), f.Load(1)), f.Imm(7)));
    f.Store(4, f.Imm(0));
    f.Jmp(cond);

    f.Begin(cond);
    f.Cmp(f.Op("<"_op, f.Load(4), f.Load(2), true), body, end);

    f.Begin(body);
    f.Cmp(f.Op("=="_op, f.Load(0), f.Imm(2), true), stepA, stepB);

    f.Begin(stepA);
    f.Store(5, f.Imm(4));
    f.Jmp(stepJoin);

    f.Begin(stepB);
    f.Store(5, f.Imm(3));
    f.Jmp(stepJoin);

    f.Begin(stepJoin);
    f.Store(6, f.Op("<<"_op, f.Load(3), f.Load(5)));
    f.Cmp(f.Op("!="_op, f.Op("&"_op, f.Load(6), f.Imm(64)), f.Imm(0), true), hit, next);

    f.Begin(hit);
    b.Emit0("ret"_op, {f.Op("+"_op, f.Load(6), f.Load(1))});

    f.Begin(next);
    f.Store(3, f.Op("&"_op, f.Op("+"_op, f.Load(3), f.Imm(1)), f.Imm(7)));
    f.Store(4, f.Op("+"_op, f.Load(4), f.Imm(1)));
    f.Jmp(cond);

    f.Begin(end);
    b.Emit0("ret"_op, {f.Imm(-1)});

    auto& function = f.Module.Functions.front();
    BuildCfg(function);
    PromoteLocalsToSSA(function, f.Module);
    ExpectDefinedTmps(function);
}

TEST(CfgTest, PromoteLocalsToSSAUnreachableSinglePredecessorCycles) {
    // Unreachable blocks form a self-loop and a two-block cycle, each block with one predecessor.
    NIR::TModule module;
    NIR::TBuilder builder(module);
    const int i64 = module.Types.I(EKind::I64);
    const int i1 = module.Types.I(EKind::I1);
    builder.NewFunction("f", {}, 0);
    builder.SetReturnType(i64);
    builder.SetType(TLocal{0}, i64, {});
    const TLabel join = builder.NewLabel();
    const TLabel self = builder.NewLabel();
    const TLabel first = builder.NewLabel();
    const TLabel second = builder.NewLabel();
    builder.Emit0("jmp"_op, {join});
    builder.NewBlock(join);
    auto value = builder.Emit1("load"_op, {TLocal{0}});
    builder.SetType(value, i64);
    builder.Emit0("ret"_op, {value});
    builder.NewBlock(self);
    auto selfCond = builder.Emit1("=="_op, {TImm{0, i64}, TImm{0, i64}});
    builder.SetType(selfCond, i1);
    builder.Emit0("cmp"_op, {selfCond, self, join});
    builder.NewBlock(first);
    auto firstCond = builder.Emit1("=="_op, {TImm{0, i64}, TImm{0, i64}});
    builder.SetType(firstCond, i1);
    builder.Emit0("cmp"_op, {firstCond, second, join});
    builder.NewBlock(second);
    builder.Emit0("jmp"_op, {first});

    auto& function = module.Functions.front();
    BuildCfg(function);
    PromoteLocalsToSSA(function, module);

    // The local is never stored, so the join reads undef from every predecessor.
    const TInstr* ret = nullptr;
    for (const auto& block : function.Blocks) {
        for (const auto& instr : block.Instrs) {
            EXPECT_NE(instr.Op, "load"_op);
            if (instr.Op == "ret"_op) {
                ret = &instr;
            }
        }
    }
    ASSERT_NE(ret, nullptr);
    ASSERT_EQ(ret->OperandCount, 1);
    ASSERT_EQ(ret->Operands[0].Type, TOperand::EType::Imm);
    EXPECT_EQ(module.Types.GetKind(ret->Operands[0].Imm.TypeId), EKind::Undef);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
