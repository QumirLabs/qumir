#include <gtest/gtest.h>

#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/runner/runner_llvm.h>
#include <qumir/semantics/transform/transform.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace NQumir;
using namespace NQumir::NFrontend;

namespace fs = std::filesystem;

namespace {

NAst::TExprPtr ParseAst(const std::string& source) {
    std::istringstream in(source);
    NAst::NCore::TTokenStream tokens(in);
    NAst::NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    EXPECT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    return parsed ? *parsed : nullptr;
}

std::string CompileBitcode(
    const std::string& source,
    const std::string& targetTriple = {})
{
    auto ast = ParseAst(source);
    NSemantics::TNameResolver resolver;
    resolver.GetOrCreateRootScope()->RootLevel = false;
    if (auto error = resolver.Resolve(ast)) {
        ADD_FAILURE() << error->ToString();
        return {};
    }
    if (auto transformed = NTransform::Pipeline(ast, resolver); !transformed) {
        ADD_FAILURE() << transformed.error().ToString();
        return {};
    }

    NIR::TModule module;
    NIR::TBuilder builder(module);
    NIR::TAstLowerer lowerer(module, builder, resolver);
    if (auto lowered = lowerer.LowerTop(ast); !lowered) {
        ADD_FAILURE() << lowered.error().ToString();
        return {};
    }

    NCodeGen::TLLVMCodeGen codegen({.TargetTriple = targetTriple});
    auto artifacts = codegen.Emit(module);
    std::ostringstream out(std::ios::binary);
    artifacts->GenerateBitcode(out);
    return out.str();
}

class SourceModuleLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        static int counter = 0;
        Dir = fs::temp_directory_path() / fs::path("qumir_loader_test_" + std::to_string(++counter));
        fs::remove_all(Dir);
        fs::create_directories(Dir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(Dir, ec);
    }

    fs::path Write(const std::string& name, const std::string& content) {
        auto path = Dir / (name + ".oz");
        std::ofstream out(path);
        out << content;
        out.close();
        return path;
    }

    bool DependencyFirst(const std::vector<const TSourceModule*>& order) {
        std::vector<std::string> seen;
        for (const auto* m : order) {
            for (const auto& dep : m->SourceDependencies) {
                if (std::find(seen.begin(), seen.end(), dep) == seen.end()) {
                    return false;
                }
            }
            seen.push_back(m->Name);
        }
        return true;
    }

    fs::path Dir;
};

TEST_F(SourceModuleLoaderTest, DefaultVectorAlias) {
    TSourceModuleLoader loader;
    auto module = loader.Load("Vector");
    ASSERT_TRUE(module) << module.error().ToString();
    EXPECT_EQ((*module)->Path.filename(), "vector.oz");
}

TEST_F(SourceModuleLoaderTest, SingleModuleNoDeps) {
    Write("a", "(block (type t i64) (fun foo () (block)) (fun bar () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->Name, "a");
    EXPECT_TRUE((*m)->SourceDependencies.empty());
    EXPECT_EQ((*m)->ExportedFunctions(), (std::vector<std::string>{"foo", "bar"}));
    EXPECT_EQ((*m)->ExportedTypes(), (std::vector<std::string>{"t"}));
}

TEST_F(SourceModuleLoaderTest, LoadAlreadyParsedAst) {
    auto ast = ParseAst("(block (type t i64) (fun foo () (block)))");

    TSourceModuleLoader loader;
    auto m = loader.LoadAst("memory", ast);
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->Ast, ast);
    EXPECT_EQ((*m)->Path, fs::path("<ast:memory>"));
    EXPECT_EQ((*m)->ExportedFunctions(), (std::vector<std::string>{"foo"}));
    EXPECT_EQ((*m)->ExportedTypes(), (std::vector<std::string>{"t"}));
    EXPECT_TRUE(loader.Resolvable("memory"));

    auto loadedByName = loader.Load("memory");
    ASSERT_TRUE(loadedByName) << loadedByName.error().ToString();
    EXPECT_EQ(*loadedByName, *m);
}

