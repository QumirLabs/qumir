#include <gtest/gtest.h>

#include <qumir/ir/builder.h>
#include <qumir/ir/eval.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/modules/system/system.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/parser.h>
#include <qumir/runtime/io.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/transform/transform.h>

#include <pthread.h>

#include <expected>
#include <sstream>
#include <string>

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

// Lowering recurses once per AST level. A frame that holds the locals of every node
// kind (tens of KB in a Debug or ASan build) per level would overflow this stack.
constexpr size_t LoweringStack = 2 << 20;
constexpr int Depth = 50;

template <typename TFunc>
void RunOnStack(size_t bytes, TFunc& func) {
    pthread_attr_t attr;
    ASSERT_EQ(pthread_attr_init(&attr), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attr, bytes), 0);
    pthread_t thread;
    auto start = [](void* arg) -> void* {
        (*static_cast<TFunc*>(arg))();
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attr, start, &func), 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
}

// if 1 then (... + 1) else 0, `depth` times around 0: evaluates to `depth`.
TExprPtr NestedIf(int depth) {
    TExprPtr expr = MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0});
    for (int i = 0; i < depth; ++i) {
        auto sum = MakeIntrusive<TBinaryExpr>(TLocation{}, TOperator("+"),
            std::move(expr), MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}));
        expr = MakeIntrusive<TIfExpr>(TLocation{},
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{1}),
            std::move(sum),
            MakeIntrusive<TNumberExpr>(TLocation{}, int64_t{0}));
    }
    return expr;
}

} // namespace

TEST(LowerDepth, DeepExpressionFitsSmallStack) {
    NSemantics::TNameResolver resolver;
    auto sysMod = std::make_shared<NRegistry::SystemModule>();
    resolver.RegisterModule(sysMod.get());
    resolver.ImportModule(sysMod->Name());

    std::istringstream in("алг\nнач\n  цел x\n  вывод x, нс\nкон\n");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.parse(tokens, &resolver);
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    auto ast = std::move(parsed.value());
    auto* top = dynamic_cast<TBlockExpr*>(ast.Get());
    ASSERT_NE(top, nullptr);
    auto* fun = dynamic_cast<TFunDecl*>(top->Stmts.front().Get());
    ASSERT_NE(fun, nullptr);
    auto& body = fun->Body->Stmts;
    body.insert(body.begin() + 1, MakeIntrusive<TAssignExpr>(TLocation{}, "x", NestedIf(Depth)));

    auto scope = resolver.GetOrCreateRootScope();
    scope->AllowsRedeclare = true;
    scope->RootLevel = false;
    auto pipeline = NTransform::Pipeline(ast, resolver);
    ASSERT_TRUE(pipeline) << pipeline.error().ToString();

    NIR::TModule module;
    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver);
    std::expected<std::monostate, TError> lowered = std::monostate{};
    auto lower = [&] {
        lowered = lowerer.LowerTop(ast);
    };
    RunOnStack(LoweringStack, lower);
    ASSERT_TRUE(lowered) << lowered.error().ToString();

    auto* mainFun = module.GetEntryPoint();
    ASSERT_NE(mainFun, nullptr);
    std::ostringstream out;
    std::istringstream input;
    NRuntime::SetOutputStream(&out);
    NRuntime::SetInputStream(&input);
    NIR::TVMCompiler compiler(module);
    NIR::TInterpreter interpreter(module, compiler, out, input);
    interpreter.Eval(*mainFun, {}, {});
    NRuntime::SetOutputStream(nullptr);
    EXPECT_EQ(out.str(), std::to_string(Depth) + "\n");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
