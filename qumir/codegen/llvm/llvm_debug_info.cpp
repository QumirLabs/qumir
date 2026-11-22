#include "llvm_debug_info.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace NQumir {
namespace NCodeGen {

TLLVMDebugInfo::TLLVMDebugInfo(llvm::Module& module, const NIR::TModule& sourceModule)
    : Module_(module)
    , SourceModule_(sourceModule)
    , Builder_(module)
{
    const auto path = sourceModule.SourceFilePath.empty()
        ? std::filesystem::path{}
        : std::filesystem::absolute(sourceModule.SourceFilePath).lexically_normal();
    File_ = Builder_.createFile(
        path.empty() ? "<stdin>" : path.filename().string(), path.parent_path().string());
    // The generated code uses the C ABI; debuggers do not know a Qumir language code.
    Builder_.createCompileUnit(llvm::dwarf::DW_LANG_C, File_, "Qumir", false, "", 0);
    module.addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
}

llvm::DIType* TLLVMDebugInfo::GetType(const NAst::TTypePtr& astType, int typeId) {
    auto& cached = Types_[typeId][astType.Get()];
    if (cached) {
        return cached;
    }
    const auto& types = SourceModule_.Types;
    if (typeId < 0) {
        return cached = Builder_.createUnspecifiedType("unknown");
    }
    if (auto named = NAst::TMaybeType<NAst::TNamedType>(astType)) {
        return cached = GetType(named.Cast()->UnderlyingType(), typeId);
    }
    const auto kind = types.GetKind(typeId);
    if (kind == NIR::EKind::Void) {
        return nullptr;
    }
    const uint64_t size = kind == NIR::EKind::F32
        ? 32
        : types.SizeInBytes(typeId) * 8;
    if (kind == NIR::EKind::Ptr) {
        const auto& layout = Module_.getDataLayout();
        const unsigned pointerSize = layout.getPointerSizeInBits();
        const unsigned pointerAlign = layout.getPointerABIAlignment(0).value() * 8;
        if (auto reference = NAst::TMaybeType<NAst::TReferenceType>(astType)) {
            return cached = Builder_.createReferenceType(
                llvm::dwarf::DW_TAG_reference_type,
                GetType(reference.Cast()->ReferencedType, types.UnderlyingType(typeId)),
                pointerSize, pointerAlign);
        }
        llvm::DIType* pointee = nullptr;
        const bool isString = NAst::TMaybeType<NAst::TStringType>(astType);
        if (isString) {
            pointee = Builder_.createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char);
        } else if (NAst::TMaybeType<NAst::TArrayType>(astType)) {
            // Bounds and the array layout are not carried by debug info yet.
            pointee = Builder_.createUnspecifiedType("array");
        } else {
            auto pointer = NAst::TMaybeType<NAst::TPointerType>(astType).Cast();
            pointee = GetType(pointer ? pointer->PointeeType : nullptr, types.UnderlyingType(typeId));
        }
        return cached = Builder_.createPointerType(
            pointee, pointerSize, pointerAlign, std::nullopt, isString ? "string" : "");
    }
    if (kind == NIR::EKind::Struct || kind == NIR::EKind::Func || kind == NIR::EKind::Undef) {
        return cached = Builder_.createUnspecifiedType(
            astType ? NAst::TypeDiagnosticName(astType) : "unknown");
    }
    if (NAst::TMaybeType<NAst::TSymbolType>(astType)) {
        return cached = Builder_.createBasicType("char32_t", size, llvm::dwarf::DW_ATE_UTF);
    }
    unsigned encoding;
    if (kind == NIR::EKind::I1) {
        encoding = llvm::dwarf::DW_ATE_boolean;
    } else if (types.IsFloat(typeId)) {
        encoding = llvm::dwarf::DW_ATE_float;
    } else {
        encoding = types.IsSigned(typeId)
            ? llvm::dwarf::DW_ATE_signed
            : llvm::dwarf::DW_ATE_unsigned;
    }
    std::ostringstream name;
    types.Print(name, typeId);
    return cached = Builder_.createBasicType(name.str(), size, encoding);
}

