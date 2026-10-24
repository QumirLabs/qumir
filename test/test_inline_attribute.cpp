#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/ir/builder.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/runner/runner_llvm.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>

using namespace NQumir;

namespace {

auto Parse(const std::string& source) {
    std::istringstream input(source);
    NAst::NCore::TTokenStream tokens(input);
    NAst::NCore::TParser parser;
    return parser.Parse(tokens);
}

void* Compile(
    const std::string& source,
    const std::string& entryName,
    TLLVMRunner& runner,
    std::string& llvmIr)
{
    auto parsed = Parse(source);
    if (!parsed) {
        ADD_FAILURE() << parsed.error().ToString();
        return nullptr;
    }
    std::string error;
    testing::internal::CaptureStderr();
    auto* entry = runner.CompileKernelAst(*parsed, entryName, &error);
    llvmIr = testing::internal::GetCapturedStderr();
    EXPECT_NE(entry, nullptr) << error << '\n' << llvmIr;
    return entry;
}

void CheckDefinition(const std::string& genericParameters) {
    const std::string source =
        "(block (fun twice " + genericParameters + R"(
          ((var x i64)) -> i64 (attrs inline)
          (block (return (+ x x))))
          (fun kernel ((var x i64)) -> i64
            (block (return (call twice x))))))";
    for (bool nativeCode : {false, true}) {
        for (int optLevel : {0, 3}) {
            SCOPED_TRACE(testing::Message() << "native=" << nativeCode << " opt=" << optLevel);
            TLLVMRunner runner({
                .PrintLlvm = true,
                .NativeCode = nativeCode,
                .CoreInput = true,
                .ResolveCoreInput = true,
                .AllowOverloads = true,
                .OptLevel = optLevel,
            });
            std::string llvmIr;
            auto* entry = Compile(source, "kernel", runner, llvmIr);
            ASSERT_NE(entry, nullptr);
            auto* kernel = reinterpret_cast<int64_t (*)(int64_t)>(entry);
            EXPECT_EQ(kernel(21), 42);
            EXPECT_EQ(kernel(-17), -34);
            if (optLevel == 0) {
                EXPECT_NE(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
            } else {
                const auto begin = llvmIr.rfind("@kernel(");
                ASSERT_NE(begin, std::string::npos) << llvmIr;
                const auto end = llvmIr.find("\n}", begin);
                ASSERT_NE(end, std::string::npos) << llvmIr;
                EXPECT_EQ(llvmIr.substr(begin, end - begin).find("twice"), std::string::npos)
                    << llvmIr;
            }
        }
    }
}

std::string EmitVoidFunction(bool debugPoints, bool coroutine) {
    NIR::TModule module;
    module.DebugOptions.EmitDebugPoints = debugPoints;
    NIR::TBuilder builder(module);
    builder.NewFunction("helper", {}, 1);
    const int voidType = module.Types.I(NIR::EKind::Void);
    builder.SetReturnType(coroutine ? module.Types.Ptr(voidType) : voidType);
    auto& function = module.Functions.back();
    function.Inline = true;
    function.IsCoroutine = coroutine;
    function.CoroutineResultTypeId = voidType;
    using namespace NIR::NLiterals;
    builder.Emit0("ret"_op, {});
    NCodeGen::TLLVMCodeGen codegen;
    auto artifacts = codegen.Emit(module, 0);
    std::ostringstream out;
    artifacts->PrintModule(out);
    return out.str();
}

} // namespace

TEST(InlineAttribute, PrintingAndDeepCloningPreserveAttributes) {
    for (const std::string attrs : {"inline", "cacheable inline"}) {
        const std::string source =
            "(block (fun twice [T] ((var x T)) -> T (attrs " + attrs +
            ") (block (return (+ x x)))))";
        auto parsed = Parse(source);
        ASSERT_TRUE(parsed) << parsed.error().ToString();
        auto cloned = NAst::DeepCloneExpr(*parsed);
        std::ostringstream out;
        NAst::NCore::TPrinter printer(out, {});
        printer.PrintExpr(cloned);
        auto reparsed = Parse(out.str());
        ASSERT_TRUE(reparsed) << reparsed.error().ToString();
        auto block = NAst::TMaybeNode<NAst::TBlockExpr>(*reparsed).Cast();
        ASSERT_NE(block, nullptr);
        ASSERT_EQ(block->Stmts.size(), 1u);
        auto function = NAst::TMaybeNode<NAst::TFunDecl>(block->Stmts.front()).Cast();
        ASSERT_NE(function, nullptr);
        EXPECT_TRUE(function->Inline);
        EXPECT_EQ(function->Cacheable, attrs == "cacheable inline");
    }
}

