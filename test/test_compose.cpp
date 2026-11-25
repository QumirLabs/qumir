#include <gtest/gtest.h>
#include "test_types.h"

#include <qumir/frontend/compose.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <cstdint>
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
            result.push_back(stmt->Kind.ToString());
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

// A module with reachable, unreachable and implicitly called functions.
const char* ReachModule = R"((block
  (pragma language overloads)
  (type Pt <struct (X i64)>)
  (var origin = (call from_global))
  (fun from_global () -> i64 (block (return 0)))
  (fun helper () -> i64 (block (return 1)))
  (fun reached () -> i64 (block (return (+ (call helper) (call shared)))))
  (fun shared () -> i64 (block (return 2)))
  (fun shared ((var x i64)) -> i64 (block (return x)))
  (fun from_aux () -> i64 (block (return 3)))
  (fun from_root () -> i64 (block (return 4)))
  (fun pt_add ((var a <named Pt>) (var b <named Pt>)) -> <named Pt> (attrs (operator "+"))
    (block (return (cast (struct ((X (+ (field a X) (field b X))))) <named Pt>))))
  (fun kept () -> void (attrs used) (block))
  (fun unreached () -> i64 (block (return (+ (call unreached_callee 1) (call undefined_name)))))
  (fun unreached_callee ((var x i64)) -> i64 (block (return x)))))";

const char* ReachMain = R"((block
  (pragma language overloads)
  (use System)
  (use m)
  (fun entry () -> i64
    (block
      (assert (== (call reached) 3))
      (var p <named Pt>)
      (= p (+ (cast (struct ((X 20))) <named Pt>) (cast (struct ((X 22))) <named Pt>)))
      (return (field p X))))
  (fun aux () -> i64 (block (return (call from_aux))))))";

TEST(ComposeTest, DropUnreachableModuleFunctions) {
    // The passes call __ensure by name: a module that defines it keeps it.
    auto rt = Module("rt", "(block (pragma language overloads)"
        " (fun __ensure ((var condition bool) (var message string)) -> void (block)))");
    auto m = Module("m", ReachModule);
    auto main = Parse(ReachMain);
    auto res = Compose({rt.get(), m.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_TRUE(res) << res.error().ToString();

    EXPECT_EQ(DropUnreachableModuleFunctions(res->Ast, {"from_root"}), 2u);
    EXPECT_EQ(Layout(res->Ast), (std::vector<std::string>{
        "Use:System",
        "Use:m",
        "Type:Pt",
        "Var",
        "Fun:entry",
        "Fun:aux",
        "Fun:__ensure",
        "Fun:from_global",
        "Fun:helper",
        "Fun:reached",
        "Fun:shared",
        "Fun:shared",
        "Fun:from_aux",
        "Fun:from_root",
        "Fun:pt_add",
        "Fun:kept",
    }));
}

TEST(ComposeTest, DropKeepsConservativeCases) {
    // Literal suffix, generic, function as a value and a parameter bound stay;
    // a name defined in two modules keeps both definitions.
    auto m1 = Module("m1", R"((block
      (pragma language overloads)
      (fun __sfx ((var x i64)) -> i64 (attrs (literal "z")) (block (return x)))
      (fun gen [T] ((var x T)) -> T (block (return x)))
      (fun as_value () -> i64 (block (return 1)))
      (fun bound () -> i64 (block (return 2)))
      (fun dup () -> i64 (block (return 3)))
      (fun gone () -> i64 (block (return 4)))))");
    auto m2 = Module("m2", R"((block
      (pragma language overloads)
      (fun dup () -> i64 (block (return 5)))))");
    auto main = Parse(R"((block
      (pragma language overloads)
      (use m1)
      (use m2)
      (fun entry () -> i64
        (block
          (var f = as_value)
          (return (+ (call gen (: 1 i64)) (call dup)))))
      (fun with_bound ((var a <array i64 1> [0 (call bound)])) -> void (block))))");
    auto res = Compose({m1.get(), m2.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_TRUE(res) << res.error().ToString();

    EXPECT_EQ(DropUnreachableModuleFunctions(res->Ast, {}), 1u);
    auto layout = Layout(res->Ast);
    auto count = [&](const std::string& name) {
        return std::count(layout.begin(), layout.end(), "Fun:" + name);
    };
    EXPECT_EQ(count("__sfx"), 1);
    EXPECT_EQ(count("gen"), 1);
    EXPECT_EQ(count("as_value"), 1);
    EXPECT_EQ(count("bound"), 1);
    EXPECT_EQ(count("dup"), 2);
    EXPECT_EQ(count("gone"), 0);
}

TEST(ComposeTest, KernelSkipsUnreachableModuleFunctions) {
    // `unreached` calls an undefined name: the kernel compiles because nothing reaches it.
    auto m = Module("m", ReachModule);
    auto main = Parse(ReachMain);
    auto res = Compose({m.get()}, main.Ast, main.Pragmas, "<main>");
    ASSERT_TRUE(res) << res.error().ToString();

    TLLVMRunner runner({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .OptLevel = 2,
    }, nullptr, &NQumir::NTest::TestTypes());
    std::string error;
    auto* entry = reinterpret_cast<int64_t (*)()>(
        runner.CompileKernelAst(std::move(*res), "entry", &error));
    ASSERT_NE(entry, nullptr) << error;
    EXPECT_EQ(entry(), 42);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