void TLLVMDebugInfo::BeginFunction(const NIR::TFunction& function, llvm::Function& llvmFunction) {
    Function_ = &function;
    Subprogram_ = nullptr;
    Scopes_.clear();
    if (!function.DebugInfo || function.IsCoroutine) {
        return;
    }
    const auto& info = *function.DebugInfo;
    auto signature = NAst::TMaybeType<NAst::TFunctionType>(info.AstType).Cast();
    std::vector<llvm::Metadata*> types;
    types.push_back(GetType(signature ? signature->ReturnType : nullptr, function.ReturnTypeId));
    for (size_t i = 0; i < function.ArgLocals.size(); ++i) {
        auto astType = signature && i < signature->ParamTypes.size()
            ? signature->ParamTypes[i]
            : nullptr;
        types.push_back(GetType(astType, function.LocalTypes[function.ArgLocals[i].Idx]));
    }
    Subprogram_ = Builder_.createFunction(
        File_, info.Name, function.Name, File_, std::max(0, info.Location.Line),
        Builder_.createSubroutineType(Builder_.getOrCreateTypeArray(types)),
        std::max(0, info.Location.Line), llvm::DINode::FlagZero,
        llvm::DISubprogram::SPFlagDefinition);
    llvmFunction.setSubprogram(Subprogram_);
    // O0 debug locations require machine code generation to preserve statement order.
    llvmFunction.addFnAttr(llvm::Attribute::NoInline);
    llvmFunction.addFnAttr(llvm::Attribute::OptimizeNone);
    Scopes_[info.ScopeId] = Subprogram_;
}

llvm::DILocalScope* TLLVMDebugInfo::GetScope(int scopeId) {
    if (scopeId < 0 || static_cast<size_t>(scopeId) >= Function_->ScopeParents.size()) {
        return Subprogram_;
    }
    auto [it, inserted] = Scopes_.emplace(scopeId, nullptr);
    if (!inserted) {
        if (!it->second) {
            throw std::runtime_error("cycle in debug scope parents");
        }
        return it->second;
    }
    auto* parent = GetScope(Function_->ScopeParents[scopeId]);
    auto* scope = Builder_.createLexicalBlock(parent, File_, 0, 0);
    Scopes_[scopeId] = scope;
    return scope;
}

llvm::DebugLoc TLLVMDebugInfo::GetLocation(const NIR::TInstrDebugInfo& info) {
    if (!Subprogram_) {
        return {};
    }
    // A call in a subprogram still needs a debug location when its source line is unknown.
    return llvm::DILocation::get(
        Module_.getContext(), std::max(0, info.Location.Line),
        info.Location.Line > 0 ? std::max(0, info.Location.Column) : 0, GetScope(info.ScopeId));
}

void TLLVMDebugInfo::DeclareLocals(
    std::span<llvm::AllocaInst* const> allocas,
    llvm::BasicBlock& entryBlock)
{
    if (!Subprogram_) {
        return;
    }
    std::vector<unsigned> argNumbers(Function_->LocalTypes.size());
    for (size_t i = 0; i < Function_->ArgLocals.size(); ++i) {
        argNumbers[Function_->ArgLocals[i].Idx] = i + 1;
    }
    const size_t count = std::min({allocas.size(), Function_->LocalTypes.size(), Function_->LocalDebugInfo.size()});
    for (size_t i = 0; i < count; ++i) {
        const auto& info = Function_->LocalDebugInfo[i];
        if (info.Name.empty() || !allocas[i]) {
            continue;
        }
        auto* type = GetType(info.AstType, Function_->LocalTypes[i]);
        auto* scope = argNumbers[i]
            ? Subprogram_
            : GetScope(info.ScopeId);
        auto* variable = argNumbers[i]
            ? Builder_.createParameterVariable(
                scope, info.Name, argNumbers[i], File_, std::max(0, info.Location.Line), type, true)
            : Builder_.createAutoVariable(
                scope, info.Name, File_, std::max(0, info.Location.Line), type, true);
        auto* location = llvm::DILocation::get(
            Module_.getContext(), std::max(0, info.Location.Line), std::max(0, info.Location.Column), scope);
        Builder_.insertDeclare(allocas[i], variable, Builder_.createExpression(), location, &entryBlock);
    }
}

void TLLVMDebugInfo::Finalize() {
    Builder_.finalize();
}

} // namespace NCodeGen
} // namespace NQumir
