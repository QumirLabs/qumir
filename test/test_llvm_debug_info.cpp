#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/parser.h>
#include <qumir/modules/system/system.h>
#include <qumir/semantics/transform/transform.h>

#include <gtest/gtest.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>

#include <sstream>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

namespace {

void BuildModule(TModule& module, bool withDebugInfo = true) {
    module.DebugOptions.EmitDebugInfo = withDebugInfo;
    TBuilder builder(module);
    auto integer = std::make_shared<NAst::TIntegerType>();
    const int i64 = FromAstType(integer, module.Types);
    const int f64 = module.Types.I(EKind::F64);
    builder.NewFunction("mangled_divide", {TLocal{0}, TLocal{2}}, 1);
    builder.SetReturnType(f64);
    builder.SetType(TLocal{0}, i64, {});
    builder.SetType(TLocal{1}, i64, {});
    builder.SetType(TLocal{2}, i64, {});
    if (withDebugInfo) {
        auto& function = module.Functions.front();
        function.DebugInfo = TFunctionDebugInfo{
            "divide", {2, 1, 1}, 10,
            std::make_shared<NAst::TFunctionType>(
                std::vector<NAst::TTypePtr>{integer, integer}, std::make_shared<NAst::TFloatType>())};
        function.LocalDebugInfo = {
            {"x", {2, 1, 1}, 10, integer},
            {"x", {4, 1, 1}, 11, integer},
            {"y", {2, 1, 1}, 10, integer},
        };
        builder.SetScopeDebugInfo(10, -1);
        builder.SetScopeDebugInfo(11, 10);
    }
    const TInstrDebugInfo location = withDebugInfo
        ? TInstrDebugInfo{{7, 1, 3}, 11}
        : TInstrDebugInfo{};
    auto lhs = builder.Emit1("load"_op, {TLocal{0}}, location);
    builder.SetType(lhs, i64);
    auto rhs = builder.Emit1("load"_op, {TLocal{2}}, location);
    builder.SetType(rhs, i64);
    auto result = builder.Emit1("/"_op, {lhs, rhs}, location);
    builder.SetType(result, f64);
    builder.Emit0("call"_op, {TImm{2}});
    builder.Emit0("ret"_op, {result}, location);
    builder.NewFunction("raw", {}, 2);
    builder.SetReturnType(module.Types.I(EKind::Void));
    builder.Emit0("ret"_op, {});
}

std::unique_ptr<llvm::Module> Emit(
    TModule& module,
    llvm::LLVMContext& context,
    int optLevel = 0,
    const NCodeGen::TLLVMCodeGenOptions& options = {})
{
    NCodeGen::TLLVMCodeGen codegen(options);
    auto artifacts = codegen.Emit(module, optLevel);
    std::ostringstream bitcode;
    artifacts->GenerateBitcode(bitcode);
    const auto data = bitcode.str();
    auto parsed = llvm::parseBitcodeFile(llvm::MemoryBufferRef(data, "debug-info-test"), context);
    if (!parsed) {
        ADD_FAILURE() << llvm::toString(parsed.takeError());
        return nullptr;
    }
    EXPECT_FALSE(llvm::verifyModule(**parsed, &llvm::errs()));
    (*parsed)->convertFromNewDbgValues();
    return std::move(*parsed);
}

} // namespace

