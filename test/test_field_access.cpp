#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/parser/ast.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/runtime/io.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/ir/eval.h>

#include <sstream>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

// Parse source, inject extra stmts into the top-level block at `insertAt`,
// run full pipeline + interpreter, return stdout or error string.
struct TRunResult {
    std::string output;
    std::string error;
    bool ok() const { return error.empty(); }
};

TRunResult RunWithInjection(
    const std::string& src,
    size_t insertAt,
    std::vector<TExprPtr> injected)
{
    NSemantics::TNameResolver resolver(NQumir::NTest::TestTypes());
    auto sysMod = std::make_shared<NRegistry::SystemModule>(NQumir::NTest::TestTypes());
    resolver.RegisterModule(sysMod.get());
    resolver.ImportModule(sysMod->Name());

    std::istringstream in(src);
    TTokenStream ts(in);
    TParser p(NQumir::NTest::TestTypes());
    NFrontend::TSourceModuleLoader loader(NQumir::NTest::TestTypes());
    auto parsed = p.parse(ts, &resolver, &loader);
    if (!parsed) {
        return {{}, parsed.error().ToString()};
    }
    auto ast = std::move(parsed.value());
    auto mainPragmas = ts.GetContext()->GetPragmas();
    resolver.ApplyPragmas(mainPragmas);

    auto composed = NFrontend::LoadAndCompose(loader, ast, mainPragmas);
    if (!composed) {
        return {{}, composed.error().ToString()};
    }
    ast = std::move(composed->Ast);
    resolver.ApplyPragmas(composed->Pragmas);

    auto* topBlock = dynamic_cast<TBlockExpr*>(ast.Get());
    if (!topBlock) return {{}, "expected TBlockExpr at top level"};

    // Find the function body (алг нач...кон) inside the top-level block
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

    if (auto err = NTransform::Pipeline(ast, resolver); !err)
        return {{}, err.error().ToString()};

    NIR::TModule module;
    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver);
    if (auto lowerRes = lowerer.LowerTop(ast); !lowerRes)
        return {{}, lowerRes.error().ToString()};

    auto* mainFun = module.GetEntryPoint();
    if (!mainFun) return {{}, "no main function"};

    std::ostringstream out;
    NRuntime::SetOutputStream(&out);
    std::istringstream stdin_in;
    NRuntime::SetInputStream(&stdin_in);
    NIR::TVMCompiler compiler(module);
    NIR::TInterpreter interp(module, compiler, out, stdin_in);
    try {
        interp.Eval(*mainFun, {}, {});
    } catch (const std::exception& e) {
        NRuntime::SetOutputStream(nullptr);
        return {{}, std::string("runtime error: ") + e.what()};
    }
    NRuntime::SetOutputStream(nullptr);
    return {out.str(), {}};
}

} // namespace

// Write to c.re, then read c.re back and print it.
// Verifies both TFieldAssignExpr (lvalue) and TFieldAccessExpr (rvalue).
TEST(FieldAccess, ReadWriteFirstField) {
    // алг нач
    //   компл c
    //   вещ x
    //   [inject] c.re := 3.5
    //   [inject] x := c.re
    //   вывод x, нс
    // кон
    const std::string src = "использовать Комплексные числа\nалг\nнач\n  компл c\n  вещ x\n  вывод x, нс\nкон\n";
    auto loc = TLocation{};

    std::vector<TExprPtr> injected = {
        MakeIntrusive<TFieldAssignExpr>(
            loc,
            MakeIntrusive<TIdentExpr>(loc, "c"),
            "re",
            MakeIntrusive<TNumberExpr>(loc, 3.5, NQumir::NTest::TestTypes())),
        MakeIntrusive<TAssignExpr>(
            loc,
            "x",
            MakeIntrusive<TFieldAccessExpr>(
                loc,
                MakeIntrusive<TIdentExpr>(loc, "c"),
                "re")),
    };

    // insert before вывод (index 2, after the two var decls)
    auto result = RunWithInjection(src, 2, std::move(injected));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.output.find("3.5"), std::string::npos) << "output: " << result.output;
}

// Write to c.im (second field, non-zero byte offset), then read it back.
TEST(FieldAccess, ReadWriteSecondField) {
    const std::string src = "использовать Комплексные числа\nалг\nнач\n  компл c\n  вещ x\n  вывод x, нс\nкон\n";
    auto loc = TLocation{};

    std::vector<TExprPtr> injected = {
        MakeIntrusive<TFieldAssignExpr>(
            loc,
            MakeIntrusive<TIdentExpr>(loc, "c"),
            "im",
            MakeIntrusive<TNumberExpr>(loc, 7.25, NQumir::NTest::TestTypes())),
        MakeIntrusive<TAssignExpr>(
            loc,
            "x",
            MakeIntrusive<TFieldAccessExpr>(
                loc,
                MakeIntrusive<TIdentExpr>(loc, "c"),
                "im")),
    };

    auto result = RunWithInjection(src, 2, std::move(injected));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.output.find("7.25"), std::string::npos) << "output: " << result.output;
}

// Assign to one field, read both - the other field must stay zero.
TEST(FieldAccess, UnwrittenFieldIsZero) {
    const std::string src = "использовать Комплексные числа\nалг\nнач\n  компл c\n  вещ x\n  вывод x, нс\nкон\n";
    auto loc = TLocation{};

    // Only write re; read im
    std::vector<TExprPtr> injected = {
        MakeIntrusive<TFieldAssignExpr>(
            loc,
            MakeIntrusive<TIdentExpr>(loc, "c"),
            "re",
            MakeIntrusive<TNumberExpr>(loc, 5.0, NQumir::NTest::TestTypes())),
        MakeIntrusive<TAssignExpr>(
            loc,
            "x",
            MakeIntrusive<TFieldAccessExpr>(
                loc,
                MakeIntrusive<TIdentExpr>(loc, "c"),
                "im")),
    };

    auto result = RunWithInjection(src, 2, std::move(injected));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NE(result.output.find("0"), std::string::npos) << "output: " << result.output;
}

// Bad field name must be rejected by the type annotator.
TEST(FieldAccess, UnknownFieldError) {
    const std::string src = "использовать Комплексные числа\nалг\nнач\n  компл c\n  вещ x\n  вывод x, нс\nкон\n";
    auto loc = TLocation{};

    std::vector<TExprPtr> injected = {
        MakeIntrusive<TAssignExpr>(
            loc,
            "x",
            MakeIntrusive<TFieldAccessExpr>(
                loc,
                MakeIntrusive<TIdentExpr>(loc, "c"),
                "nonexistent")),
    };

    auto result = RunWithInjection(src, 2, std::move(injected));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("nonexistent"), std::string::npos) << result.error;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