TEST_F(SourceModuleLoaderTest, ProgramUseComposesAlreadyParsedAst) {
    auto moduleAst = ParseAst("(block (fun from_memory () -> i64 (block (return (: 42 i64)))))");
    auto mainAst = ParseAst("(block (use memory) (fun <main> () (block (call from_memory))))");

    TSourceModuleLoader loader;
    auto loaded = loader.LoadAst("memory", moduleAst);
    ASSERT_TRUE(loaded) << loaded.error().ToString();

    auto composed = LoadAndCompose(loader, mainAst, {});
    ASSERT_TRUE(composed) << composed.error().ToString();

    auto block = NAst::TMaybeNode<NAst::TBlockExpr>(composed->Ast);
    ASSERT_TRUE(block);
    ASSERT_EQ(block.Cast()->Stmts.size(), 3u);

    auto use = NAst::TMaybeNode<NAst::TUseExpr>(block.Cast()->Stmts[0]);
    ASSERT_TRUE(use);
    EXPECT_EQ(use.Cast()->ModuleName, "memory");
    EXPECT_TRUE(use.Cast()->Resolved);

    auto imported = NAst::TMaybeNode<NAst::TFunDecl>(block.Cast()->Stmts[2]);
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.Cast()->Name, "from_memory");
    EXPECT_EQ(imported.Cast()->Origin, "memory");
}

TEST_F(SourceModuleLoaderTest, ProgramUseLinksBitcodeFromAlreadyParsedAst) {
    auto bitcode = CompileBitcode(
        "(block (fun memory_add ((var x i64)) -> i64"
        " (block (return (+ x (: 2 i64))))))");
    ASSERT_FALSE(bitcode.empty());

    auto moduleAst = ParseAst(
        "(block (fun add ((var x i64)) -> i64"
        " (attrs (extern memory_add)) (block)))");
    auto mainAst = ParseAst(
        "(block (use memory)"
        " (fun entry () -> i64 (block (return (call add (: 40 i64))))))");

    TSourceModuleLoader loader;
    auto loaded = loader.LoadAst("memory", moduleAst, {std::move(bitcode)});
    ASSERT_TRUE(loaded) << loaded.error().ToString();

    auto composed = LoadAndCompose(loader, mainAst, {});
    ASSERT_TRUE(composed) << composed.error().ToString();
    ASSERT_EQ(composed->LlvmBitcode.size(), 1u);

    TLLVMRunner runner({
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .OptLevel = 2,
    });
    std::string error;
    auto* entry = reinterpret_cast<int64_t (*)()>(
        runner.CompileKernelAst(std::move(*composed), "entry", &error));
    ASSERT_NE(entry, nullptr) << error;
    EXPECT_EQ(entry(), 42);
}

TEST_F(SourceModuleLoaderTest, ProgramUseLinksBitcodeForWasmObject) {
    NCodeGen::TLLVMInitializer llvmInit;
    auto bitcode = CompileBitcode(
        "(block (fun memory_add ((var x i64)) -> i64"
        " (block (return (+ x (: 2 i64))))))",
        "wasm64-unknown-unknown");
    ASSERT_FALSE(bitcode.empty());

    auto moduleAst = ParseAst(
        "(block (fun add ((var x i64)) -> i64"
        " (attrs (extern memory_add)) (block)))");
    auto mainAst = ParseAst(
        "(block (use memory)"
        " (fun entry () -> i64 (block (return (call add (: 40 i64))))))");

    TSourceModuleLoader loader;
    auto loaded = loader.LoadAst("memory", moduleAst, {std::move(bitcode)});
    ASSERT_TRUE(loaded) << loaded.error().ToString();

    auto composed = LoadAndCompose(loader, mainAst, {});
    ASSERT_TRUE(composed) << composed.error().ToString();

    TLLVMRunner runner({
        .CoreInput = true,
        .ResolveCoreInput = true,
        .OptLevel = 2,
        .TargetTriple = "wasm64-unknown-unknown",
    });
    std::string error;
    auto object = runner.CompileKernelAstToObject(
        std::move(*composed), {"entry"}, &error);
    ASSERT_TRUE(object) << error;
    ASSERT_GE(object->size(), 4u);
    EXPECT_EQ(object->substr(0, 4), std::string("\0asm", 4));
}

TEST_F(SourceModuleLoaderTest, ParsedAstUsesRegularSourceDependency) {
    Write("dep", "(block (fun dependency () (block)))");
    auto ast = ParseAst("(block (use dep) (fun from_memory () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);
    auto m = loader.LoadAst("memory", std::move(ast));
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->SourceDependencies, (std::vector<std::string>{"dep"}));

    auto order = loader.TopologicalOrder();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0]->Name, "dep");
    EXPECT_EQ(order[1]->Name, "memory");
}

TEST_F(SourceModuleLoaderTest, AliasFromModaliasFile) {
    Write("local_complex", "(block (fun foo () -> i64 (block (return (: 7 i64)))))");
    std::ofstream(Dir / "modalias") << "Тестовый модуль = local_complex\n";

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    EXPECT_TRUE(loader.Resolvable("Тестовый модуль"));
    auto m = loader.Load("Тестовый модуль");
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->ExportedFunctions(), (std::vector<std::string>{"foo"}));
}