TEST(LLVMDebugInfo, FunctionsScopesLocalsAndLocations) {
    TModule source;
    BuildModule(source);
    TBuilder builder(source);
    builder.NewFunction("coro", {}, 3);
    const int voidType = source.Types.I(EKind::Void);
    builder.SetReturnType(source.Types.Ptr(voidType));
    auto& coroutine = source.Functions.back();
    coroutine.IsCoroutine = true;
    coroutine.CoroutineResultTypeId = voidType;
    coroutine.DebugInfo = TFunctionDebugInfo{"coro", {20, 1, 1}, 12, nullptr};
    builder.Emit0("ret"_op, {});
    llvm::LLVMContext context;
    auto module = Emit(source, context);
    ASSERT_TRUE(module);
    auto* function = module->getFunction("mangled_divide");
    EXPECT_TRUE(function->hasFnAttribute(llvm::Attribute::NoInline));
    EXPECT_TRUE(function->hasFnAttribute(llvm::Attribute::OptimizeNone));
    auto* subprogram = function->getSubprogram();
    ASSERT_NE(subprogram, nullptr);
    EXPECT_EQ(subprogram->getName(), "divide");
    EXPECT_EQ(subprogram->getLinkageName(), "mangled_divide");
    EXPECT_EQ(subprogram->getLine(), 2);
    EXPECT_EQ(subprogram->getFile()->getFilename(), "<stdin>");
    auto* signature = subprogram->getType();
    ASSERT_EQ(signature->getTypeArray().size(), 3);
    EXPECT_EQ(signature->getTypeArray()[0]->getSizeInBits(), 64);
    unsigned declarations = 0;
    unsigned divisionInstructions = 0;
    for (auto& block : *function) {
        for (auto& instruction : block) {
            if (auto* declare = llvm::dyn_cast<llvm::DbgDeclareInst>(&instruction)) {
                ++declarations;
                const auto* variable = declare->getVariable();
                EXPECT_TRUE(llvm::isa<llvm::AllocaInst>(declare->getAddress()));
                if (variable->isParameter()) {
                    EXPECT_EQ(variable->getScope(), subprogram);
                    EXPECT_EQ(variable->getArg(), variable->getName() == "x" ? 1 : 2);
                } else {
                    auto* scope = llvm::dyn_cast<llvm::DILexicalBlock>(variable->getScope());
                    ASSERT_NE(scope, nullptr);
                    EXPECT_EQ(scope->getScope(), subprogram);
                    EXPECT_EQ(variable->getName(), "x");
                }
            } else if (llvm::isa<llvm::SIToFPInst>(instruction) || instruction.getOpcode() == llvm::Instruction::FDiv) {
                ++divisionInstructions;
                ASSERT_TRUE(instruction.getDebugLoc());
                EXPECT_EQ(instruction.getDebugLoc().getLine(), 7);
                EXPECT_EQ(instruction.getDebugLoc().getCol(), 3);
            } else if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
                if (call->getCalledFunction()->getName() == "raw") {
                    ASSERT_TRUE(call->getDebugLoc());
                    EXPECT_EQ(call->getDebugLoc().getLine(), 0);
                }
            } else if (llvm::isa<llvm::AllocaInst>(instruction) || llvm::isa<llvm::StoreInst>(instruction)) {
                EXPECT_FALSE(instruction.getDebugLoc());
            }
        }
    }
    EXPECT_EQ(declarations, 3);
    EXPECT_EQ(divisionInstructions, 3);
    auto* raw = module->getFunction("raw");
    EXPECT_EQ(raw->getSubprogram(), nullptr);
    EXPECT_FALSE(raw->hasFnAttribute(llvm::Attribute::OptimizeNone));
    EXPECT_FALSE(raw->getEntryBlock().getTerminator()->getDebugLoc());
    for (const auto& f : *module) {
        if (f.getName().starts_with("coro")) {
            EXPECT_EQ(f.getSubprogram(), nullptr);
            for (const auto& block : f) {
                for (const auto& instruction : block) {
                    EXPECT_FALSE(instruction.getDebugLoc());
                }
            }
        }
    }
}

