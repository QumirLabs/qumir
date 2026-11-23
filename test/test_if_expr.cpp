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
#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/codegen/llvm/llvm_runner.h>

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
    EBackend backend = EBackend::VM)
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
            funcBody = funDecl->Body();
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
                return {{}, std::string("llvm run error: ") + runErr, ir.str()};
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

TExprPtr fnum(double value) {
    return MakeIntrusive<TNumberExpr>(TLocation{}, value, NQumir::NTest::TestTypes());
}

TExprPtr str(std::string value) {
    return MakeIntrusive<TStringLiteralExpr>(TLocation{}, std::move(value), NQumir::NTest::TestTypes());
}

TExprPtr ident(const std::string& name) {
    return MakeIntrusive<TIdentExpr>(TLocation{}, name);
}

TExprPtr binary(const std::string& op, const TExprPtr& left, const TExprPtr& right) {
    return MakeIntrusive<TBinaryExpr>(TLocation{}, TOperator(op), left, right);
}

TExprPtr call(const std::string& name, std::vector<TExprPtr> args) {
    return MakeIntrusive<TCallExpr>(
        TLocation{},
        MakeIntrusive<TIdentExpr>(TLocation{}, name),
        std::move(args));
}

TExprPtr ifExpr(const TExprPtr& cond, const TExprPtr& thenExpr, const TExprPtr& elseExpr) {
    return MakeIntrusive<TIfExpr>(
        TLocation{},
        cond,
        thenExpr,
        elseExpr);
}

TExprPtr letExpr(std::string name, const TExprPtr& value, const TExprPtr& body) {
    auto var = MakeIntrusive<TVarStmt>(TLocation{}, std::move(name), nullptr);
    var->Init() = value;
    return MakeIntrusive<TBlockExpr>(TLocation{}, std::vector<TExprPtr>{std::move(var), body});
}

TExprPtr assign(const std::string& name, const TExprPtr& value) {
    return MakeIntrusive<TAssignExpr>(TLocation{}, name, value);
}

TRunResult RunIntIfExpr(const TExprPtr& expr, EBackend backend = EBackend::VM) {
    const std::string src = "алг\nнач\n  цел x\n  вывод x, нс\nкон\n";
    return RunWithInjection(src, 1, {assign("x", expr)}, backend);
}

TRunResult RunProgram(const std::string& src, EBackend backend = EBackend::VM) {
    return RunWithInjection(src, 0, {}, backend);
}

} // namespace

TEST(IfExpr, VMExecutesThenBranch) {
    auto result = RunIntIfExpr(ifExpr(num(1), num(10), num(20)));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "10\n");
}

TEST(IfExpr, VMExecutesElseBranch) {
    auto result = RunIntIfExpr(ifExpr(num(0), num(10), num(20)));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "20\n");
}

TEST(IfExpr, LLVMExecutesBranches) {
    auto thenResult = RunIntIfExpr(ifExpr(num(1), num(10), num(20)), EBackend::LLVM);
    ASSERT_TRUE(thenResult.ok()) << thenResult.error;
    EXPECT_EQ(thenResult.output, "10\n");

    auto elseResult = RunIntIfExpr(ifExpr(num(0), num(10), num(20)), EBackend::LLVM);
    ASSERT_TRUE(elseResult.ok()) << elseResult.error;
    EXPECT_EQ(elseResult.output, "20\n");
}

TEST(IfExpr, LoweringEmitsPhi) {
    auto result = RunIntIfExpr(ifExpr(num(1), num(10), num(20)));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.ir.find("phi"), std::string::npos) << result.ir;
}

TEST(IfExpr, TypeAnnotationCastsNumericBranchesToCommonType) {
    const std::string src = "алг\nнач\n  вещ x\n  вывод x, нс\nкон\n";
    auto result = RunWithInjection(src, 1, {
        assign("x", ifExpr(num(1), num(7), fnum(2.5))),
    });

    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.ir.find("phi"), std::string::npos) << result.ir;
    EXPECT_NE(result.ir.find("f64"), std::string::npos) << result.ir;
    EXPECT_NE(result.output.find("7"), std::string::npos) << result.output;
}

TEST(IfExpr, TypeAnnotationRejectsMissingElse) {
    auto result = RunIntIfExpr(ifExpr(num(1), num(10), nullptr));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("if-expression"), std::string::npos) << result.error;
}

TEST(IfExpr, TypeAnnotationRejectsIncompatibleBranches) {
    auto result = RunIntIfExpr(ifExpr(num(1), num(10), str("bad")));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("if-expression"), std::string::npos) << result.error;
}

TEST(LetExpr, VMExecutesBodyWithBinding) {
    auto expr = letExpr("tmp", num(21), binary("+", ident("tmp"), ident("tmp")));
    auto result = RunIntIfExpr(std::move(expr));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "42\n");
}

TEST(LetExpr, LLVMExecutesBodyWithBinding) {
    auto expr = letExpr("tmp", num(21), binary("+", ident("tmp"), ident("tmp")));
    auto result = RunIntIfExpr(std::move(expr), EBackend::LLVM);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "42\n");
}

TEST(LetExpr, EvaluatesBindingExactlyOnce) {
    const std::string src = R"(алг
нач
  цел g
  цел y
  g := 0
  вывод y, " ", g, нс
кон

алг цел bump(арг рез цел v)
нач
  v := v + 1
  знач := v
кон
)";

    auto expr = letExpr(
        "tmp",
        call("bump", {ident("g")}),
        binary("+", ident("tmp"), ident("tmp")));
    auto result = RunWithInjection(src, 3, {assign("y", std::move(expr))});
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "2 1\n");
}

TEST(SystemInline, IntMinMaxAbsSignUseIfExpr) {
    const std::string src = R"(алг
нач
  цел x
  x := imin(7, 3)
  вывод x, нс
  x := imax(7, 3)
  вывод x, нс
  x := iabs(-5)
  вывод x, нс
  x := sign(-2.0)
  вывод x, нс
кон
)";

    auto result = RunProgram(src);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "3\n7\n5\n-1\n");
    EXPECT_NE(result.ir.find("phi"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("min_int64_t"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("max_int64_t"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("labs"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("sign"), std::string::npos) << result.ir;
}

TEST(SystemInline, NestedIntMinUsesDistinctLetScopes) {
    const std::string src = R"(алг
нач
  цел a,b,c,d,x
  a := 4
  b := 3
  c := 2
  d := 1
  x := imin(imin(imin(a,b),c),d)
  вывод x, нс
кон
)";

    auto result = RunProgram(src);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.output, "1\n");
    EXPECT_EQ(result.ir.find("min_int64_t"), std::string::npos) << result.ir;
}

TEST(SystemInline, FloatMinMaxAbsUseIfExpr) {
    const std::string src = R"(алг
нач
  вещ x
  x := min(2.5, -1.5)
  вывод x, нс
  x := max(2.5, -1.5)
  вывод x, нс
  x := abs(-3.25)
  вывод x, нс
кон
)";

    auto result = RunProgram(src);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.output.find("-1.5"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("2.5"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("3.25"), std::string::npos) << result.output;
    EXPECT_NE(result.ir.find("phi"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("min_double"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("max_double"), std::string::npos) << result.ir;
    EXPECT_EQ(result.ir.find("fabs"), std::string::npos) << result.ir;
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