TEST(InlineAttribute, NoInlineKeepsCallsOutOfLine) {
    const std::string source = R"((block
      (fun twice ((var x i64)) -> i64 (attrs noinline)
        (block (return (+ x x))))
      (fun kernel ((var x i64)) -> i64
        (block (return (call twice x))))))";
    auto parsed = Parse(source);
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    std::ostringstream out;
    NAst::NCore::TPrinter printer(out, {});
    printer.PrintExpr(NAst::DeepCloneExpr(*parsed));
    auto reparsed = Parse(out.str());
    ASSERT_TRUE(reparsed) << reparsed.error().ToString();
    auto block = NAst::TMaybeNode<NAst::TBlockExpr>(*reparsed).Cast();
    ASSERT_NE(block, nullptr);
    auto function = NAst::TMaybeNode<NAst::TFunDecl>(block->Stmts.front()).Cast();
    ASSERT_NE(function, nullptr);
    EXPECT_TRUE(function->NoInline);
    EXPECT_FALSE(function->Inline);

    TLLVMRunner runner({
        .PrintLlvm = true,
        .NativeCode = true,
        .CoreInput = true,
        .ResolveCoreInput = true,
        .AllowOverloads = true,
        .OptLevel = 3,
    });
    std::string llvmIr;
    auto* entry = Compile(source, "kernel", runner, llvmIr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(reinterpret_cast<int64_t (*)(int64_t)>(entry)(21), 42);
    EXPECT_NE(llvmIr.find("noinline"), std::string::npos) << llvmIr;
    const auto begin = llvmIr.rfind("@kernel(");
    ASSERT_NE(begin, std::string::npos) << llvmIr;
    const auto end = llvmIr.find("\n}", begin);
    ASSERT_NE(end, std::string::npos) << llvmIr;
    EXPECT_NE(llvmIr.substr(begin, end - begin).find("twice"), std::string::npos) << llvmIr;
}

TEST(InlineAttribute, OrdinaryDefinitionReachesLlvmAndIsInlined) {
    CheckDefinition("");
}

TEST(InlineAttribute, GenericSpecializationReachesLlvmAndIsInlined) {
    const std::string source = R"((block
      (fun twice [T] ((var x T)) -> T (attrs inline)
        (block (return (+ x x))))
      (fun kernel ((var x i64)) -> i64
        (block (return (call twice x))))))";
    for (int optLevel : {0, 3}) {
        SCOPED_TRACE(optLevel);
        TLLVMRunner runner({
            .PrintLlvm = true,
            .NativeCode = true,
            .CoreInput = true,
            .ResolveCoreInput = true,
            .AllowOverloads = true,
            .OptLevel = optLevel,
        });
        std::string llvmIr;
        auto* entry = Compile(source, "kernel", runner, llvmIr);
        ASSERT_NE(entry, nullptr);
        auto* kernel = reinterpret_cast<int64_t (*)(int64_t)>(entry);
        EXPECT_EQ(kernel(21), 42);
        EXPECT_EQ(kernel(-17), -34);
        if (optLevel == 0) {
            EXPECT_NE(llvmIr.find("__generic_twice"), std::string::npos) << llvmIr;
            EXPECT_NE(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
        } else {
            const auto begin = llvmIr.rfind("@kernel(");
            ASSERT_NE(begin, std::string::npos);
            const auto end = llvmIr.find("\n}", begin);
            ASSERT_NE(end, std::string::npos);
            EXPECT_EQ(llvmIr.substr(begin, end - begin).find("__generic_twice"), std::string::npos)
                << llvmIr;
        }
    }
}

TEST(InlineAttribute, UnannotatedDefinitionKeepsDefaultPolicy) {
    const std::string source = R"((block
      (fun kernel () -> i64 (block (return 42)))))";
    TLLVMRunner runner({.PrintLlvm = true, .CoreInput = true, .OptLevel = 0});
    std::string llvmIr;
    auto* entry = Compile(source, "kernel", runner, llvmIr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(reinterpret_cast<int64_t (*)()>(entry)(), 42);
    EXPECT_EQ(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
}

TEST(InlineAttribute, ExternalDeclarationDoesNotReceiveAlwaysInline) {
    const std::string source = R"((block
      (fun absolute ((var x i64)) -> i64 (attrs (extern llabs) inline)
        (block))
      (fun kernel ((var x i64)) -> i64
        (block (return (call absolute x))))))";
    TLLVMRunner runner({.PrintLlvm = true, .CoreInput = true, .OptLevel = 0});
    std::string llvmIr;
    auto* entry = Compile(source, "kernel", runner, llvmIr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(reinterpret_cast<int64_t (*)(int64_t)>(entry)(-21), 21);
    EXPECT_EQ(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
}

TEST(InlineAttribute, ReplPreservesNoInlinePolicy) {
    const std::string source = R"((block
      (fun __repl_inline () -> i64 (attrs inline)
        (block (return 42)))))";
    TLLVMRunner runner({.PrintLlvm = true, .CoreInput = true, .OptLevel = 3});
    std::string llvmIr;
    auto* entry = Compile(source, "__repl_inline", runner, llvmIr);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(reinterpret_cast<int64_t (*)()>(entry)(), 42);
    EXPECT_NE(llvmIr.find("noinline"), std::string::npos) << llvmIr;
    EXPECT_EQ(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
}

TEST(InlineAttribute, DebugPointsPreserveNoInlinePolicy) {
    const auto llvmIr = EmitVoidFunction(true, false);
    EXPECT_NE(llvmIr.find("noinline"), std::string::npos) << llvmIr;
    EXPECT_EQ(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
}

TEST(InlineAttribute, CoroutineDoesNotReceiveAlwaysInline) {
    const auto llvmIr = EmitVoidFunction(false, true);
    EXPECT_EQ(llvmIr.find("alwaysinline"), std::string::npos) << llvmIr;
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