TEST(LLVMDebugInfo, PrimitiveTypesReferencesAndOpaqueFallback) {
    using namespace NAst;
    std::vector<TTypePtr> astTypes;
    for (auto kind : {TIntegerType::I8, TIntegerType::I16, TIntegerType::I32,
        TIntegerType::I64, TIntegerType::I128, TIntegerType::U8, TIntegerType::U16,
        TIntegerType::U32, TIntegerType::U64, TIntegerType::U128})
    {
        astTypes.push_back(std::make_shared<TIntegerType>(kind));
    }
    auto integer = std::make_shared<TIntegerType>();
    auto structure = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{"field", integer}});
    astTypes.insert(astTypes.end(), {
        std::make_shared<TFloatType>(), std::make_shared<TBoolType>(),
        std::make_shared<TSymbolType>(), std::make_shared<TStringType>(),
        std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::I8)),
        std::make_shared<TReferenceType>(integer), std::make_shared<TArrayType>(integer, 1),
        structure, std::make_shared<TNamedType>("alias", integer),
    });
    for (unsigned pointerBits : {32, 64}) {
        TModule source;
        source.DebugOptions.EmitDebugInfo = true;
        source.SourceFilePath = "build/types.oz";
        source.Types.SetPointerSize(pointerBits / 8);
        TBuilder builder(source);
        builder.NewFunction("types", {}, 1);
        builder.SetReturnType(source.Types.I(EKind::Void));
        source.Functions[0].DebugInfo = TFunctionDebugInfo{"types", {1, 1, 1}, 0, nullptr};
        for (size_t i = 0; i < astTypes.size(); ++i) {
            builder.AllocLocal(FromAstType(astTypes[i], source.Types),
                {"v" + std::to_string(i), {2, 1, 1}, 0, astTypes[i]});
        }
        builder.Emit0("ret"_op, {}, {{3, 1, 1}, 0});
        llvm::LLVMContext context;
        NCodeGen::TLLVMCodeGenOptions options;
        if (pointerBits == 32) {
            options.TargetTriple = "wasm32-unknown-unknown";
        }
        auto module = Emit(source, context, 0, options);
        ASSERT_TRUE(module);
        auto* function = module->getFunction("types");
        EXPECT_EQ(function->getSubprogram()->getFile()->getFilename(), "types.oz");
        size_t index = 0;
        for (auto& instruction : function->getEntryBlock()) {
            auto* declare = llvm::dyn_cast<llvm::DbgDeclareInst>(&instruction);
            if (!declare) {
                continue;
            }
            auto* type = declare->getVariable()->getType();
            if (index < 10) {
                auto integerType = TMaybeType<TIntegerType>(astTypes[index]).Cast();
                EXPECT_EQ(type->getSizeInBits(), integerType->BitWidth());
                EXPECT_EQ(llvm::cast<llvm::DIBasicType>(type)->getEncoding(), integerType->IsSigned()
                    ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned);
            } else if (index == 10) {
                EXPECT_EQ(type->getSizeInBits(), 64);
                EXPECT_EQ(llvm::cast<llvm::DIBasicType>(type)->getEncoding(), llvm::dwarf::DW_ATE_float);
            } else if (index == 11 || index == 12) {
                EXPECT_EQ(type->getSizeInBits(), index == 11 ? 8 : 32);
                EXPECT_EQ(llvm::cast<llvm::DIBasicType>(type)->getEncoding(), index == 11
                    ? llvm::dwarf::DW_ATE_boolean : llvm::dwarf::DW_ATE_UTF);
            } else if (index >= 13 && index <= 16) {
                auto* pointer = llvm::cast<llvm::DIDerivedType>(type);
                EXPECT_EQ(pointer->getSizeInBits(), pointerBits);
                EXPECT_EQ(pointer->getTag(), index == 15
                    ? llvm::dwarf::DW_TAG_reference_type : llvm::dwarf::DW_TAG_pointer_type);
                if (index == 13) {
                    EXPECT_EQ(pointer->getBaseType()->getSizeInBits(), 8);
                    EXPECT_EQ(pointer->getBaseType()->getName(), "char");
                } else if (index == 16) {
                    EXPECT_EQ(pointer->getBaseType()->getTag(), llvm::dwarf::DW_TAG_unspecified_type);
                }
            } else if (index == 17) {
                EXPECT_EQ(type->getTag(), llvm::dwarf::DW_TAG_unspecified_type);
            } else {
                EXPECT_EQ(type->getSizeInBits(), 64);
                EXPECT_EQ(type->getTag(), llvm::dwarf::DW_TAG_base_type);
            }
            ++index;
        }
        EXPECT_EQ(index, astTypes.size());
    }
}

