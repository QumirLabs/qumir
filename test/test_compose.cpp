#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/frontend/compose.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace NQumir;
using namespace NQumir::NAst;
using namespace NQumir::NFrontend;

namespace {

struct Parsed {
    TExprPtr Ast;
    std::vector<TPragma> Pragmas;
};

Parsed Parse(const std::string& src) {
    std::istringstream in(src);
    NCore::TTokenStream ts(in);
    NCore::TParser parser(NQumir::NTest::TestTypes());
    auto parsed = parser.Parse(ts);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    return {*parsed, parser.Pragmas};
}

std::shared_ptr<TSourceModule> Module(const std::string& name, const std::string& src) {
    auto parsed = Parse(src);
    auto m = std::make_shared<TSourceModule>();
    m->Name = name;
    m->Path = name + ".oz";
    m->Ast = parsed.Ast;
    m->Pragmas = parsed.Pragmas;
    return m;
}

// "Use:name" / "Fun:name" / "Type:name" labels for a composed block.
std::vector<std::string> Layout(const TExprPtr& root) {
    std::vector<std::string> result;
    for (const auto& stmt : TMaybeNode<TBlockExpr>(root).Cast()->Stmts) {
        if (auto use = TMaybeNode<TUseExpr>(stmt)) {
            result.push_back("Use:" + use.Cast()->ModuleName);
        } else if (auto type = TMaybeNode<TTypeDeclStmt>(stmt)) {
            result.push_back("Type:" + TMaybeType<TNamedType>(type.Cast()->Type).Cast()->Name);
        } else if (auto fun = TMaybeNode<TFunDecl>(stmt)) {
            result.push_back("Fun:" + fun.Cast()->Name);
        } else {
            result.push_back(std::string(stmt->NodeName()));
        }
    }
    return result;
}

TEST(ComposeTest, OrderingAndSourceUseResolved) {
    auto m = Module("m", "(block (type t i64) (fun helper () (block)))");
    auto main = Parse("(block (use m) (use System) (var g i64) (fun aux () (block)) (fun <main> () (block)))");

    auto res = Compose({m.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_TRUE(res) << res.error().ToString();

    EXPECT_EQ(Layout(res->Ast), (std::vector<std::string>{
        "Use:m",        // source use kept for printing, but marked resolved
        "Use:System",   // runtime use kept unresolved
        "Type:t",       // module type before main
        "Var",          // main global before functions
        "Fun:aux",      // main functions first (Kumir entry must stay first)
        "Fun:<main>",
        "Fun:helper",   // module functions after main
    }));
    auto block = TMaybeNode<TBlockExpr>(res->Ast).Cast();
    ASSERT_TRUE(TMaybeNode<TUseExpr>(block->Stmts[0]).Cast()->Resolved);
    EXPECT_EQ(NCore::PrintAst(res->Ast), R"((block
  (use "m")
  (use "System")
  (var g i64)
  (fun aux ()
    (block))
  (fun <main> ()
    (block))))");
}

TEST(ComposeTest, ConflictFunctionRedeclared) {
    auto a = Module("a", "(block (fun foo () (block)))");
    auto b = Module("b", "(block (fun foo () (block)))");
    auto main = Parse("(block (fun <main> () (block)))");

    auto res = Compose({a.get(), b.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_FALSE(res);
    auto msg = res.error().ToString();
    EXPECT_NE(msg.find("foo"), std::string::npos);
    EXPECT_NE(msg.find("a.oz"), std::string::npos) << msg;
    EXPECT_NE(msg.find("b.oz"), std::string::npos) << msg;
}

TEST(ComposeTest, ConflictTypeVsFunction) {
    auto m = Module("m", "(block (type x i64))");
    auto main = Parse("(block (fun x () (block)) (fun <main> () (block)))");

    auto res = Compose({m.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("x"), std::string::npos);
}

TEST(ComposeTest, OverloadsAllowDuplicateFunctions) {
    auto a = Module("a", "(block (pragma language overloads) (fun foo () (block)))");
    auto main = Parse("(block (pragma language overloads) (fun foo () (block)) (fun <main> () (block)))");

    auto res = Compose({a.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_TRUE(res) << res.error().ToString();
    EXPECT_EQ(res->Pragmas.size(), 1u); // identical pragmas merged
}

TEST(ComposeTest, IncompatiblePragmas) {
    auto a = Module("a", "(block (pragma language overloads) (fun foo () (block)))");
    auto main = Parse("(block (pragma language strict) (fun <main> () (block)))");

    auto res = Compose({a.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("несовместимые прагмы"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
