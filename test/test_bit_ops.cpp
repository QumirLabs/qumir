#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/runtime/io.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/eval.h>
#include <qumir/ir/passes/transforms/pipeline.h>
#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/codegen/llvm/llvm_runner.h>

#include <limits>
#include <sstream>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

struct TRunResult {
    std::string output;
    std::string error;
    std::string ir;
    bool ok() const { return error.empty(); }
};

enum class EBackend {
    VM,
    LLVM,
};

TRunResult RunWithInjection(
    const std::string& src,
    size_t insertAt,
    std::vector<TExprPtr> injected,
    EBackend backend = EBackend::VM,
    bool optimizeIr = false)
{
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());
    auto sysMod = std::make_shared<NRegistry::SystemModule>(NQumir::NTest::TestTypes());
    resolver.RegisterModule(sysMod.get());
    resolver.ImportModule(sysMod->Name());

    std::istringstream in(src);
    TTokenStream ts(in);
    TParser p(NQumir::NTest::TestTypes());
    auto parsed = p.parse(ts, &resolver);
    if (!parsed) return {{}, parsed.error().ToString()};
    auto ast = std::move(parsed.value());

    auto* topBlock = dynamic_cast<TBlockExpr*>(ast.Get());
    if (!topBlock) return {{}, "expected TBlockExpr at top level"};

    TBlockExpr* funcBody = nullptr;
    for (auto& stmt : topBlock->Stmts) {
        if (auto* funDecl = dynamic_cast<TFunDecl*>(stmt.Get())) {
            funcBody = funDecl->Body.Get();
            break;
        }
    }
    if (!funcBody) return {{}, "expected TFunDecl in top-level block"};

    auto it = funcBody->Stmts.begin() + static_cast<ptrdiff_t>(insertAt);
    funcBody->Stmts.insert(it, injected.begin(), injected.end());

    auto scope = resolver.GetOrCreateRootScope();
    scope->AllowsRedeclare = true;
    scope->RootLevel = false;

    if (auto err = NTransform::Pipeline(ast, resolver); !err) {
        return {{}, err.error().ToString()};
    }

    NIR::TModule module;
    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver);
    if (auto lowerRes = lowerer.LowerTop(ast); !lowerRes) {
        return {{}, lowerRes.error().ToString()};
    }

    auto* mainFun = module.GetEntryPoint();
    if (!mainFun) return {{}, "no main function"};
    if (optimizeIr) {
        NIR::NPasses::Pipeline(module);
        mainFun = module.GetEntryPoint();
        if (!mainFun) return {{}, "no main function after optimization"};
    }

    std::ostringstream ir;
    module.Print(ir);

    std::ostringstream out;
    NRuntime::SetOutputStream(&out);
    std::istringstream stdinIn;
    NRuntime::SetInputStream(&stdinIn);
    try {
        if (backend == EBackend::VM) {
            NIR::TVMCompiler compiler(module);
            NIR::TInterpreter interp(module, compiler, out, stdinIn);
            interp.Eval(*mainFun, {}, {});
        } else {
            NCodeGen::TLLVMCodeGen cg({});
            auto artifacts = cg.Emit(module, 0);
            NCodeGen::TLlvmRunner runner;
            std::string runErr;
            runner.Run(std::move(artifacts), mainFun->Name, &runErr, mainFun->ReturnTypeIsString);
            if (!runErr.empty()) {
                NRuntime::SetOutputStream(nullptr);
                return {{}, std::string("llvm run error: ") + runErr};
            }
        }
    } catch (const std::exception& e) {
        NRuntime::SetOutputStream(nullptr);
        return {{}, std::string("runtime error: ") + e.what(), ir.str()};
    }
    NRuntime::SetOutputStream(nullptr);
    return {out.str(), {}, ir.str()};
}

TExprPtr num(int64_t value) {
    return MakeIntrusive<TNumberExpr>(TLocation{}, value, NQumir::NTest::TestTypes());
}

TExprPtr ident(const std::string& name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, name);
}

TExprPtr binary(const std::string& op, const TExprPtr& left, const TExprPtr& right) {
    return MakeIntrusive<TBinaryExpr>(TLocation{}, TOperator(op), left, right);
}