TEST(LLVMDebugInfo, DebugPointsAndNativeInfoAreIndependent) {
    for (bool info : {false, true}) {
        for (bool points : {false, true}) {
            std::istringstream input(
                "алг цел main\nнач\n  знач := add(21, 23)\nкон\n"
                "алг цел add(цел x, цел y)\nнач\n  лог positive\n  positive := (x > 0) и (y > 0)\n  знач := x + y\nкон\n");
            NSemantics::TNameResolver resolver;
            NRegistry::SystemModule system;
            resolver.RegisterModule(&system);
            ASSERT_TRUE(resolver.ImportModule(system.Name()));
            NAst::TTokenStream tokens(input);
            NAst::TParser parser;
            auto parsed = parser.parse(tokens, &resolver);
            ASSERT_TRUE(parsed);
            auto ast = *parsed;
            ASSERT_TRUE(NTransform::Pipeline(ast, resolver));
            TModule source;
            TBuilder builder(source);
            TAstLowerer lowerer(source, builder, resolver, {info, points});
            ASSERT_TRUE(lowerer.LowerTop(ast));
            EXPECT_EQ(source.DebugPoints.empty(), !points);
            EXPECT_EQ(source.Functions[0].DebugInfo.has_value(), info || points);
            for (const auto& f : source.Functions) {
                for (const auto& block : f.Blocks) {
                    EXPECT_EQ(block.InstrDebugInfo.empty(), !info);
                }
            }
            llvm::LLVMContext context;
            auto module = Emit(source, context);
            ASSERT_TRUE(module);
            // Native DWARF emission currently excludes coroutine functions.
            EXPECT_EQ(module->getNamedMetadata("llvm.dbg.cu") != nullptr, info && !points);
            EXPECT_EQ(module->getNamedMetadata("wasm.custom_sections") != nullptr, points);
            unsigned calls = 0;
            for (const auto& block : *module->getFunction("main")) {
                for (const auto& instruction : block) {
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                        call && call->getCalledFunction()->getName() == "add")
                    {
                        ++calls;
                        ASSERT_EQ(call->arg_size(), 2);
                        EXPECT_EQ(llvm::cast<llvm::ConstantInt>(call->getArgOperand(0))->getSExtValue(), 21);
                        EXPECT_EQ(llvm::cast<llvm::ConstantInt>(call->getArgOperand(1))->getSExtValue(), 23);
                    }
                }
            }
            EXPECT_EQ(calls, 1);
        }
    }
}

TEST(LLVMDebugInfo, LoweringRetainsSourceModulePresence) {
    std::istringstream input(
        "(block (fun <main> () -> i64 (block (return (call helper))))"
        " (fun helper () -> i64 (block (return (: 7 i64)))))");
    NAst::NCore::TTokenStream tokens(input);
    NAst::NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed);
    auto ast = *parsed;
    NAst::TMaybeNode<NAst::TBlockExpr>(ast).Cast()->Stmts[1]->Origin = "imported";
    NSemantics::TNameResolver resolver;
    ASSERT_TRUE(NTransform::Pipeline(ast, resolver));
    TModule source;
    TBuilder builder(source);
    TAstLowerer lowerer(source, builder, resolver, {.EmitDebugInfo = true});
    ASSERT_TRUE(lowerer.LowerTop(ast));
    EXPECT_TRUE(source.HasSourceModules);
    ASSERT_EQ(source.Functions.size(), 2);
    llvm::LLVMContext context;
    auto module = Emit(source, context);
    ASSERT_TRUE(module);
    EXPECT_EQ(module->getNamedMetadata("llvm.dbg.cu"), nullptr);
}

TEST(LLVMDebugInfo, DisabledWithoutMetadataAtO1OrForSourceModules) {
    for (int mode = 0; mode < 3; ++mode) {
        TModule source;
        BuildModule(source, mode != 0);
        source.HasSourceModules = mode == 2;
        llvm::LLVMContext context;
        auto module = Emit(source, context, mode == 1 ? 1 : 0);
        ASSERT_TRUE(module);
        EXPECT_EQ(module->getNamedMetadata("llvm.dbg.cu"), nullptr);
        for (auto& function : *module) {
            EXPECT_EQ(function.getSubprogram(), nullptr);
        }
    }
}

int main(int argc, char** argv) {
    NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