TEST_F(SourceModuleLoaderTest, TransitiveChain) {
    Write("a", "(block (use b) (fun fa () (block)))");
    Write("b", "(block (use c) (fun fb () (block)))");
    Write("c", "(block (fun fc () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_TRUE(m) << m.error().ToString();

    auto order = loader.TopologicalOrder();
    EXPECT_EQ(order.size(), 3u);
    EXPECT_TRUE(DependencyFirst(order));
    EXPECT_EQ(order.back()->Name, "a");
}

TEST_F(SourceModuleLoaderTest, DiamondDependency) {
    Write("a", "(block (use b) (use c) (fun fa () (block)))");
    Write("b", "(block (use d) (fun fb () (block)))");
    Write("c", "(block (use d) (fun fc () (block)))");
    Write("d", "(block (fun fd () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_TRUE(m) << m.error().ToString();

    auto order = loader.TopologicalOrder();
    EXPECT_EQ(order.size(), 4u); // d loaded once
    EXPECT_TRUE(DependencyFirst(order));
    EXPECT_EQ(order.back()->Name, "a");
}

TEST_F(SourceModuleLoaderTest, DirectCycle) {
    Write("a", "(block (use a) (fun fa () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_FALSE(m);
    EXPECT_NE(m.error().ToString().find("цикл"), std::string::npos);
}

TEST_F(SourceModuleLoaderTest, IndirectCycle) {
    Write("a", "(block (use b) (fun fa () (block)))");
    Write("b", "(block (use a) (fun fb () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_FALSE(m);
    auto msg = m.error().ToString();
    EXPECT_NE(msg.find("цикл"), std::string::npos);
    EXPECT_NE(msg.find("a -> b -> a"), std::string::npos) << msg;
}

TEST_F(SourceModuleLoaderTest, ModuleNotFound) {
    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("missing");
    ASSERT_FALSE(m);
    auto msg = m.error().ToString();
    EXPECT_NE(msg.find("не найден"), std::string::npos);
    EXPECT_NE(msg.find(Dir.string()), std::string::npos) << msg;
}

TEST_F(SourceModuleLoaderTest, DuplicateExplicitRegistration) {
    auto first = Write("a", "(block (fun fa () (block)))");
    auto other = Dir / "sub";
    fs::create_directories(other);
    std::ofstream out(other / "a.oz");
    out << "(block (fun fa () (block)))";
    out.close();

    TSourceModuleLoader loader;
    ASSERT_TRUE(loader.RegisterSourceModule(first));
    auto res = loader.RegisterSourceModule(other / "a.oz");
    ASSERT_FALSE(res);
    EXPECT_NE(res.error().ToString().find("уже зарегистрирован"), std::string::npos);
}

TEST_F(SourceModuleLoaderTest, IdempotentLoad) {
    Write("a", "(block (fun fa () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto first = loader.Load("a");
    auto second = loader.Load("a");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(loader.TopologicalOrder().size(), 1u);
}

TEST_F(SourceModuleLoaderTest, ForbidEntryPoint) {
    Write("a", "(block (fun <main> () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_FALSE(m);
    auto msg = m.error().ToString();
    EXPECT_NE(msg.find("<main>"), std::string::npos);
    EXPECT_NE(msg.find("a.oz"), std::string::npos) << msg; // diagnostic names the file
}

TEST_F(SourceModuleLoaderTest, GlobalExported) {
    Write("a", "(block (var g i64) (fun fa () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->ExportedGlobals(), (std::vector<std::string>{"g"}));
}

TEST_F(SourceModuleLoaderTest, ForbidExecutableTopLevel) {
    Write("a", "(block (output \"hi\") (fun fa () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_FALSE(m);
    EXPECT_NE(m.error().ToString().find("недопустимое верхнеуровневое"), std::string::npos);
}

TEST_F(SourceModuleLoaderTest, ExternalDependencyRecordedNotFollowed) {
    Write("a", "(block (use System) (fun fa () (block)))");

    TSourceModuleLoader loader;
    loader.AddSearchPath(Dir);

    auto m = loader.Load("a");
    ASSERT_TRUE(m) << m.error().ToString();
    EXPECT_EQ((*m)->Dependencies, (std::vector<std::string>{"System"}));
    EXPECT_TRUE((*m)->SourceDependencies.empty());
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