TExprPtr unary(const std::string& op, const TExprPtr& operand) {
    return MakeIntrusive<TUnaryExpr>(TLocation{}, TOperator(op), operand);
}

TExprPtr assignX(const TExprPtr& value) {
    return MakeIntrusive<TAssignExpr>(TLocation{}, "x", value);
}

TRunResult RunBitExpr(const TExprPtr& expr, EBackend backend = EBackend::VM, bool optimizeIr = false) {
    const std::string src = "алг\nнач\n  цел x\n  вывод x, нс\nкон\n";
    return RunWithInjection(src, 1, {assignX(expr)}, backend, optimizeIr);
}

} // namespace

void ExpectBinaryOps(EBackend backend) {
    struct TCase {
        std::string op;
        int64_t left;
        int64_t right;
        std::string expected;
    };

    const std::vector<TCase> cases = {
        {"&",   6, 3, "2\n"},
        {"|",   4, 1, "5\n"},
        {"^",   7, 3, "4\n"},
        {"<<",  1, 8, "256\n"},
        {">>",  256, 4, "16\n"},
        {">>", -1, 1, "-1\n"},
        {"<<",  1, 64, "1\n"},
    };

    for (const auto& c : cases) {
        auto result = RunBitExpr(binary(c.op, num(c.left), num(c.right)), backend);
        ASSERT_TRUE(result.ok()) << c.op << ": " << result.error;
        EXPECT_EQ(result.output, c.expected) << c.op;
    }
}

void ExpectUnaryBitNot(EBackend backend) {
    auto result = RunBitExpr(unary("~", num(0)), backend);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "-1\n");
}

TEST(BitOps, VMExecutesBinaryOps) {
    ExpectBinaryOps(EBackend::VM);
}

TEST(BitOps, VMExecutesUnaryBitNot) {
    ExpectUnaryBitNot(EBackend::VM);
}

TEST(BitOps, LLVMExecutesBinaryOps) {
    ExpectBinaryOps(EBackend::LLVM);
}

TEST(BitOps, LLVMExecutesUnaryBitNot) {
    ExpectUnaryBitNot(EBackend::LLVM);
}

TEST(BitOps, ConstFoldEliminatesLiteralBitOps) {
    auto expr = binary("^",
        binary("|",
            binary("&", num(6), num(3)),
            binary("<<", num(1), num(8))),
        unary("~", num(0)));

    auto result = RunBitExpr(std::move(expr), EBackend::VM, true);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "-259\n");
    EXPECT_EQ(result.ir.find(" & "), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find(" | "), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find(" ^ "), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find(" << "), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find(" ~ "), std::string::npos) << result.ir;
}

TEST(BitOps, ConstFoldWrapsIntegerOverflow) {
    constexpr int64_t Max = std::numeric_limits<int64_t>::max();
    constexpr int64_t Min = std::numeric_limits<int64_t>::min();
    auto fold = [&](const char* op, int64_t a, int64_t b) {
        auto result = RunBitExpr(binary(op, num(a), num(b)), EBackend::VM, true);
        EXPECT_TRUE(result.ok()) << result.error;
        return result.output;
    };
    EXPECT_EQ(fold("+", Max, 1), std::to_string(Min) + "\n");
    EXPECT_EQ(fold("-", Min, 1), std::to_string(Max) + "\n");
    EXPECT_EQ(fold("*", Max, 2), "-2\n");
}

TEST(BitOps, TypeRejectsNonIntegerOperands) {
    const std::string src = "алг\nнач\n  цел x\n  вывод x, нс\nкон\n";
    auto result = RunWithInjection(src, 1, {
        assignX(binary("&", MakeIntrusive<TNumberExpr>(TLocation{}, 1.5, NQumir::NTest::TestTypes()), num(1))),
    });

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("Битовые операции"), std::string::npos) << result.error;
}

TEST(BitOps, XorRejectsNonIntegerOperands) {
    const std::string src = "алг\nнач\n  цел x\n  вывод x, нс\nкон\n";
    auto result = RunWithInjection(src, 1, {
        assignX(binary("^", MakeIntrusive<TNumberExpr>(TLocation{}, 1.5, NQumir::NTest::TestTypes()), num(1))),
    });

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("Битовые операции"), std::string::npos) << result.error;
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
