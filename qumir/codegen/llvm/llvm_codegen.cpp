#include "llvm_codegen.h"
#include "llvm_codegen_impl.h"
#include "llvm_debug_info.h"

#include <qumir/ir/builder.h>
#include <qumir/ir/debug_data.h>
#include <qumir/align.h>

#include <memory>
#include <string>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <cassert>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Error.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/MC/MCSubtargetInfo.h>

// For optimization
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <utility>

namespace NQumir::NCodeGen {

using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

namespace {

llvm::Type* GetTypeById(int typeId, const TTypeTable& tt, llvm::LLVMContext& ctx) {
    if (typeId < 0) {
        // untyped, assume int64
        return llvm::Type::getInt64Ty(ctx);
    }
    switch (tt.GetKind(typeId)) {
        case EKind::I1: return llvm::Type::getInt1Ty(ctx);
        case EKind::I8:
        case EKind::U8:
            return llvm::Type::getInt8Ty(ctx);
        case EKind::I16:
        case EKind::U16:
            return llvm::Type::getInt16Ty(ctx);
        case EKind::I32: return llvm::Type::getInt32Ty(ctx);
        case EKind::U32:
            return llvm::Type::getInt32Ty(ctx);
        case EKind::I64: return llvm::Type::getInt64Ty(ctx);
        case EKind::U64:
            return llvm::Type::getInt64Ty(ctx);
        case EKind::I128:
        case EKind::U128:
            return llvm::Type::getInt128Ty(ctx);
        case EKind::F32: return llvm::Type::getFloatTy(ctx);
        case EKind::F64: return llvm::Type::getDoubleTy(ctx);
        case EKind::Void: return llvm::Type::getVoidTy(ctx);
        case EKind::Ptr: return llvm::PointerType::get(ctx, 0); // use pointer type (i8*) across targets
        case EKind::Func: return llvm::Type::getInt64Ty(ctx); // Function Id so far, TODO
        case EKind::Struct: {
            std::vector<llvm::Type*> fields;
            for (int f : tt.GetStructFields(typeId)) {
                fields.push_back(GetTypeById(f, tt, ctx));
            }
            return llvm::StructType::get(ctx, fields);
        }
        case EKind::Vec: {
            auto* element = GetTypeById(tt.UnderlyingType(typeId), tt, ctx);
            // Boolean lanes occupy one byte in IR storage, not one bit.
            if (element->isIntegerTy(1)) {
                element = llvm::Type::getInt8Ty(ctx);
            }
            return llvm::FixedVectorType::get(element, tt.VectorSize(typeId));
        }
        default:
            throw std::runtime_error("unsupported primitive type");
    }
}

llvm::MaybeAlign VectorMemoryAlign(const TTypeTable& types, int typeId) {
    // A pointer to IR vector storage only guarantees element alignment.
    return types.IsVector(typeId)
        ? llvm::MaybeAlign(types.SizeInBytes(types.UnderlyingType(typeId)))
        : llvm::MaybeAlign();
}

llvm::CmpInst::Predicate ComparisonPredicate(TOp opcode, bool floating, bool signedInteger) {
    llvm::CmpInst::Predicate predicate;
    switch (opcode) {
        case "<"_op: predicate = floating ? llvm::CmpInst::FCMP_OLT : llvm::CmpInst::ICMP_SLT; break;
        case ">"_op: predicate = floating ? llvm::CmpInst::FCMP_OGT : llvm::CmpInst::ICMP_SGT; break;
        case "<="_op: predicate = floating ? llvm::CmpInst::FCMP_OLE : llvm::CmpInst::ICMP_SLE; break;
        case ">="_op: predicate = floating ? llvm::CmpInst::FCMP_OGE : llvm::CmpInst::ICMP_SGE; break;
        case "=="_op: predicate = floating ? llvm::CmpInst::FCMP_OEQ : llvm::CmpInst::ICMP_EQ; break;
        case "!="_op: predicate = floating ? llvm::CmpInst::FCMP_UNE : llvm::CmpInst::ICMP_NE; break;
        default: throw std::runtime_error("Unsupported comparison opcode");
    }
    return !floating && !signedInteger
        ? llvm::ICmpInst::getUnsignedPredicate(predicate)
        : predicate;
}

llvm::Value* EmitVectorBinary(
    llvm::IRBuilder<>& builder,
    TOp opcode,
    llvm::FixedVectorType* type,
    llvm::Value* left,
    llvm::Value* right,
    bool signedElement)
{
    auto broadcast = [&](llvm::Value* value) {
        if (value->getType() == type) {
            return value;
        }
        if (value->getType()->isIntegerTy(1) && type->getElementType()->isIntegerTy(8)) {
            value = builder.CreateZExt(value, type->getElementType(), "boolbyte");
        }
        if (value->getType() != type->getElementType()) {
            throw std::runtime_error("Vector operand type mismatch");
        }
        return builder.CreateVectorSplat(type->getNumElements(), value, "broadcast");
    };
    left = broadcast(left);
    right = broadcast(right);
    const bool floating = type->getElementType()->isFloatingPointTy();
    switch (opcode) {
        case "+"_op:
            return builder.CreateBinOp(floating ? llvm::Instruction::FAdd : llvm::Instruction::Add, left, right, "vadd");
        case "-"_op:
            return builder.CreateBinOp(floating ? llvm::Instruction::FSub : llvm::Instruction::Sub, left, right, "vsub");
        case "*"_op:
            return builder.CreateBinOp(floating ? llvm::Instruction::FMul : llvm::Instruction::Mul, left, right, "vmul");
        case "&"_op:
        case "&&"_op:
            return builder.CreateAnd(left, right, "vand");
        case "|"_op:
        case "||"_op:
            return builder.CreateOr(left, right, "vor");
        case "^"_op:
            return builder.CreateXor(left, right, "vxor");
        case "<<"_op:
        case ">>"_op: {
            auto* mask = llvm::ConstantVector::getSplat(type->getElementCount(),
                llvm::ConstantInt::get(type->getElementType(), type->getElementType()->getIntegerBitWidth() - 1));
            right = builder.CreateAnd(right, mask, "vshiftmask");
            if (opcode == "<<"_op) {
                return builder.CreateShl(left, right, "vshl");
            }
            return signedElement
                ? builder.CreateAShr(left, right, "vashr")
                : builder.CreateLShr(left, right, "vlshr");
        }
        default:
            break;
    }
    const auto predicate = ComparisonPredicate(opcode, floating, signedElement);
    auto* booleanType = llvm::FixedVectorType::get(builder.getInt8Ty(), type->getNumElements());
    return builder.CreateZExt(builder.CreateCmp(predicate, left, right, "vcmp"), booleanType, "vbool");
}

llvm::Value* TruthValue(llvm::IRBuilder<>& builder, llvm::Value* value, bool negate = false) {
    auto* zero = llvm::Constant::getNullValue(value->getType());
    if (value->getType()->getScalarType()->isFloatingPointTy()) {
        return builder.CreateFCmp(negate ? llvm::CmpInst::FCMP_OEQ : llvm::CmpInst::FCMP_UNE, value, zero, "vtruth");
    }
    return builder.CreateICmp(negate ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE, value, zero, "vtruth");
}

// External calls must follow the target C ABI for by-value structs.
enum class EAbiArch { X86_64, AArch64, Wasm, Other };

EAbiArch AbiArchOf(const llvm::Triple& triple) {
    switch (triple.getArch()) {
        case llvm::Triple::x86_64: return EAbiArch::X86_64;
        case llvm::Triple::aarch64: return EAbiArch::AArch64;
        case llvm::Triple::wasm32:
        case llvm::Triple::wasm64:
            return EAbiArch::Wasm;
        default: return EAbiArch::Other;
    }
}

EAbiArch ModuleAbiArch(const llvm::Module& module) {
#if LLVM_VERSION_MAJOR <= 20
    return AbiArchOf(llvm::Triple(module.getTargetTriple()));
#else
    return AbiArchOf(module.getTargetTriple());
#endif
}

llvm::Type* AbiIntType(llvm::LLVMContext& ctx, int bytes) {
    int width = bytes <= 1 ? 1 : bytes <= 2 ? 2 : bytes <= 4 ? 4 : 8;
    return llvm::Type::getIntNTy(ctx, width * 8);
}

llvm::Type* AbiFpType(llvm::LLVMContext& ctx, int bytes) {
    return bytes <= 4 ? llvm::Type::getFloatTy(ctx) : llvm::Type::getDoubleTy(ctx);
}

struct TStructAbi {
    bool Memory = false;             // passed by pointer (byval / copy), returned via sret
    std::vector<llvm::Type*> Pieces;
    std::vector<int> Offsets;        // byte offset of each piece in the struct
};

// Classifies a by-value struct into the registers the C ABI uses:
//   wasm                -> Memory (byval / sret).
//   AArch64 all-float   -> one FP register per field (homogeneous float aggregate).
//   other >16-byte      -> Memory.
//   AArch64 otherwise   -> one integer register per eightbyte (general registers).
//   x86-64 SysV         -> per eightbyte, an FP register if every overlapping field
//                          is floating point, otherwise an integer register.
TStructAbi ClassifyStructAbi(int typeId, const TTypeTable& tt, llvm::LLVMContext& ctx, EAbiArch arch) {
    TStructAbi abi;
    int size = tt.SizeInBytes(typeId);
    const std::vector<int>& fields = tt.GetStructFields(typeId);
    if (arch == EAbiArch::Wasm) {
        abi.Memory = true;
        return abi;
    }

    bool allFloat = !fields.empty();
    for (int f : fields) {
        allFloat = allFloat && tt.IsFloat(f);
    }
    // AArch64 homogeneous floating-point aggregates of up to four members use
    // FP registers even when their total size exceeds two eightbytes.
    if (arch == EAbiArch::AArch64 && allFloat && fields.size() <= 4) {
        int offset = 0;
        for (int f : fields) {
            int fieldSize = tt.SizeInBytes(f);
            offset = AlignUp(offset, std::min(fieldSize, 8));
            abi.Pieces.push_back(AbiFpType(ctx, fieldSize));
            abi.Offsets.push_back(offset);
            offset += fieldSize;
        }
        return abi;
    }
    if (size == 0 || size > 16) {
        abi.Memory = true;
        return abi;
    }

    bool sse[2] = {arch == EAbiArch::X86_64, arch == EAbiArch::X86_64};
    int offset = 0;
    for (int f : fields) {
        int fieldSize = tt.SizeInBytes(f);
        offset = AlignUp(offset, std::min(fieldSize, 8));
        if (!tt.IsFloat(f)) {
            for (int eb = offset / 8; eb <= (offset + fieldSize - 1) / 8 && eb < 2; ++eb) {
                sse[eb] = false;
            }
        }
        offset += fieldSize;
    }
    for (int eb = 0; eb * 8 < size; ++eb) {
        int bytes = std::min(size - eb * 8, 8);
        abi.Pieces.push_back(sse[eb] ? AbiFpType(ctx, bytes) : AbiIntType(ctx, bytes));
        abi.Offsets.push_back(eb * 8);
    }
    return abi;
}

// ABI temporaries (argument spills, byval copies, sret buffers) must live in the
// entry block. An alloca emitted at the call site is a dynamic stack allocation
// for backends without a frame pointer (notably wasm), where every loop
// iteration would push the shadow stack down and never restore it, so a kernel
// looping over a batch exhausts its 256 KB stack and starts handing out
// addresses outside linear memory
llvm::AllocaInst* AbiEntryAlloca(
    llvm::IRBuilder<>& irb, llvm::Type* type, const char* name)
{
    auto* function = irb.GetInsertBlock()->getParent();
    auto& entry = function->getEntryBlock();
    llvm::IRBuilder<> entryBuilder(&entry, entry.getFirstInsertionPt());
    return entryBuilder.CreateAlloca(type, nullptr, name);
}

// Spills an aggregate value to the stack so its bytes can be read as ABI pieces.
llvm::Value* AbiAsPointer(llvm::IRBuilder<>& irb, llvm::Value* v, llvm::Type* structTy) {
    if (v->getType()->isPointerTy()) {
        return v;
    }
    auto* slot = AbiEntryAlloca(irb, structTy, "argspill");
    irb.CreateStore(v, slot);
    return slot;
}

// The Memory class is passed by pointer differently per ABI: x86-64 SysV places
// the bytes in the stack argument area (byval), AArch64 passes a pointer to a
// caller-made copy.
void AbiGatherArg(llvm::IRBuilder<>& irb, llvm::Value* structPtr, llvm::Type* structTy,
    const TStructAbi& abi, EAbiArch arch, std::vector<llvm::Type*>& paramTys,
    std::vector<llvm::Value*>& args, std::vector<std::pair<unsigned, llvm::Type*>>& byval)
{
    if (abi.Memory) {
        paramTys.push_back(structPtr->getType());
        if (arch == EAbiArch::X86_64 || arch == EAbiArch::Wasm) {
            byval.push_back({(unsigned)args.size(), structTy});
            args.push_back(structPtr);
        } else {
            auto& dl = irb.GetInsertBlock()->getModule()->getDataLayout();
            auto align = dl.getABITypeAlign(structTy);
            auto* copy = AbiEntryAlloca(irb, structTy, "abi.copy");
            irb.CreateMemCpy(copy, align, structPtr, align, dl.getTypeAllocSize(structTy));
            args.push_back(copy);
        }
        return;
    }
    auto* i8 = llvm::Type::getInt8Ty(irb.getContext());
    for (size_t i = 0; i < abi.Pieces.size(); ++i) {
        auto* gep = irb.CreateGEP(i8, structPtr, irb.getInt64(abi.Offsets[i]), "abi.field");
        paramTys.push_back(abi.Pieces[i]);
        args.push_back(irb.CreateLoad(abi.Pieces[i], gep, "abi.arg"));
    }
}

void AbiScatterReturn(llvm::IRBuilder<>& irb, llvm::Value* call, llvm::Value* buf, const TStructAbi& abi) {
    auto* i8 = llvm::Type::getInt8Ty(irb.getContext());
    for (size_t i = 0; i < abi.Pieces.size(); ++i) {
        llvm::Value* piece = abi.Pieces.size() == 1 ? call : irb.CreateExtractValue(call, {(unsigned)i}, "abi.piece");
        auto* gep = irb.CreateGEP(i8, buf, irb.getInt64(abi.Offsets[i]), "abi.field");
        irb.CreateStore(piece, gep);
    }
}

llvm::Type* AbiReturnType(const TStructAbi& abi, llvm::LLVMContext& ctx) {
    if (abi.Memory) {
        return llvm::Type::getVoidTy(ctx);
    }
    return abi.Pieces.size() == 1 ? abi.Pieces[0] : llvm::StructType::get(ctx, abi.Pieces);
}

void AbiApplyAttrs(llvm::CallInst* ci, llvm::FunctionCallee fc,
    const std::vector<std::pair<unsigned, llvm::Type*>>& byval, llvm::Type* sretTy)
{
    auto& ctx = ci->getContext();
    auto* fn = llvm::dyn_cast<llvm::Function>(fc.getCallee());
    auto apply = [&](unsigned idx, llvm::Attribute attr) {
        ci->addParamAttr(idx, attr);
        if (fn) {
            fn->addParamAttr(idx, attr);
        }
    };
    if (sretTy) {
        apply(0, llvm::Attribute::getWithStructRetType(ctx, sretTy));
    }
    for (const auto& [idx, sty] : byval) {
        apply(idx, llvm::Attribute::getWithByValType(ctx, sty));
    }
}

// Coerces by-value struct arguments/return per the platform C ABI. Consumes the
// pending argument values; returns the call result (nullptr for void); `cast`
// adapts a scalar argument to its parameter type.
template<class TCast>
llvm::Value* EmitCoercedExternalCall(llvm::IRBuilder<>& irb, llvm::Module& lmodule,
    const TTypeTable& types, const TExternalFunction& extFun, EAbiArch arch,
    std::vector<llvm::Value*>& pendingArgs, TCast&& cast)
{
    auto& ctx = irb.getContext();
    std::vector<llvm::Type*> paramTys;
    std::vector<llvm::Value*> args;
    std::vector<std::pair<unsigned, llvm::Type*>> byval;
    std::vector<unsigned> zeroExtended;

    int retId = extFun.ReturnTypeId;
    bool retIsStruct = retId >= 0 && types.GetKind(retId) == EKind::Struct;
    auto* declaredRetTy = GetTypeById(retId, types, ctx);
    llvm::Type* retType = declaredRetTy;
    TStructAbi retAbi;
    llvm::Value* sret = nullptr;
    if (retIsStruct) {
        retAbi = ClassifyStructAbi(retId, types, ctx, arch);
        retType = AbiReturnType(retAbi, ctx);
        if (retAbi.Memory) {
            sret = AbiEntryAlloca(irb, declaredRetTy, "sret");
            paramTys.push_back(sret->getType());
            args.push_back(sret);
        }
    }

    if (pendingArgs.size() != extFun.ArgTypes.size()) {
        throw std::runtime_error("wrong argument count for external call: " + extFun.Name);
    }
    for (size_t i = 0; i < extFun.ArgTypes.size(); ++i) {
        int tid = extFun.ArgTypes[i];
        if (tid >= 0 && types.GetKind(tid) == EKind::Struct) {
            auto* structTy = GetTypeById(tid, types, ctx);
            auto* ptr = AbiAsPointer(irb, pendingArgs[i], structTy);
            AbiGatherArg(irb, ptr, structTy, ClassifyStructAbi(tid, types, ctx, arch), arch, paramTys, args, byval);
        } else {
            auto* pty = GetTypeById(tid, types, ctx);
            if (pty->isIntegerTy(1)) {
                // Runtime functions read a boolean argument as a whole register.
                zeroExtended.push_back(paramTys.size());
            }
            paramTys.push_back(pty);
            args.push_back(cast(pendingArgs[i], pty));
        }
    }
    pendingArgs.clear();

    auto fc = lmodule.getOrInsertFunction(extFun.MangledName, llvm::FunctionType::get(retType, paramTys, false));
    auto* ci = irb.CreateCall(fc, args);
    AbiApplyAttrs(ci, fc, byval, sret ? declaredRetTy : nullptr);
    for (unsigned idx : zeroExtended) {
        ci->addParamAttr(idx, llvm::Attribute::ZExt);
        if (auto* fn = llvm::dyn_cast<llvm::Function>(fc.getCallee())) {
            fn->addParamAttr(idx, llvm::Attribute::ZExt);
        }
    }

    if (!retIsStruct) {
        return retType->isVoidTy() ? nullptr : ci;
    }
    llvm::Value* buf = sret ? sret : AbiEntryAlloca(irb, declaredRetTy, "retbuf");
    if (!sret) {
        AbiScatterReturn(irb, ci, buf, retAbi);
    }
    return irb.CreateLoad(declaredRetTy, buf, "retstruct");
}

// "builtin::" is the always-loaded qumir/modules/builtins prelude
enum class EBuiltinIntrinsic {
    Memcpy,
    Memmove,
    Cttz,
    Ctlz,
    Ctpop,
    ByteMatch8,
    ByteMatch16,
    ByteMatchBackend
};

static const std::unordered_map<std::string, EBuiltinIntrinsic> BuiltinIntrinsics = {
    {"builtin::memcpy", EBuiltinIntrinsic::Memcpy},
    {"builtin::memmove", EBuiltinIntrinsic::Memmove},
    {"builtin::cttz", EBuiltinIntrinsic::Cttz},
    {"builtin::ctlz", EBuiltinIntrinsic::Ctlz},
    {"builtin::ctpop", EBuiltinIntrinsic::Ctpop},
    {"builtin::byte_match8", EBuiltinIntrinsic::ByteMatch8},
    {"builtin::byte_match16", EBuiltinIntrinsic::ByteMatch16},
    {"builtin::byte_match_backend", EBuiltinIntrinsic::ByteMatchBackend},
};

int ByteMatchBackend(const llvm::TargetMachine& tm) {
    const auto arch = tm.getTargetTriple().getArch();
    const auto* features = tm.getMCSubtargetInfo();
    if ((arch == llvm::Triple::x86 || arch == llvm::Triple::x86_64) &&
        features && features->checkFeatures("+sse2"))
    {
        return 2;
    }
    if (arch == llvm::Triple::aarch64 && features && features->checkFeatures("+neon")) {
        return 1;
    }
    return 0;
}

llvm::Value* PackVectorBits(llvm::IRBuilder<>& irb, llvm::Value* lanes, llvm::IntegerType* resultType) {
    auto* type = llvm::cast<llvm::FixedVectorType>(lanes->getType());
    const unsigned count = type->getNumElements();
    const unsigned bits = type->getElementType()->getIntegerBitWidth();
    if (!irb.GetInsertBlock()->getModule()->getDataLayout().isBigEndian()) {
        // This pattern lets x86 select PMOVMSKB for byte comparisons.
        auto* packed = irb.CreateBitCast(lanes, irb.getIntNTy(count * bits), "mask.bits");
        return irb.CreateZExtOrTrunc(packed, resultType, "mask");
    }
    // Pack lanes from low to high bits independently of target endianness.
    llvm::Value* result = llvm::ConstantInt::get(resultType, 0);
    for (unsigned i = 0; i < count; ++i) {
        auto* bit = irb.CreateZExt(irb.CreateExtractElement(lanes, i), resultType);
        result = irb.CreateOr(result, irb.CreateShl(bit, i * bits));
    }
    return result;
}

template<class TCast>
llvm::Value* EmitBuiltinByteMatch(
    llvm::IRBuilder<>& irb,
    EBuiltinIntrinsic kind,
    std::vector<llvm::Value*>& pendingArgs,
    TCast&& cast)
{
    if (pendingArgs.size() != 2) {
        throw std::runtime_error("builtin byte_match8/16 expects 2 arguments");
    }
    const bool compact = kind == EBuiltinIntrinsic::ByteMatch16;
    const unsigned width = compact ? 16 : 8;
    auto& ctx = irb.getContext();
    auto* i8Ty = llvm::Type::getInt8Ty(ctx);
    auto* resultTy = compact
        ? llvm::Type::getInt32Ty(ctx)
        : llvm::Type::getInt64Ty(ctx);
    auto* bytesTy = llvm::FixedVectorType::get(i8Ty, width);
    auto* ptr = cast(pendingArgs[0], llvm::PointerType::get(ctx, 0));
    auto* byte = cast(pendingArgs[1], i8Ty);
    pendingArgs.clear();
    auto* bytes = irb.CreateLoad(bytesTy, ptr, "byte.ctrl");
    bytes->setAlignment(llvm::Align(1));
    auto* matches = irb.CreateICmpEQ(bytes, irb.CreateVectorSplat(width, byte));

    // NEON CMEQ already produces the eight 0x00/0xff bytes represented by u64.
    auto* lanes = compact
        ? matches
        : irb.CreateSExt(matches, bytesTy);
    return PackVectorBits(irb, lanes, resultTy);
}

template<class TCast>
llvm::Value* EmitBuiltinBitCountCall(llvm::IRBuilder<>& irb, EBuiltinIntrinsic kind,
    std::vector<llvm::Value*>& pendingArgs, TCast&& cast)
{
    if (pendingArgs.size() != 1) {
        throw std::runtime_error("builtin cttz/ctlz/ctpop expects 1 argument");
    }
    auto* i64Ty = llvm::Type::getInt64Ty(irb.getContext());
    auto* value = cast(pendingArgs[0], i64Ty);
    pendingArgs.clear();
    if (kind == EBuiltinIntrinsic::Ctpop) {
        return irb.CreateIntrinsic(llvm::Intrinsic::ctpop, {i64Ty}, {value});
    }
    auto id = kind == EBuiltinIntrinsic::Cttz
        ? llvm::Intrinsic::cttz
        : llvm::Intrinsic::ctlz;
    return irb.CreateIntrinsic(id, {i64Ty}, {value, irb.getFalse()});
}

template<class TCast>
llvm::Value* EmitBuiltinIntrinsicCall(
    llvm::IRBuilder<>& irb,
    EBuiltinIntrinsic kind,
    std::vector<llvm::Value*>& pendingArgs,
    TCast&& cast,
    const llvm::TargetMachine& tm)
{
    if (kind == EBuiltinIntrinsic::ByteMatchBackend) {
        if (!pendingArgs.empty()) {
            throw std::runtime_error("builtin byte_match_backend expects no arguments");
        }
        return irb.getInt64(ByteMatchBackend(tm));
    }
    if (kind == EBuiltinIntrinsic::ByteMatch8 || kind == EBuiltinIntrinsic::ByteMatch16) {
        return EmitBuiltinByteMatch(irb, kind, pendingArgs, cast);
    }
    if (kind == EBuiltinIntrinsic::Cttz || kind == EBuiltinIntrinsic::Ctlz ||
        kind == EBuiltinIntrinsic::Ctpop)
    {
        return EmitBuiltinBitCountCall(irb, kind, pendingArgs, cast);
    }
    if (pendingArgs.size() != 3) {
        throw std::runtime_error("builtin memcpy/memmove expects 3 arguments");
    }
    auto* ptrTy = llvm::PointerType::get(irb.getContext(), 0);
    auto* i64Ty = llvm::Type::getInt64Ty(irb.getContext());
    auto* dst = cast(pendingArgs[0], ptrTy);
    auto* src = cast(pendingArgs[1], ptrTy);
    auto* len = cast(pendingArgs[2], i64Ty);
    pendingArgs.clear();
    if (kind == EBuiltinIntrinsic::Memcpy) {
        irb.CreateMemCpy(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), len);
    } else {
        irb.CreateMemMove(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), len);
    }
    return dst;
}

std::pair<std::string, std::string> GetNativeCpuAndFeatures() {
    auto cpu = llvm::sys::getHostCPUName().str();
    std::string features;
    auto hostFeatures = llvm::sys::getHostCPUFeatures();
    for (const auto& feature : hostFeatures) {
        if (!features.empty()) {
            features += ",";
        }
        features += feature.getValue() ? "+" : "-";
        features += feature.getKey().str();
    }
    return {std::move(cpu), std::move(features)};
}

bool IsSignedIntegerType(const TTypeTable& tt, int typeId) {
    switch (tt.GetKind(typeId)) {
        case EKind::U8:
        case EKind::U16:
        case EKind::U32:
        case EKind::U64:
        case EKind::U128:
            return false;
        default:
            return true;
    }
}

void MarkPointerArgsNoAlias(llvm::Function* function) {
    if (!function) {
        return;
    }
    for (auto& arg : function->args()) {
        if (arg.getType()->isPointerTy()) {
            arg.addAttr(llvm::Attribute::NoAlias);
        }
    }
}

void EmitCoroutineRuntimeHelpers(llvm::Module& module, llvm::LLVMContext& ctx) {
    auto* i1Ty = llvm::Type::getInt1Ty(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    auto getFn = [&](llvm::StringRef name, llvm::FunctionType* type) {
        return module.getOrInsertFunction(name, type);
    };

    auto coroDone = getFn("llvm.coro.done", llvm::FunctionType::get(i1Ty, {ptrTy}, false));
    auto coroResume = getFn("llvm.coro.resume", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    auto coroDestroy = getFn("llvm.coro.destroy", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    auto createHelper = [&](llvm::StringRef name, llvm::FunctionType* type) {
        if (auto* existing = module.getFunction(name)) {
            if (!existing->isDeclaration()) {
                return existing;
            }
            existing->eraseFromParent();
        }
        auto* helper = llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module);
        MarkPointerArgsNoAlias(helper);
        return helper;
    };

    {
        auto* helper = createHelper("__qumir_coro_done", llvm::FunctionType::get(i32Ty, {ptrTy}, false));
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", helper);
        llvm::IRBuilder<> irb(bb);
        auto* done = irb.CreateCall(coroDone, {helper->getArg(0)}, "done");
        auto* result = irb.CreateZExt(done, i32Ty, "done.i32");
        irb.CreateRet(result);
    }

    {
        auto* helper = createHelper("__qumir_coro_resume", llvm::FunctionType::get(voidTy, {ptrTy}, false));
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", helper);
        llvm::IRBuilder<> irb(bb);
        irb.CreateCall(coroResume, {helper->getArg(0)});
        irb.CreateRetVoid();
    }

    {
        auto* helper = createHelper("__qumir_coro_destroy", llvm::FunctionType::get(voidTy, {ptrTy}, false));
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", helper);
        llvm::IRBuilder<> irb(bb);
        irb.CreateCall(coroDestroy, {helper->getArg(0)});
        irb.CreateRetVoid();
    }

    {
        // Returns a pointer to the coroutine's promise (result storage at offset 0).
        auto coroPromise = getFn("llvm.coro.promise", llvm::FunctionType::get(
            ptrTy, {ptrTy, i32Ty, i1Ty}, false));
        auto* helper = createHelper("__qumir_coro_promise_ptr", llvm::FunctionType::get(ptrTy, {ptrTy}, false));
        auto* bb = llvm::BasicBlock::Create(ctx, "entry", helper);
        llvm::IRBuilder<> irb(bb);
        auto* promise = irb.CreateCall(coroPromise, {
            helper->getArg(0),
            llvm::ConstantInt::get(i32Ty, 0),
            llvm::ConstantInt::getFalse(ctx)
        }, "promise");
        irb.CreateRet(promise);
    }
}

std::unordered_map<uint64_t, llvm::Instruction::BinaryOps> ibinOpMap = {
    { "+"_op, llvm::Instruction::Add },
    { "-"_op, llvm::Instruction::Sub },
    { "*"_op, llvm::Instruction::Mul },
    { "/"_op, llvm::Instruction::SDiv }, // signed division
    { "%"_op, llvm::Instruction::SRem }, // signed remainder
};

std::unordered_map<uint64_t, llvm::Instruction::BinaryOps> ubinOpMap = {
    { "+"_op, llvm::Instruction::Add },
    { "-"_op, llvm::Instruction::Sub },
    { "*"_op, llvm::Instruction::Mul },
    { "/"_op, llvm::Instruction::UDiv }, // unsigned division
    { "%"_op, llvm::Instruction::URem }, // unsigned remainder
};

std::unordered_map<uint64_t, llvm::Instruction::BinaryOps> fbinOpMap = {
    { "+"_op, llvm::Instruction::FAdd },
    { "-"_op, llvm::Instruction::FSub },
    { "*"_op, llvm::Instruction::FMul },
    { "/"_op, llvm::Instruction::FDiv },
};

} // namespace

std::unordered_set<std::string> CollectInlineDefinitions(
    const NIR::TModule& module,
    const std::unordered_set<std::string>& cacheable,
    const std::unordered_set<std::string>& definitions)
{
    // Importing a body lets LLVM inline it, but also makes every optimization
    // pass visit it again. Keep automatic imports small and bounded; explicit
    // inline requests are exempt from the budget.
    constexpr size_t maxAutomaticFunctionInstructions = 256;
    size_t automaticBudget = 4096;
    std::unordered_map<int, const NIR::TFunction*> functions;
    std::vector<int> pending;
    for (const auto& function : module.Functions) {
        if (function.Blocks.empty()) {
            continue;
        }
        functions[function.SymId] = &function;
        if (definitions.count(function.Name)) {
            pending.push_back(function.SymId);
        }
    }
    std::unordered_set<int> visited;
    std::unordered_set<std::string> imports;
    for (size_t index = 0; index < pending.size(); ++index) {
        const int symId = pending[index];
        auto found = functions.find(symId);
        if (found == functions.end() || !visited.insert(symId).second) {
            continue;
        }
        const auto& function = *found->second;
        if (!definitions.count(function.Name)) {
            if (function.IsCoroutine || !cacheable.count(function.Name)) {
                continue;
            }
            size_t instructions = 0;
            for (const auto& block : function.Blocks) {
                instructions += block.Instrs.size() + block.Phis.size();
            }
            if (!function.Inline) {
                if (
                    instructions > maxAutomaticFunctionInstructions ||
                    instructions > automaticBudget) {
                    continue;
                }
                automaticBudget -= instructions;
            }
            imports.insert(function.Name);
        }
        // A large out-of-line callee stops this traversal. Its own cache miss
        // selects imports separately, starting at that object's definition.
        for (const auto& block : function.Blocks) {
            for (const auto& instruction : block.Instrs) {
                if (
                    (instruction.Op == "call"_op || instruction.Op == "await"_op) &&
                    instruction.OperandCount >= 1 &&
                    instruction.Operands[0].Type == NIR::TOperand::EType::Imm)
                {
                    pending.push_back(static_cast<int>(instruction.Operands[0].Imm.Value));
                }
            }
        }
    }
    return imports;
}

std::vector<std::string> CollectCacheableSymbols(const NIR::TModule& module) {
    // Candidate cacheable definitions (generic instance or `cacheable`); a
    // coroutine lowers to several symbols one name cannot describe.
    std::unordered_set<int> candidates;
    std::unordered_set<int> definedSyms;
    for (const auto& fn : module.Functions) {
        definedSyms.insert(fn.SymId);
        if (fn.Blocks.empty() || fn.IsCoroutine) {
            continue;
        }
        if (IsCacheableSymbol(fn.Name) || fn.Cacheable) {
            candidates.insert(fn.SymId);
        }
    }

    // Defined callees of each candidate (external/runtime callees never matter,
    // they resolve at load time everywhere).
    std::unordered_map<int, std::vector<int>> definedCallees;
    for (const auto& fn : module.Functions) {
        if (!candidates.count(fn.SymId)) {
            continue;
        }
        auto& callees = definedCallees[fn.SymId];
        for (const auto& block : fn.Blocks) {
            for (const auto& instr : block.Instrs) {
                if ((instr.Op == "call"_op || instr.Op == "await"_op)
                    && instr.OperandCount >= 1
                    && instr.Operands[0].Type == TOperand::EType::Imm) {
                    int callee = static_cast<int>(instr.Operands[0].Imm.Value);
                    if (definedSyms.count(callee)) {
                        callees.push_back(callee);
                    }
                }
            }
        }
    }

    // A cache object must be self-contained: reference only cacheable or runtime
    // symbols. Demote (to fixpoint) any candidate that calls a defined
    // non-candidate (a query-specific function like jt_residual_filter, or an
    // already-demoted candidate) - its object could not link into a kernel that
    // lacks that symbol.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = candidates.begin(); it != candidates.end();) {
            bool demote = false;
            for (int callee : definedCallees[*it]) {
                if (!candidates.count(callee)) {
                    demote = true;
                    break;
                }
            }
            if (demote) {
                it = candidates.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    std::vector<std::string> out;
    for (const auto& fn : module.Functions) {
        if (candidates.count(fn.SymId)) {
            out.push_back(fn.Name);
        }
    }
    return out;
}

TLLVMCodeGen::TLLVMCodeGen(const TLLVMCodeGenOptions& opts): Opts(opts) {}
TLLVMCodeGen::~TLLVMCodeGen() = default;

std::unique_ptr<ILLVMModuleArtifacts> TLLVMCodeGen::Emit(TModule& module, int optLevel) {
    Ctx = std::make_unique<llvm::LLVMContext>();
    LModule = std::make_unique<llvm::Module>(Opts.ModuleName, *Ctx);

    auto tripleString = Opts.TargetTriple.empty()
        ? llvm::sys::getDefaultTargetTriple()
        : Opts.TargetTriple;

#if LLVM_VERSION_MAJOR <= 20
    LModule->setTargetTriple(tripleString);
#else
    LModule->setTargetTriple(llvm::Triple(tripleString));
#endif

    // Lowering queries the data layout for struct sizes, so it must be set
    // before any instruction is emitted, at every optimization level.
    CreateTargetMachine();

    auto builder = std::make_unique<llvm::IRBuilder<>>(*Ctx);
    BuilderBase = std::move(builder);
    if (0) {
        // fast-math
        auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
        llvm::FastMathFlags FMF;
        FMF.setAllowContract(true);
        FMF.setAllowReassoc(true);
        FMF.setNoNaNs(true);
        FMF.setNoInfs(true);
        FMF.setAllowReciprocal(true);
        irb->setFastMathFlags(FMF);
    }
    // Whether this function gets a body here (vs an external declaration).
    auto shouldDefine = [&](const std::string& name) -> bool {
        if (Opts.RestrictToDefinitions) {
            return Opts.RestrictToDefinitions->count(name) != 0;
        }
        if (Opts.EmitAsExternal) {
            return Opts.EmitAsExternal->count(name) == 0;
        }
        return true;
    };

    std::unordered_set<int> newSymIds;
    // Pass 1: predeclare all functions so calls can reference them by SymId in any order
    for (const auto& f : module.Functions) {
        auto maybeUniqueIdIt = SymIdToUniqueFunId.find(f.SymId);
        if (maybeUniqueIdIt != SymIdToUniqueFunId.end()) {
            if (maybeUniqueIdIt->second == f.UniqueId) {
                continue;
            } else {
                // delete old function and re-declare
                auto* oldFun = LModule->getFunction(f.Name);
                if (oldFun) {
                    oldFun->eraseFromParent();
                }
            }
        }

        auto& ctx = *Ctx;
        // Struct arguments and returns are modeled as ordinary LLVM values.
        std::vector<llvm::Type*> argTys;
        for (size_t i = 0; i < f.ArgLocals.size(); ++i) {
            const auto& s = f.ArgLocals[i];
            auto typeId = f.LocalTypes[s.Idx];
            argTys.push_back(GetTypeById(typeId, module.Types, ctx));
        }

        llvm::Type* retTy = GetTypeById(f.ReturnTypeId, module.Types, ctx);
        auto fty = llvm::FunctionType::get(retTy, argTys, false);
        auto lfun = LModule->getFunction(f.Name);
        if (!lfun) {
            lfun = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, f.Name, LModule.get());
        } else {
            throw std::runtime_error("function already declared");
        }
        MarkPointerArgsNoAlias(lfun);
        if (module.DebugOptions.EmitDebugPoints || f.Name.find("__repl") == 0) { // repl functions should not be optimized
            lfun->addFnAttr(llvm::Attribute::NoInline);
            lfun->addFnAttr(llvm::Attribute::OptimizeNone);
            lfun->addFnAttr("disable-tail-calls", "true");
        } else if (f.Inline && !f.Blocks.empty() && !f.IsCoroutine) {
            lfun->addFnAttr(llvm::Attribute::AlwaysInline);
        }
        SymIdToLFun[f.SymId] = lfun;
        if (
            shouldDefine(f.Name) ||
            (Opts.InlineDefinitions && Opts.InlineDefinitions->count(f.Name)))
        {
            newSymIds.insert(f.SymId);
            SymIdToUniqueFunId[f.SymId] = f.UniqueId;
        }
    }

    if (!module.AsyncMode.empty()) {
        LModule->getOrInsertNamedMetadata("wasm.custom_sections")->addOperand(llvm::MDNode::get(*Ctx, {
            llvm::MDString::get(*Ctx, "qumir.runtime"), llvm::MDString::get(*Ctx, SerializeRuntimeData(module))}));
    }
    if (module.DebugOptions.EmitDebugPoints) {
        auto data = SerializeDebugData(module);
        LModule->getOrInsertNamedMetadata("wasm.custom_sections")->addOperand(llvm::MDNode::get(*Ctx, {
            llvm::MDString::get(*Ctx, "qumir.debug"), llvm::MDString::get(*Ctx, data)}));
    }

    // Pass 2: lower function bodies
    std::unique_ptr<TLLVMDebugInfo> debugInfo;
    if (
        module.DebugOptions.EmitDebugInfo && optLevel == 0 && !module.HasSourceModules && std::any_of(
            module.Functions.begin(), module.Functions.end(),
            [&](const TFunction& f) {
                return f.DebugInfo && !f.IsCoroutine && newSymIds.count(f.SymId);
            }))
    {
        debugInfo = std::make_unique<TLLVMDebugInfo>(*LModule, module);
    }
    int funcIdx = 0;
    std::vector<llvm::Function*> ctorFunctions;
    std::vector<llvm::Function*> dtorFunctions;
    for (const auto& f : module.Functions) {
        if (newSymIds.find(f.SymId) == newSymIds.end()) {
            funcIdx++;
            continue;
        }
        auto* function = LowerFunction(f, module, debugInfo.get());
        if (!shouldDefine(f.Name) && !function->isDeclaration()) {
            function->setLinkage(llvm::Function::AvailableExternallyLinkage);
        }
        if (funcIdx == module.ModuleConstructorFunctionId) {
            function->setLinkage(llvm::Function::InternalLinkage);
            ctorFunctions.push_back(function);
        } else if (funcIdx == module.ModuleDestructorFunctionId) {
            function->setLinkage(llvm::Function::InternalLinkage);
            dtorFunctions.push_back(function);
        }
        funcIdx++;
    }

    auto& ctx = *Ctx;
    auto* int32Ty = llvm::Type::getInt32Ty(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* voidFnTy = llvm::FunctionType::get(voidTy, false);
    // LLVM 21+: PointerType::get(ctx, AS) returns i8* for the given address space
    auto* voidFnPtrTy = llvm::PointerType::get(ctx, 0);
    auto* i8PtrTy = llvm::PointerType::get(ctx, 0);
    auto* ctorEntryTy = llvm::StructType::get(int32Ty, voidFnPtrTy, i8PtrTy);

    auto appendGlobalFuncList = [&](const char* globalName, const std::vector<llvm::Function*>& functions) {
        if (functions.empty()) {
            return;
        }

        std::vector<llvm::Constant*> newElems;
        newElems.reserve(functions.size());

        for (auto* fn : functions) {
            auto* prio = llvm::ConstantInt::get(int32Ty, 65535);
            auto* fnPtr = llvm::ConstantExpr::getBitCast(fn, voidFnPtrTy);
            auto* nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(i8PtrTy));
            newElems.push_back(llvm::ConstantStruct::get(ctorEntryTy, { prio, fnPtr, nullPtr }));
        }

        auto* existingGV = LModule->getNamedGlobal(globalName);
        if (!existingGV) {
            auto* arrTy = llvm::ArrayType::get(ctorEntryTy, newElems.size());
            auto* init = llvm::ConstantArray::get(arrTy, newElems);
            new llvm::GlobalVariable(*LModule, arrTy, /*isConstant*/false,
                                     llvm::GlobalValue::AppendingLinkage,
                                     init, globalName);
        } else {
            auto* existingArrTy = llvm::cast<llvm::ArrayType>(existingGV->getValueType());
            auto* existingInit = llvm::cast<llvm::ConstantArray>(existingGV->getInitializer());

            std::vector<llvm::Constant*> combined;
            combined.reserve(existingArrTy->getNumElements() + newElems.size());
            for (unsigned i = 0; i < existingArrTy->getNumElements(); ++i) {
                combined.push_back(existingInit->getOperand(i));
            }
            combined.insert(combined.end(), newElems.begin(), newElems.end());

            auto* newArrTy = llvm::ArrayType::get(ctorEntryTy, combined.size());
            auto* newInit = llvm::ConstantArray::get(newArrTy, combined);
            auto* newPtrTy = llvm::PointerType::get(ctx, 0);
            existingGV->mutateType(newPtrTy);
            existingGV->setInitializer(newInit);
        }
    };

    appendGlobalFuncList("llvm.global_ctors", ctorFunctions);
    appendGlobalFuncList("llvm.global_dtors", dtorFunctions);

    // Emit a sentinel global so the JS runtime can reliably detect coroutine modules
    // without heuristics. JS checks instance.exports.__qumir_is_coroutine !== undefined.
    const bool hasCoroutines = std::any_of(module.Functions.begin(), module.Functions.end(),
        [&](const NIR::TFunction& f) { return f.IsCoroutine && newSymIds.count(f.SymId); });
    if (hasCoroutines) {
        auto* i32Ty = llvm::Type::getInt32Ty(*Ctx);
        new llvm::GlobalVariable(*LModule, i32Ty, /*isConstant*/true,
            llvm::GlobalValue::ExternalLinkage,
            llvm::ConstantInt::get(i32Ty, 1),
            "__qumir_is_coroutine");
        EmitCoroutineRuntimeHelpers(*LModule, *Ctx);
    }

    if (debugInfo) {
        debugInfo->Finalize();
        debugInfo.reset();
    }

    if (Opts.LlvmBitcode) {
        for (size_t i = 0; i < Opts.LlvmBitcode->size(); ++i) {
            const auto& blob = (*Opts.LlvmBitcode)[i];
            auto parsed = llvm::parseBitcodeFile(
                llvm::MemoryBufferRef(
                    llvm::StringRef(blob.data(), blob.size()),
                    "source-module-bitcode-" + std::to_string(i)),
                *Ctx);
            if (!parsed) {
                throw std::runtime_error(
                    "failed to parse source module LLVM bitcode #" + std::to_string(i)
                    + ": " + llvm::toString(parsed.takeError()));
            }
            if (llvm::Linker::linkModules(*LModule, std::move(*parsed))) {
                throw std::runtime_error(
                    "failed to link source module LLVM bitcode #" + std::to_string(i));
            }
        }
    }

    // Coroutine passes must run before verification: pre-split coroutine IR
    // intentionally violates SSA dominance (values live across suspend points
    // are not yet spilled). coro-split inserts the frame spills that make the
    // IR valid. Verifying before the passes would reject well-formed coroutines.
    if (optLevel > 0) {
        Optimize(optLevel);
    } else if (hasCoroutines) {
        RunCoroutinePasses();
    }

    if (llvm::verifyModule(*LModule, &llvm::errs())) {
        llvm::errs() << "\n[LLVMCodeGen] Module verify failed. Dumping IR:\n";
        LModule->print(llvm::errs(), nullptr);
        throw std::runtime_error("LLVM verify failed");
    }

    auto out = std::make_unique<TLLVMModuleArtifacts>();
    out->Ctx = std::move(Ctx);
    out->Module = std::move(LModule);
    out->NativeCode = Opts.NativeCode;
    // Collect defined function names for tests without pulling in LLVM headers
    if (out->Module) {
        for (auto& F : *out->Module) {
            if (!F.isDeclaration() && !F.hasAvailableExternallyLinkage()) {
                out->FunctionNames.push_back(F.getName().str());
            }
        }
    }
    return out;
}

llvm::GlobalVariable* TLLVMCodeGen::EnsureSlotGlobal(int64_t sidx, NIR::TModule& module)
{
    if (sidx < 0) throw std::runtime_error("negative slot index");
    if (sidx >= (int64_t)ModuleSlots.size()) ModuleSlots.resize(sidx + 1, nullptr);
    if (!ModuleSlots[sidx]) {
        auto *slotTy = GetTypeById(module.GlobalTypes[sidx], module.Types, *Ctx);
        llvm::Constant* init = nullptr;
        if (slotTy->isFloatingPointTy()) {
            init = llvm::ConstantFP::get(slotTy, 0.0);
        } else if (slotTy->isPointerTy()) {
            init = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(slotTy));
        } else if (slotTy->isIntegerTy()) {
            init = llvm::ConstantInt::get(slotTy, 0, /*isSigned*/true);
        } else {
            init = llvm::UndefValue::get(slotTy);
        }
        auto *g = new llvm::GlobalVariable(*LModule, slotTy, /*isConstant*/false, llvm::GlobalValue::InternalLinkage,
            init,
            "slot" + std::to_string(sidx));
        ModuleSlots[sidx] = g;
    }
    return static_cast<llvm::GlobalVariable*>(ModuleSlots[sidx]);
}

llvm::Function* TLLVMCodeGen::LowerFunction(
    const TFunction& fun,
    NIR::TModule& module,
    TLLVMDebugInfo* debugInfo)
{
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    irb->SetCurrentDebugLocation({});
    if (fun.IsCoroutine) {
        return LowerCoroutineFunction(fun, module);
    }

    auto& ctx = *Ctx;
    auto lfun = LModule->getFunction(fun.Name);
    if (debugInfo) {
        debugInfo->BeginFunction(fun, *lfun);
    }
    // Function has already been registered in Emit pre-pass

    CurFun = std::make_unique<TFunState>();
    CurFun->Fun = &fun;
    CurFun->LFun = lfun;
    CurFun->TmpValues.resize(fun.NextTmpIdx, nullptr);

    std::vector<llvm::BasicBlock*> bbs; bbs.reserve(fun.Blocks.size());
    for (const auto& b : fun.Blocks) {
        auto *bb = llvm::BasicBlock::Create(ctx, "bb" + std::to_string(b.Label.Idx), lfun);
        bbs.push_back(bb);
        CurFun->LabelToBB[b.Label.Idx] = bb;
        CurFun->LabelExitBB[b.Label.Idx] = bb;
    }

    irb->SetInsertPoint(bbs.front());

    CurFun->Allocas.resize(fun.LocalTypes.size(), nullptr);
    for (int i = 0; i < (int)fun.LocalTypes.size(); ++i) {
        auto* localTy = GetTypeById(fun.LocalTypes[i], module.Types, ctx);
        auto* alloca = irb->CreateAlloca(localTy, nullptr, "local" + std::to_string(i));
        // TODO: remove it after removing str_release(nullptr)
        irb->CreateStore(llvm::Constant::getNullValue(localTy), alloca);
        CurFun->Allocas[i] = alloca;
    }

    for (int i = 0; i < (int)fun.ArgLocals.size(); ++i) {
        const auto& l = fun.ArgLocals[i];
        if (l.Idx < 0 || l.Idx >= (int)CurFun->Allocas.size()) {
            throw std::runtime_error("invalid argument local index");
        }
        auto* ptr = CurFun->Allocas[l.Idx];
        auto& arg = *lfun->getArg(i);
        irb->CreateStore(&arg, ptr);
    }

    if (debugInfo) {
        debugInfo->DeclareLocals(CurFun->Allocas, *bbs.front());
    }

    for (size_t i = 0; i < fun.Blocks.size(); ++i) {
        irb->SetInsertPoint(bbs[i]);
        LowerBlock(fun.Blocks[i], module, lfun, bbs, debugInfo);
    }
    irb->SetCurrentDebugLocation({});
    for (size_t i = 0; i < fun.Blocks.size(); ++i) {
        irb->SetInsertPoint(bbs[i]);
        for (const auto& instr : fun.Blocks[i].Phis) {
           AddIncomingPhiEdges(instr, module);
        }
    }
    return lfun;
}

llvm::Function* TLLVMCodeGen::LowerCoroutineFunction(const TFunction& fun, NIR::TModule& module) {
    auto& ctx = *Ctx;
    auto* lfun = LModule->getFunction(fun.Name);
    if (!lfun) {
        throw std::runtime_error("coroutine function was not predeclared: " + fun.Name);
    }
    lfun->setPresplitCoroutine();

    CurFun = std::make_unique<TFunState>();
    CurFun->Fun = &fun;
    CurFun->LFun = lfun;
    CurFun->TmpValues.resize(fun.NextTmpIdx, nullptr);

    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto* entry = llvm::BasicBlock::Create(ctx, "entry", lfun);
    auto* cleanup = llvm::BasicBlock::Create(ctx, "cleanup", lfun);
    auto* suspend = llvm::BasicBlock::Create(ctx, "suspend", lfun);
    auto* final = llvm::BasicBlock::Create(ctx, "final", lfun);
    std::vector<llvm::BasicBlock*> bbs; bbs.reserve(fun.Blocks.size());
    for (const auto& b : fun.Blocks) {
        auto* bb = llvm::BasicBlock::Create(ctx, "bb" + std::to_string(b.Label.Idx), lfun);
        bbs.push_back(bb);
        CurFun->LabelToBB[b.Label.Idx] = bb;
        CurFun->LabelExitBB[b.Label.Idx] = bb;
    }
    irb->SetInsertPoint(entry);

    auto* i1Ty = llvm::Type::getInt1Ty(ctx);
    auto* i8Ty = llvm::Type::getInt8Ty(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* ptrTy = llvm::PointerType::get(ctx, 0);
    auto* tokenTy = llvm::Type::getTokenTy(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto* nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
    auto* tokenNone = llvm::ConstantTokenNone::get(ctx);
    auto* resultTy = GetTypeById(fun.CoroutineResultTypeId, module.Types, ctx);
    auto* promise = resultTy->isVoidTy() ? nullptr : irb->CreateAlloca(resultTy, nullptr, "coro.promise");
    if (promise) {
        irb->CreateStore(llvm::Constant::getNullValue(resultTy), promise);
    }

    auto getFn = [&](llvm::StringRef name, llvm::FunctionType* type) {
        return LModule->getOrInsertFunction(name, type);
    };

    auto coroId = getFn("llvm.coro.id", llvm::FunctionType::get(
        tokenTy, {i32Ty, ptrTy, ptrTy, ptrTy}, false));
    auto coroSize = getFn("llvm.coro.size.i32", llvm::FunctionType::get(i32Ty, false));
    auto coroBegin = getFn("llvm.coro.begin", llvm::FunctionType::get(ptrTy, {tokenTy, ptrTy}, false));
    auto coroSuspend = getFn("llvm.coro.suspend", llvm::FunctionType::get(i8Ty, {tokenTy, i1Ty}, false));
#if LLVM_VERSION_MAJOR >= 22
    auto coroEnd = getFn("llvm.coro.end", llvm::FunctionType::get(voidTy, {ptrTy, i1Ty, tokenTy}, false));
#else
    auto coroEnd = getFn("llvm.coro.end", llvm::FunctionType::get(i1Ty,   {ptrTy, i1Ty, tokenTy}, false));
#endif
    auto coroFree = getFn("llvm.coro.free", llvm::FunctionType::get(ptrTy, {tokenTy, ptrTy}, false));
    auto coroPromise = getFn("llvm.coro.promise", llvm::FunctionType::get(ptrTy, {ptrTy, i32Ty, i1Ty}, false));
    auto coroDone = getFn("llvm.coro.done", llvm::FunctionType::get(i1Ty, {ptrTy}, false));
    auto coroResume = getFn("llvm.coro.resume", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    auto coroDestroy = getFn("llvm.coro.destroy", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    auto arrayCreateFn = getFn("array_create", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
    auto arrayDestroyFn = getFn("array_destroy", llvm::FunctionType::get(voidTy, {ptrTy}, false));

    auto* id = irb->CreateCall(coroId, {
        llvm::ConstantInt::get(i32Ty, 0),
        promise ? static_cast<llvm::Value*>(promise) : static_cast<llvm::Value*>(nullPtr),
        nullPtr,
        nullPtr
    }, "coro.id");
    auto* size = irb->CreateCall(coroSize, {}, "coro.size");
    auto* allocSize = irb->CreateZExt(size, i64Ty, "coro.alloc.size");
    auto* alloc = irb->CreateCall(arrayCreateFn, {allocSize}, "coro.alloc");
    auto* handle = irb->CreateCall(coroBegin, {id, alloc}, "coro.handle");

    CurFun->Allocas.resize(fun.LocalTypes.size(), nullptr);
    for (int i = 0; i < (int)fun.LocalTypes.size(); ++i) {
        auto* localTy = GetTypeById(fun.LocalTypes[i], module.Types, ctx);
        auto* alloca = irb->CreateAlloca(localTy, nullptr, "local" + std::to_string(i));
        irb->CreateStore(llvm::Constant::getNullValue(localTy), alloca);
        CurFun->Allocas[i] = alloca;
    }

    for (int i = 0; i < (int)fun.ArgLocals.size(); ++i) {
        const auto& l = fun.ArgLocals[i];
        if (l.Idx < 0 || l.Idx >= (int)CurFun->Allocas.size()) {
            throw std::runtime_error("invalid argument local index");
        }
        auto* ptr = CurFun->Allocas[l.Idx];
        auto& arg = *lfun->getArg(i);
        irb->CreateStore(&arg, ptr);
    }
    irb->CreateBr(bbs.front());

    auto suspendAfterStep = [&](llvm::BasicBlock* nextBB) {
        auto* s = irb->CreateCall(coroSuspend, {
            tokenNone,
            llvm::ConstantInt::getFalse(ctx)
        }, "coro.suspend");
        auto* sw = irb->CreateSwitch(s, suspend, 2);
        sw->addCase(llvm::ConstantInt::get(i8Ty, 0), nextBB);
        sw->addCase(llvm::ConstantInt::get(i8Ty, 1), cleanup);
    };

    auto castCoroutineResult = [&](llvm::Value* val, llvm::Type* expectedType) -> llvm::Value* {
        auto* actualTy = val->getType();
        if (actualTy == expectedType) {
            return val;
        }
        if (actualTy->isIntegerTy() && expectedType->isIntegerTy()) {
            return irb->CreateIntCast(val, expectedType, /*isSigned=*/true, "coro.ret.cast");
        }
        if (actualTy->isIntegerTy() && expectedType->isDoubleTy()) {
            return irb->CreateSIToFP(val, expectedType, "coro.ret.cast");
        }
        if (actualTy->isDoubleTy() && expectedType->isIntegerTy()) {
            return irb->CreateFPToSI(val, expectedType, "coro.ret.cast");
        }
        if (actualTy->isPointerTy() && expectedType->isIntegerTy()) {
            return irb->CreatePtrToInt(val, expectedType, "coro.ret.cast");
        }
        if (actualTy->isIntegerTy() && expectedType->isPointerTy()) {
            return irb->CreateIntToPtr(val, expectedType, "coro.ret.cast");
        }
        throw std::runtime_error("coroutine return type mismatch; pre-cast required");
    };

    auto wrapCoroFn = getFn("__qumir_wrap_coro",
        llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty}, false));

    auto lowerAwaitFuture = [&](const NIR::TInstr& instr, int awaitIndex) {
        assert(instr.Size() >= 1 && instr.Operands[0].Type == TOperand::EType::Tmp);
        llvm::Value* future = CurFun->TmpValues[instr.Operands[0].Tmp.Idx];

        auto futureAwaitReadyFn   = getFn("__qumir_future_await_ready",
            llvm::FunctionType::get(i1Ty,   {ptrTy},         false));
        auto futureAwaitSuspendFn = getFn("__qumir_future_await_suspend",
            llvm::FunctionType::get(ptrTy,  {ptrTy, ptrTy},  false));
        auto futureAwaitResumeFn  = getFn("__qumir_future_await_resume",
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy},  false));
        auto futureDestroyFn      = getFn("__qumir_future_destroy",
            llvm::FunctionType::get(voidTy, {ptrTy},          false));

        auto sfx = std::to_string(awaitIndex);
        auto* checkBB = llvm::BasicBlock::Create(ctx, "await.check."   + sfx, lfun);
        auto* suspBB  = llvm::BasicBlock::Create(ctx, "await.suspend."  + sfx, lfun);
        auto* afterBB = llvm::BasicBlock::Create(ctx, "after.await."    + sfx, lfun);

        irb->CreateBr(checkBB);

        irb->SetInsertPoint(checkBB);
        auto* ready = irb->CreateCall(futureAwaitReadyFn, {future}, "await.ready");
        irb->CreateCondBr(ready, afterBB, suspBB);

        irb->SetInsertPoint(suspBB);
        irb->CreateCall(futureAwaitSuspendFn, {future, handle});
        suspendAfterStep(checkBB);

        irb->SetInsertPoint(afterBB);
        if (instr.Dest.Idx >= 0) {
            const int outputTypeId = CurFun->Fun->GetTmpType(instr.Dest.Idx);
            auto* childResultTy = GetTypeById(outputTypeId, module.Types, ctx);
            auto* resultSlot = irb->CreateAlloca(childResultTy, nullptr, "await.result");
            auto futureAddressFn = getFn("__qumir_future_address",
                llvm::FunctionType::get(ptrTy, {ptrTy}, false));
            auto* childHandle = irb->CreateCall(futureAddressFn, {future}, "child.handle");
            auto* wrappedCoro = irb->CreateICmpNE(childHandle, nullPtr, "await.wrapped_coro");
            auto* wrappedBB = llvm::BasicBlock::Create(ctx, "await.wrapped." + sfx, lfun);
            auto* jsBB = llvm::BasicBlock::Create(ctx, "await.js." + sfx, lfun);
            auto* loadBB = llvm::BasicBlock::Create(ctx, "await.load." + sfx, lfun);
            irb->CreateCondBr(wrappedCoro, wrappedBB, jsBB);

            irb->SetInsertPoint(wrappedBB);
            auto* promisePtr  = irb->CreateCall(coroPromise, {
                childHandle,
                llvm::ConstantInt::get(i32Ty, 0),
                llvm::ConstantInt::getFalse(ctx)
            }, "child.promise");
            auto* wrappedResult = irb->CreateLoad(childResultTy, promisePtr, "child.result");
            irb->CreateStore(wrappedResult, resultSlot);
            irb->CreateCall(futureAwaitResumeFn, {future, nullPtr});
            irb->CreateBr(loadBB);

            irb->SetInsertPoint(jsBB);
            irb->CreateCall(futureAwaitResumeFn, {future, resultSlot});
            irb->CreateBr(loadBB);

            irb->SetInsertPoint(loadBB);
            auto* result = irb->CreateLoad(childResultTy, resultSlot, "await.result.value");
            CurFun->TmpValues[instr.Dest.Idx] = castCoroutineResult(result, childResultTy);
        } else {
            irb->CreateCall(futureAwaitResumeFn, {future, nullPtr});
        }
        irb->CreateCall(futureDestroyFn, {future});
    };

    int awaitIndex = 0;
    for (size_t i = 0; i < fun.Blocks.size(); ++i) {
        irb->SetInsertPoint(bbs[i]);
        for (const auto& phi : fun.Blocks[i].Phis) {
            if (irb->GetInsertBlock()->getTerminator()) {
                throw std::runtime_error("attempt to emit instruction after terminator");
            }
            EmitPhi(phi, module);
        }
        for (const auto& instr : fun.Blocks[i].Instrs) {
            if (irb->GetInsertBlock()->getTerminator()) {
                throw std::runtime_error("attempt to emit instruction after terminator");
            }
            if (instr.Op == "ret"_op) {
                if (promise && instr.Size() == 1) {
                    auto* retVal = GetOp(instr.Operands[0], module);
                    if (retVal->getType() != resultTy) {
                        retVal = castCoroutineResult(retVal, resultTy);
                    }
                    irb->CreateStore(retVal, promise);
                }
                irb->CreateBr(final);
            } else if (instr.Op == "await"_op) {
                lowerAwaitFuture(instr, awaitIndex++);
            } else if (instr.Op == "call"_op
                       && instr.Dest.Idx >= 0
                       && instr.Size() >= 1
                       && instr.Operands[0].Type == TOperand::EType::Imm) {
                // Call to a user coroutine: lower normally then wrap the raw
                // coro handle in ITypeErasedFuture* so await sees a uniform type.
                const int symId = static_cast<int>(instr.Operands[0].Imm.Value);
                const TFunction* childFun = nullptr;
                for (const auto& f : module.Functions) {
                    if (f.SymId == symId && f.IsCoroutine) { childFun = &f; break; }
                }
                LowerInstr(instr, module);
                if (childFun) {
                    auto* raw = CurFun->TmpValues[instr.Dest.Idx];
                    const int64_t resultSize = (childFun->CoroutineResultTypeId >= 0)
                        ? static_cast<int64_t>(module.Types.SizeInBytes(childFun->CoroutineResultTypeId))
                        : 0;
                    auto* wrapped = irb->CreateCall(wrapCoroFn,
                        {raw, llvm::ConstantInt::get(i64Ty, resultSize)},
                        "wrapped.coro");
                    CurFun->TmpValues[instr.Dest.Idx] = wrapped;
                }
            } else {
                LowerInstr(instr, module);
            }
        }
        // After await insertion, the insert block may be a newly created block
        // rather than bbs[i]. Keep LabelToBB as the block entry for jmp/cmp
        // targets, and record the real exit block separately for PHI edges.
        CurFun->LabelExitBB[fun.Blocks[i].Label.Idx] = irb->GetInsertBlock();
    }
    for (size_t i = 0; i < fun.Blocks.size(); ++i) {
        irb->SetInsertPoint(bbs[i]);
        for (const auto& phi : fun.Blocks[i].Phis) {
            AddIncomingPhiEdges(phi, module);
        }
    }

    irb->SetInsertPoint(final);
    auto* finalSuspend = irb->CreateCall(coroSuspend, {
        tokenNone,
        llvm::ConstantInt::getTrue(ctx)
    }, "coro.final.suspend");
    auto* finalSwitch = irb->CreateSwitch(finalSuspend, suspend, 2);
    finalSwitch->addCase(llvm::ConstantInt::get(i8Ty, 0), suspend);
    finalSwitch->addCase(llvm::ConstantInt::get(i8Ty, 1), cleanup);

    irb->SetInsertPoint(cleanup);
    auto* mem = irb->CreateCall(coroFree, {id, handle}, "coro.free");
    irb->CreateCall(arrayDestroyFn, {mem});
    irb->CreateBr(suspend);

    irb->SetInsertPoint(suspend);
    irb->CreateCall(coroEnd, {
        handle,
        llvm::ConstantInt::getFalse(ctx),
        tokenNone
    });
    irb->CreateRet(handle);

    return lfun;
}

void TLLVMCodeGen::LowerBlock(
    const TBlock& blk,
    NIR::TModule& module,
    llvm::Function*,
    std::vector<llvm::BasicBlock*>& orderedBBs,
    TLLVMDebugInfo* debugInfo)
{
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    irb->SetCurrentDebugLocation({});
    for (const auto& instr : blk.Phis) {
        if (irb->GetInsertBlock()->getTerminator()) {
            throw std::runtime_error("attempt to emit instruction after terminator");
        }
        EmitPhi(instr, module);
    }

    for (size_t i = 0; i < blk.Instrs.size(); ++i) {
        if (irb->GetInsertBlock()->getTerminator()) {
            throw std::runtime_error("attempt to emit instruction after terminator");
        }
        if (debugInfo) {
            irb->SetCurrentDebugLocation(debugInfo->GetLocation(
                i < blk.InstrDebugInfo.size() ? blk.InstrDebugInfo[i] : TInstrDebugInfo{}));
        }
        LowerInstr(blk.Instrs[i], module);
    }
    // Bounds checks split LLVM blocks; PHIs must refer to the actual exit.
    CurFun->LabelExitBB[blk.Label.Idx] = irb->GetInsertBlock();
}

llvm::Value* TLLVMCodeGen::GetOp(const TOperand& op, NIR::TModule& module)
{
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto& ctx = irb->getContext();
    auto i64 = llvm::Type::getInt64Ty(ctx);
    auto f64 = llvm::Type::getDoubleTy(ctx);
    auto lowStringTypeId = module.Types.Ptr(module.Types.I(EKind::I8));
    auto lowFloatTypeId = module.Types.I(EKind::F64);
    auto lowIntTypeId = module.Types.I(EKind::I64);
    auto lowBoolTypeId = module.Types.I(EKind::I1);

    switch (op.Type) {
        case TOperand::EType::Imm:
            if (op.Imm.TypeId == lowFloatTypeId) {
                return llvm::ConstantFP::get(f64, std::bit_cast<double>(op.Imm.Value));
            } else if (op.Imm.TypeId == lowIntTypeId) {
                return llvm::ConstantInt::get(i64, op.Imm.Value, true);
            } else if (module.Types.IsVector(op.Imm.TypeId) && op.Imm.Value == 0) {
                return llvm::Constant::getNullValue(GetTypeById(op.Imm.TypeId, module.Types, ctx));
            } else if (module.Types.IsFloat(op.Imm.TypeId)) {
                return llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx),
                    std::bit_cast<float>(static_cast<uint32_t>(op.Imm.Value)));
            } else if (module.Types.IsInteger(op.Imm.TypeId)) {
                auto* ty = GetTypeById(op.Imm.TypeId, module.Types, ctx);
                return llvm::ConstantInt::get(ty, op.Imm.Value, module.Types.IsSigned(op.Imm.TypeId));
            } else if (op.Imm.TypeId == lowStringTypeId) {
                // assume char* for now
                int id = (int)op.Imm.Value;
                if (id < 0 || id >= module.StringLiterals.size()) {
                    throw std::runtime_error("Invalid string literal id in outs");
                }
                if (id >= (int)StringLiterals.size()) {
                    StringLiterals.resize(id + 1, nullptr);
                }
                auto& str = StringLiterals[id];
                if (str) {
                    return str;
                }
                if (id == 0) {
                    // nullptr string
                    str = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(GetTypeById(lowStringTypeId, module.Types, ctx)));
                    return str;
                }

                str = irb->CreateGlobalString(module.StringLiterals[id], "strlit" + std::to_string(id));
                return StringLiterals[id] = str;
            } else if (op.Imm.TypeId == lowBoolTypeId) {
                return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), op.Imm.Value != 0 ? 1 : 0, false);
            } else {
                assert(op.Imm.TypeId >= 0 && "immediate TypeId must be assigned before LLVM lowering");
                throw std::runtime_error("unsupported immediate typeId: " + std::to_string(op.Imm.TypeId));
            }
        case TOperand::EType::Tmp: {
            if (op.Tmp.Idx < 0 || op.Tmp.Idx >= CurFun->TmpValues.size()) {
                throw std::runtime_error("invalid temporary index: " + std::to_string(op.Tmp.Idx));
            }
            if (!CurFun->TmpValues[op.Tmp.Idx]) {
                throw std::runtime_error("use of uninitialized temporary: " + std::to_string(op.Tmp.Idx));
            }
            return CurFun->TmpValues[op.Tmp.Idx];
        }
        default:
            throw std::runtime_error("unsupported operand type in ALU instruction");
    };
}

llvm::Value* TLLVMCodeGen::EmitPhi(const NIR::TPhi& instr, NIR::TModule& module)
{
    auto opcode = instr.Op;
    if (opcode != "phi"_op && opcode != "nop"_op) {
        throw std::runtime_error("EmitPhi called with non-phi instruction");
    }
    if (opcode == "nop"_op) {
        return nullptr;
    }

    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto& ctx = irb->getContext();
    llvm::Type* outputType = nullptr;
    int outputTypeId = -1;

    if (instr.Dest.Idx >= 0) {
        outputTypeId = CurFun->Fun->GetTmpType(instr.Dest.Idx);
        outputType = GetTypeById(outputTypeId, module.Types, ctx);
    }

    auto storeTmp = [&](llvm::Value* v){
        if (instr.Dest.Idx >= 0) {
            if (instr.Dest.Idx >= CurFun->TmpValues.size()) {
                CurFun->TmpValues.resize(instr.Dest.Idx + 1, nullptr);
            }
            CurFun->TmpValues[instr.Dest.Idx] = v;
        }
        return v;
    };

    auto operandCount = instr.Size();
    if (operandCount % 2 != 0 || operandCount < 2) {
        throw std::runtime_error("phi needs even number of operands >= 2");
    }

    auto phi = irb->CreatePHI(outputType, operandCount / 2, "phitmp");
    return storeTmp(phi);
}

void TLLVMCodeGen::AddIncomingPhiEdges(const NIR::TPhi& instr, NIR::TModule& module)
{
    if (instr.Op == "nop"_op) {
        return;
    }
    auto* value = CurFun->TmpValues[instr.Dest.Idx];
    auto* phi = llvm::cast<llvm::PHINode>(value);
    for (int i = 0; i < instr.Size() / 2; ++i) {
        auto value = GetOp(instr.Operands[2*i], module);
        auto label = instr.Operands[2*i+1].Label.Idx;
        llvm::BasicBlock* block = nullptr;
        if (auto it = CurFun->LabelExitBB.find(label); it != CurFun->LabelExitBB.end()) {
            block = it->second;
        } else if (auto it = CurFun->LabelToBB.find(label); it != CurFun->LabelToBB.end()) {
            block = it->second;
        }
        if (!block) {
            throw std::runtime_error("phi incoming label not found");
        }
        phi->addIncoming(value, block);
    }
}

void TLLVMCodeGen::EnsureVectorIndices(llvm::Value* indices, unsigned count) {
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto& ctx = irb->getContext();
    auto* bound = llvm::ConstantInt::get(indices->getType(), count);
    auto* valid = irb->CreateICmpULT(indices, bound, "index.valid");
    if (valid->getType()->isVectorTy()) {
        valid = irb->CreateAndReduce(valid);
    }
    auto* success = llvm::BasicBlock::Create(ctx, "index.ok", CurFun->LFun);
    auto* failure = llvm::BasicBlock::Create(ctx, "index.fail", CurFun->LFun);
    irb->CreateCondBr(valid, success, failure);

    irb->SetInsertPoint(failure);
    auto ensure = LModule->getOrInsertFunction("__ensure",
        llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
            {llvm::Type::getInt1Ty(ctx), llvm::PointerType::get(ctx, 0)}, false));
    auto* error = irb->CreateCall(ensure, {llvm::ConstantInt::getFalse(ctx),
        irb->CreateGlobalString("Vector index out of bounds")});
    error->addFnAttr(llvm::Attribute::NoReturn);
    irb->CreateUnreachable();

    irb->SetInsertPoint(success);
}

llvm::Value* TLLVMCodeGen::LowerVectorIndex(llvm::Value* vector, llvm::Value* index, llvm::Type* resultType) {
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto* type = llvm::dyn_cast<llvm::FixedVectorType>(vector->getType());
    if (!type || !index->getType()->isIntegerTy(64)) {
        throw std::runtime_error("Invalid LLVM vector index types");
    }
    EnsureVectorIndices(index, type->getNumElements());
    auto* value = irb->CreateExtractElement(vector, index, "element");
    if (resultType->isIntegerTy(1) && value->getType()->isIntegerTy(8)) {
        return irb->CreateTrunc(value, resultType, "element.bool");
    }
    if (value->getType() != resultType) {
        throw std::runtime_error("Vector index result type mismatch");
    }
    return value;
}

llvm::Value* TLLVMCodeGen::LowerVectorInstr(const TInstr& instr, TModule& module, int outputTypeId) {
    const auto opcode = instr.Op;
    auto* outputType = GetTypeById(outputTypeId, module.Types, *Ctx);
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto operandTypeId = [&](const TOperand& op) {
        return op.Type == TOperand::EType::Tmp
            ? CurFun->Fun->GetTmpType(op.Tmp.Idx)
            : op.Imm.TypeId;
    };
    if (opcode == "movemask"_op) {
        return PackVectorBits(*irb, TruthValue(*irb, GetOp(instr.Operands[0], module)), irb->getInt32Ty());
    }
    if (opcode == "all"_op || opcode == "any"_op) {
        auto* mask = TruthValue(*irb, GetOp(instr.Operands[0], module));
        return opcode == "all"_op
            ? irb->CreateAndReduce(mask)
            : irb->CreateOrReduce(mask);
    }
    if (auto* vector = llvm::dyn_cast_or_null<llvm::FixedVectorType>(outputType)) {
        switch (opcode) {
            case "select"_op:
                return irb->CreateSelect(TruthValue(*irb, GetOp(instr.Operands[0], module)),
                    GetOp(instr.Operands[1], module), GetOp(instr.Operands[2], module), "vselect");
            case "gather"_op: {
                auto* pointers = irb->CreateGEP(vector->getElementType(), GetOp(instr.Operands[0], module),
                    GetOp(instr.Operands[1], module), "gather.ptrs");
                return irb->CreateMaskedGather(vector, pointers, llvm::Align(1), nullptr, nullptr, "vgather");
            }
            case "shuffle"_op: {
                auto* source = GetOp(instr.Operands[0], module);
                if (!module.Types.IsVector(operandTypeId(instr.Operands[1]))) {
                    const auto& mask = module.VectorShuffleMasks.at(instr.Operands[1].Imm.Value);
                    return irb->CreateShuffleVector(source, mask, "vshuffle");
                }
                auto* indices = GetOp(instr.Operands[1], module);
                EnsureVectorIndices(indices, vector->getNumElements());
                llvm::Value* result = llvm::PoisonValue::get(vector);
                for (unsigned i = 0; i < vector->getNumElements(); ++i) {
                    auto* index = irb->CreateExtractElement(indices, i);
                    auto* element = irb->CreateExtractElement(source, index);
                    result = irb->CreateInsertElement(result, element, i, "vshuffle");
                }
                return result;
            }
            case "+"_op:
            case "-"_op:
            case "*"_op:
            case "&"_op:
            case "|"_op:
            case "^"_op:
            case "<<"_op:
            case ">>"_op:
            case "&&"_op:
            case "||"_op:
            case "<"_op:
            case ">"_op:
            case "<="_op:
            case ">="_op:
            case "=="_op:
            case "!="_op: {
                auto* left = GetOp(instr.Operands[0], module);
                auto* right = GetOp(instr.Operands[1], module);
                auto* operandVector = llvm::dyn_cast<llvm::FixedVectorType>(left->getType());
                if (!operandVector) {
                    operandVector = llvm::cast<llvm::FixedVectorType>(right->getType());
                }
                int elementTypeId = operandTypeId(instr.Operands[0]);
                if (module.Types.IsVector(elementTypeId)) {
                    elementTypeId = module.Types.UnderlyingType(elementTypeId);
                }
                return EmitVectorBinary(*irb, opcode, operandVector, left, right,
                    module.Types.IsSigned(elementTypeId));
            }
            case "!"_op:
            case "i2b"_op:
            case "f2b"_op:
                return irb->CreateZExt(TruthValue(*irb, GetOp(instr.Operands[0], module),
                    opcode == "!"_op), vector, "vbool");
            case "~"_op:
                return irb->CreateNot(GetOp(instr.Operands[0], module), "vnot");
            case "mov"_op:
            case "i2f"_op:
            case "f2i"_op: {
                auto* value = GetOp(instr.Operands[0], module);
                if (value->getType() == vector) {
                    return value;
                }
                const int sourceElement = module.Types.UnderlyingType(operandTypeId(instr.Operands[0]));
                const int destElement = module.Types.UnderlyingType(outputTypeId);
                const auto castOp = llvm::CastInst::getCastOpcode(value, module.Types.IsSigned(sourceElement),
                    vector, module.Types.IsSigned(destElement));
                return irb->CreateCast(castOp, value, vector, "vcast");
            }
            default:
                break;
        }
    }

    return nullptr;
}

llvm::Value* TLLVMCodeGen::LowerInstr(const NIR::TInstr& instr, NIR::TModule& module) {
    auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
    auto& ctx = irb->getContext();
    auto i64 = llvm::Type::getInt64Ty(ctx);
    auto f64 = llvm::Type::getDoubleTy(ctx);
    llvm::Type* outputType = nullptr;
    int outputTypeId = -1;
    int operandCount = instr.Size();

    if (instr.Dest.Idx >= 0) {
        outputTypeId = CurFun->Fun->GetTmpType(instr.Dest.Idx);
        outputType = GetTypeById(outputTypeId, module.Types, ctx);
    }

    auto opcode = instr.Op;
    auto llvmTypeName = [](llvm::Type* type) {
        if (!type) {
            return std::string("<null>");
        }
        std::string out;
        llvm::raw_string_ostream os(out);
        type->print(os);
        os.flush();
        return out;
    };
    auto typeId = [](auto idx, const auto& cache) -> int {
        if (idx < 0 || idx >= (int)cache.size()) {
            return -1;
        }
        return cache[idx];
    };
    auto operandTypeId = [&](const TOperand& op) -> int {
        switch (op.Type) {
            case TOperand::EType::Tmp:
                return typeId(op.Tmp.Idx, CurFun->Fun->TmpTypes);
            case TOperand::EType::Slot:
                return typeId(op.Slot.Idx, module.GlobalTypes);
            case TOperand::EType::Local:
                return typeId(op.Local.Idx, CurFun->Fun->LocalTypes);
            case TOperand::EType::Imm:
                return op.Imm.TypeId;
            case TOperand::EType::Label:
                return -1;
        }
        return -1;
    };
    auto printType = [&](std::ostream& out, int tid) {
        if (tid >= 0) {
            out << ",";
            module.Types.Print(out, tid);
        }
    };
    auto formatOperand = [&](const TOperand& op, bool isCall) {
        std::ostringstream out;
        switch (op.Type) {
            case TOperand::EType::Tmp:
                out << "tmp(" << op.Tmp.Idx;
                printType(out, typeId(op.Tmp.Idx, CurFun->Fun->TmpTypes));
                out << ")";
                break;
            case TOperand::EType::Slot:
                out << "slot(" << op.Slot.Idx;
                printType(out, typeId(op.Slot.Idx, module.GlobalTypes));
                out << ")";
                break;
            case TOperand::EType::Local:
                out << "local(" << op.Local.Idx;
                printType(out, typeId(op.Local.Idx, CurFun->Fun->LocalTypes));
                out << ")";
                break;
            case TOperand::EType::Label:
                out << "label(" << op.Label.Idx << ")";
                break;
            case TOperand::EType::Imm:
                if (isCall && module.SymIdToFuncIdx.find(op.Imm.Value) != module.SymIdToFuncIdx.end()) {
                    out << module.Functions[module.SymIdToFuncIdx.at(op.Imm.Value)].Name;
                } else if (isCall && module.SymIdToExtFuncIdx.find(op.Imm.Value) != module.SymIdToExtFuncIdx.end()) {
                    out << module.ExternalFunctions[module.SymIdToExtFuncIdx.at(op.Imm.Value)].Name;
                } else {
                    out << "imm(" << op.Imm.Value;
                    printType(out, op.Imm.TypeId);
                    out << ")";
                }
                break;
        }
        return out.str();
    };
    auto formatInstr = [&]() {
        std::ostringstream out;
        out << opcode.ToString() << " ";
        if (instr.Dest.Idx >= 0) {
            out << "tmp(" << instr.Dest.Idx;
            printType(out, outputTypeId);
            out << ") = ";
        }
        bool isCall = opcode == "call"_op || opcode == "await"_op;
        for (int i = 0; i < instr.Size(); ++i) {
            if (i > 0) {
                out << " ";
            }
            out << formatOperand(instr.Operands[i], isCall);
        }
        return out.str();
    };
    auto storeTmp = [&](llvm::Value* v){
        if (instr.Dest.Idx >= 0) {
            if (instr.Dest.Idx >= CurFun->TmpValues.size()) {
                CurFun->TmpValues.resize(instr.Dest.Idx + 1, nullptr);
            }
            CurFun->TmpValues[instr.Dest.Idx] = v;
        }
        return v;
    };

    auto cast = [&](llvm::Value* val, llvm::Type* expectedType, bool sourceSigned = true) -> llvm::Value* {
        auto* actualTy = val->getType();

        if (actualTy == expectedType) {
            return val;
        }

        const bool actualIsPtr = actualTy->isPointerTy();
        const bool expectedIsPtr = expectedType->isPointerTy();

        // for strings
        // TODO: remove me after changing string type to proper pointer type
        if (actualIsPtr && expectedType->isIntegerTy()) {
            return irb->CreatePtrToInt(val, expectedType, "cast");
        }
        if (actualTy->isIntegerTy() && expectedIsPtr) {
            return irb->CreateIntToPtr(val, expectedType, "cast");
        }
        if (actualTy->isIntegerTy() && expectedType->isIntegerTy()) {
            return irb->CreateIntCast(val, expectedType, sourceSigned, "cast");
        }
        if (actualTy->isIntegerTy() && expectedType->isFloatingPointTy()) {
            return sourceSigned
                ? irb->CreateSIToFP(val, expectedType, "cast")
                : irb->CreateUIToFP(val, expectedType, "cast");
        }

        throw std::runtime_error(
            "unexpected cast request while lowering IR instruction: " + formatInstr()
            + "; actual LLVM type: " + llvmTypeName(actualTy)
            + "; expected LLVM type: " + llvmTypeName(expectedType)
            + "; insert explicit cast earlier");
    };

    auto cmpInsr = [&](llvm::CmpInst::Predicate pred, llvm::Value* lhs, llvm::Value* rhs) -> llvm::Value* {
        auto i1v = irb->CreateCmp(pred, lhs, rhs, "cmprtmp");
        return i1v;
    };

    auto binOp = [&](llvm::Instruction::BinaryOps op) -> llvm::Value* {
        // TODO: cast after operation
        auto lhs = cast(GetOp(instr.Operands[0], module), outputType);
        auto rhs = cast(GetOp(instr.Operands[1], module), outputType);
        return irb->CreateBinOp(op, lhs, rhs, "bintmp");
    };

    if ((outputType && outputType->isVectorTy()) || opcode == "all"_op || opcode == "any"_op || opcode == "movemask"_op) {
        if (auto* value = LowerVectorInstr(instr, module, outputTypeId)) {
            return storeTmp(value);
        }
    }

    switch (opcode) {
        case "prefetch"_op: {
            auto* pointer = GetOp(instr.Operands[0], module);
            irb->CreateIntrinsic(llvm::Intrinsic::prefetch, {pointer->getType()},
                {pointer, irb->getInt32(0), irb->getInt32(3), irb->getInt32(1)});
            return nullptr;
        }

        case "+"_op:
        case "-"_op:
        case "*"_op:
        case '%'_op:
        case "/"_op: {
            if (outputType == nullptr) {
                throw std::runtime_error("arithmetic op needs a typed dest");
            }
            if (outputType->isFloatingPointTy()) {
                auto it = fbinOpMap.find(opcode);
                if (it == fbinOpMap.end()) throw std::runtime_error("unsupported fbinop opcode");
                return storeTmp(binOp(it->second));
            } else if (outputType->isIntegerTy()) {
                if (IsSignedIntegerType(module.Types, outputTypeId)) {
                    // signed integer ops
                    auto it = ibinOpMap.find(opcode);
                    if (it == ibinOpMap.end()) throw std::runtime_error("unsupported ibinop opcode");
                    return storeTmp(binOp(it->second));
                } else {
                    // unsigned integer ops
                    auto it = ubinOpMap.find(opcode);
                    if (it == ubinOpMap.end()) throw std::runtime_error("unsupported ubinop opcode");
                    return storeTmp(binOp(it->second));
                }
            } else if (outputType->isPointerTy()) {
                if (opcode != "+"_op && opcode != "-"_op) {
                    throw std::runtime_error("only + and - supported for pointer types");
                }
                auto lhs = GetOp(instr.Operands[0], module);
                auto rhs = GetOp(instr.Operands[1], module);
                if (opcode == "+"_op) {
                    auto gep = irb->CreateGEP(llvm::Type::getInt8Ty(ctx), lhs, rhs, "ptraddtmp");
                    return storeTmp(gep);
                } else if (opcode == "-"_op) {
                    auto negRhs = irb->CreateNeg(cast(rhs, rhs->getType()), "negtmp");
                    auto gep = irb->CreateGEP(llvm::Type::getInt8Ty(ctx), lhs, negRhs, "ptrsubtmp");
                    return storeTmp(gep);
                }
            } else {
                throw std::runtime_error("unsupported type for arithmetic op");
            }
        }
        case "&"_op:
        case "|"_op:
        case "^"_op:
        case "<<"_op:
        case ">>"_op: {
            if (!outputType || !outputType->isIntegerTy()) {
                throw std::runtime_error("bitwise op needs an integer typed dest");
            }

            auto lhs = cast(GetOp(instr.Operands[0], module), outputType);
            auto rhs = cast(GetOp(instr.Operands[1], module), outputType);

            switch (opcode) {
                case "&"_op:
                    return storeTmp(irb->CreateAnd(lhs, rhs, "bandtmp"));
                case "|"_op:
                    return storeTmp(irb->CreateOr(lhs, rhs, "bortmp"));
                case "^"_op:
                    return storeTmp(irb->CreateXor(lhs, rhs, "bxortmp"));
                case "<<"_op:
                case ">>"_op: {
                    unsigned bitWidth = outputType->getIntegerBitWidth();
                    auto mask = llvm::ConstantInt::get(outputType, bitWidth - 1);
                    rhs = irb->CreateAnd(rhs, mask, "shiftmasktmp");
                    if (opcode == "<<"_op) {
                        return storeTmp(irb->CreateShl(lhs, rhs, "shltmp"));
                    }
                    if (IsSignedIntegerType(module.Types, outputTypeId)) {
                        return storeTmp(irb->CreateAShr(lhs, rhs, "ashrtmp"));
                    }
                    return storeTmp(irb->CreateLShr(lhs, rhs, "lshrtmp"));
                }
            }
            throw std::runtime_error("unsupported bitwise opcode");
        }
        // Relational / equality operators: produce i64 0/1
        case "<"_op:
        case "<="_op:
        case ">"_op:
        case ">="_op:
        case "=="_op:
        case "!="_op: {
            // Output type is i1 (bool)
            auto lhs = GetOp(instr.Operands[0], module);
            auto rhs = GetOp(instr.Operands[1], module);
            if (lhs->getType()->isFloatingPointTy() || lhs->getType()->isIntegerTy()) {
                rhs = cast(rhs, lhs->getType());
                const int lhsTypeId = operandTypeId(instr.Operands[0]);
                const bool isSigned = lhsTypeId < 0 || IsSignedIntegerType(module.Types, lhsTypeId);
                const auto pred = ComparisonPredicate(opcode, lhs->getType()->isFloatingPointTy(), isSigned);
                return storeTmp(cmpInsr(pred, lhs, rhs));
            } else if (lhs->getType()->isPointerTy()) {
                if (opcode != "=="_op && opcode != "!="_op) {
                    throw std::runtime_error("only == and != supported for pointer comparison");
                }
                auto pred = opcode == "=="_op ? llvm::CmpInst::ICMP_EQ : llvm::CmpInst::ICMP_NE;
                return storeTmp(cmpInsr(pred, lhs, cast(rhs, lhs->getType())));
            } else {
                throw std::runtime_error("unsupported type for comparison");
            }
        }
        case "!"_op: {
            if (!outputType || !outputType->isIntegerTy(1)) {
                throw std::runtime_error("logical not output type must be i1");
            }
            return storeTmp(TruthValue(*irb, GetOp(instr.Operands[0], module), true));
        }
        case "~"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (!outputType || !outputType->isIntegerTy()) {
                throw std::runtime_error("bitwise not output type must be integer");
            }
            return storeTmp(irb->CreateNot(cast(v, outputType), "bnottmp"));
        }
        case "neg"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (outputType->isFloatingPointTy()) {
                return storeTmp(irb->CreateFNeg(cast(v, outputType), "fnegtmp"));
            } else if (outputType->isIntegerTy()) {
                return storeTmp(irb->CreateNeg(cast(v, outputType), "inegtmp"));
            }
        }
        case "lde"_op: {
            // tmp = *ptr
            auto ptr = GetOp(instr.Operands[0], module);
            auto val = irb->CreateAlignedLoad(outputType, ptr,
                VectorMemoryAlign(module.Types, outputTypeId), "ldtmp");
            return storeTmp(val);
            break;
        }
        case "salloc"_op: {
            // TInstr: Dest = result ptr, Operands[0] = size imm (raw IR layout, not compiled VM layout)
            int64_t sizeBytes = instr.Operands[0].Imm.Value;
            auto* allocaTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx), sizeBytes);

            // WebAssembly lowers alloca at its instruction site into stack-pointer
            // movement. If salloc is emitted in a loop, O0 grows the stack on every
            // iteration. Place these fixed-size temporaries in the entry block so
            // each IR temporary has one stack slot per function invocation.
            llvm::IRBuilderBase::InsertPoint savedIp = irb->saveIP();
            auto* entry = &CurFun->LFun->getEntryBlock();
            auto insertBefore = entry->getFirstNonPHIOrDbgOrAlloca();
            if (insertBefore != entry->end()) {
                irb->SetInsertPoint(entry, insertBefore);
            } else {
                irb->SetInsertPoint(entry);
            }
            auto* alloca = irb->CreateAlloca(allocaTy, nullptr, "salloc");
            irb->restoreIP(savedIp);

            irb->CreateMemSet(alloca,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), 0),
                llvm::ConstantInt::get(i64, sizeBytes, false),
                llvm::MaybeAlign(1));
            return storeTmp(alloca);
        }
        case "ste"_op: {
            // *ptr = tmp
            auto ptr = GetOp(instr.Operands[0], module);
            auto value = GetOp(instr.Operands[1], module);
            llvm::Type* storeType = value->getType();
            const int ptrTypeId = operandTypeId(instr.Operands[0]);
            if (module.Types.IsPointer(ptrTypeId)) {
                storeType = GetTypeById(module.Types.UnderlyingType(ptrTypeId), module.Types, ctx);
            }
            const int sourceTypeId = operandTypeId(instr.Operands[1]);
            const bool sourceSigned = sourceTypeId < 0 || IsSignedIntegerType(module.Types, sourceTypeId);
            irb->CreateAlignedStore(cast(value, storeType, sourceSigned), ptr,
                VectorMemoryAlign(module.Types, sourceTypeId));
            return nullptr;
            break;
        }
        case "copy"_op: {
            if (operandCount != 3) {
                throw std::runtime_error("copy needs dst, src and size operands");
            }
            auto dst = GetOp(instr.Operands[0], module);
            auto src = GetOp(instr.Operands[1], module);
            if (dst->getType()->isPointerTy() && src->getType()->isStructTy()) {
                irb->CreateStore(src, dst);
                return nullptr;
            }
            llvm::Value* size = nullptr;
            if (instr.Operands[2].Type == TOperand::EType::Imm && instr.Operands[2].Imm.TypeId < 0) {
                size = llvm::ConstantInt::get(i64, instr.Operands[2].Imm.Value, false);
            } else {
                size = cast(GetOp(instr.Operands[2], module), i64);
            }
            irb->CreateMemCpy(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), size);
            return nullptr;
        }
        case "load"_op: {
            if (operandCount != 1 || instr.Dest.Idx < 0) throw std::runtime_error("load needs 1 operand and a dest");
            if (instr.Operands[0].Type == TOperand::EType::Slot) {
                auto idx = instr.Operands[0].Slot.Idx;
                auto g = EnsureSlotGlobal(idx, module);
                auto val = irb->CreateLoad(outputType, g, "loadtmp");
                return storeTmp(val);
            } else if (instr.Operands[0].Type == TOperand::EType::Local) {
                auto lidx = instr.Operands[0].Local.Idx;
                if (lidx < 0 || lidx >= CurFun->Allocas.size()) throw std::runtime_error("invalid local index");
                auto ptr = CurFun->Allocas[lidx];
                auto val = irb->CreateLoad(ptr->getAllocatedType(), ptr, "loadtmp");
                const bool localSigned = IsSignedIntegerType(module.Types, CurFun->Fun->LocalTypes[lidx]);
                return storeTmp(cast(val, outputType, localSigned));
            } else {
                throw std::runtime_error("load operand must be slot or local");
            }
            return nullptr;
        }
        case "lea"_op: {
            // like load byt get address of slot/local
            if (operandCount != 1 || instr.Dest.Idx < 0) throw std::runtime_error("lea needs 1 operand and a dest");
            if (instr.Operands[0].Type == TOperand::EType::Slot) {
                auto idx = instr.Operands[0].Slot.Idx;
                auto g = EnsureSlotGlobal(idx, module);
                return storeTmp(g);
            } else if (instr.Operands[0].Type == TOperand::EType::Local) {
                auto lidx = instr.Operands[0].Local.Idx;
                if (lidx < 0 || lidx >= CurFun->Allocas.size()) throw std::runtime_error("invalid local index");
                auto ptr = CurFun->Allocas[lidx];
                return storeTmp(ptr);
            } else {
                throw std::runtime_error("lea operand must be slot or local");
            }
            return nullptr;
            break;
        }
        case "stre"_op: {
            if (operandCount != 2)  throw std::runtime_error("store needs 2 operands");
            if (instr.Operands[0].Type == TOperand::EType::Slot) {
                auto idx = instr.Operands[0].Slot.Idx;
                auto g = EnsureSlotGlobal(idx, module);
                auto value = GetOp(instr.Operands[1], module);
                auto* valTy = g->getValueType();
                irb->CreateStore(cast(value, valTy), g);
            } else if (instr.Operands[0].Type == TOperand::EType::Local) {
                auto lidx = instr.Operands[0].Local.Idx;
                if (lidx < 0 || lidx >= CurFun->Allocas.size()) throw std::runtime_error("invalid local index");
                auto ptr = CurFun->Allocas[lidx];
                auto* localTy = ptr->getAllocatedType();
                auto value = GetOp(instr.Operands[1], module);
                if (localTy->isStructTy() && !value->getType()->isStructTy()) {
                    auto size = llvm::ConstantInt::get(i64,
                        LModule->getDataLayout().getTypeAllocSize(localTy), false);
                    irb->CreateMemCpy(ptr, llvm::MaybeAlign(8), value, llvm::MaybeAlign(8), size);
                } else {
                    irb->CreateStore(cast(value, localTy), ptr);
                }
            } else {
                throw std::runtime_error("store first operand must be slot or local");
            }
            return nullptr;
        }
        case "ret"_op: {
            if (operandCount > 0) {
                auto val = GetOp(instr.Operands[0], module);
                auto* need = CurFun->LFun->getFunctionType()->getReturnType();
                if (val->getType() != need) {
                    if (need->isStructTy() && val->getType()->isPointerTy()) {
                        // Bridge address-backed struct values to the LLVM value ABI.
                        val = irb->CreateLoad(need, val, "retstruct");
                    } else {
                        throw std::runtime_error("return type mismatch; pre-cast required");
                    }
                }
                irb->CreateRet(val);
            } else {
                irb->CreateRetVoid();
            }
            return nullptr;
        }
        case "i2b"_op:
        case "f2b"_op: {
            return storeTmp(TruthValue(*irb, GetOp(instr.Operands[0], module)));
        }
        case "i2f"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (!outputType) throw std::runtime_error("cast/mov needs typed dest");
            const int sourceTypeId = operandTypeId(instr.Operands[0]);
            const bool sourceSigned = sourceTypeId < 0 || IsSignedIntegerType(module.Types, sourceTypeId);
            return storeTmp(sourceSigned
                ? irb->CreateSIToFP(v, outputType, "i2f")
                : irb->CreateUIToFP(v, outputType, "u2f"));
        }
        case "f2i"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (!outputType) throw std::runtime_error("cast/mov needs typed dest");
            return storeTmp(IsSignedIntegerType(module.Types, outputTypeId)
                ? irb->CreateFPToSI(v, outputType, "f2i")
                : irb->CreateFPToUI(v, outputType, "f2u"));
        }
        case "bitcast"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (!outputType) {
                throw std::runtime_error("bitcast needs typed dest");
            }
            if (v->getType()->isPointerTy() && outputType->isIntegerTy()) {
                return storeTmp(irb->CreatePtrToInt(v, outputType, "bitcast"));
            }
            if (v->getType()->isIntegerTy() && outputType->isPointerTy()) {
                return storeTmp(irb->CreateIntToPtr(v, outputType, "bitcast"));
            }
            return storeTmp(irb->CreateBitCast(v, outputType, "bitcast"));
        }
        case "mov"_op: {
            auto v = GetOp(instr.Operands[0], module);
            if (!outputType) throw std::runtime_error("cast/mov needs typed dest");
            if (v->getType() != outputType) {
                auto sourceType = v->getType();
                if (sourceType->isIntegerTy() && outputType->isIntegerTy()) {
                    const int sourceTypeId = operandTypeId(instr.Operands[0]);
                    const bool sourceSigned = sourceTypeId < 0 || module.Types.IsSigned(sourceTypeId);
                    v = irb->CreateIntCast(v, outputType, sourceSigned, "movcast");
                } else if (sourceType->isPointerTy() && outputType->isIntegerTy()) {
                    v = irb->CreatePtrToInt(v, outputType, "ptrtoint");
                } else if (sourceType->isIntegerTy() && outputType->isPointerTy()) {
                    v = irb->CreateIntToPtr(v, outputType, "inttoptr");
                } else if (sourceType->isPointerTy() && outputType->isPointerTy()) {
                    v = irb->CreateBitCast(v, outputType, "ptrcast");
                } else {
                    throw std::runtime_error("mov type mismatch");
                }
            }
            return storeTmp(v);
        }
        case "index"_op: {
            return storeTmp(LowerVectorIndex(GetOp(instr.Operands[0], module),
                GetOp(instr.Operands[1], module), outputType));
        }
        case "arg"_op: {
            if (operandCount != 1) throw std::runtime_error("arg needs 1 operand");
            auto v = GetOp(instr.Operands[0], module);
            CurFun->PendingArgs.push_back(v);
            return nullptr;
        }
        case "await"_op:
            throw std::runtime_error("await reached regular LLVM codegen before coroutine lowering is implemented: " + formatInstr());
        case "call"_op: {
            // IR: call has optional Dest (required only for non-void) and one operand: Imm(symId)
            if (operandCount < 1) throw std::runtime_error("call needs callee operand");
            if (instr.Operands[0].Type != TOperand::EType::Imm) throw std::runtime_error("call callee must be Imm(symId)");
            const int calleeSymId = static_cast<int>(instr.Operands[0].Imm.Value);
            llvm::FunctionCallee callee;
            auto it = SymIdToLFun.find(calleeSymId);
            if (it != SymIdToLFun.end()) {
                // internal function
                callee = it->second;
            }
            if (!callee) {
                auto jt = module.SymIdToExtFuncIdx.find(calleeSymId);
                if (jt != module.SymIdToExtFuncIdx.end()) {
                    auto& extFun = module.ExternalFunctions[jt->second];
                    if (auto bit = BuiltinIntrinsics.find(extFun.Name);
                        bit != BuiltinIntrinsics.end())
                    {
                        return storeTmp(EmitBuiltinIntrinsicCall(
                            *irb,
                            bit->second,
                            CurFun->PendingArgs,
                            cast,
                            *TM));
                    }
                    EAbiArch arch = ModuleAbiArch(*LModule);
                    if (arch != EAbiArch::Other) {
                        auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
                        return storeTmp(EmitCoercedExternalCall(
                            *irb, *LModule, module.Types, extFun, arch, CurFun->PendingArgs, cast));
                    }
                    std::vector<llvm::Type*> paramTys;
                    for (int pid : extFun.ArgTypes) {
                        paramTys.push_back(GetTypeById(pid, module.Types, ctx));
                    }
                    auto* fty = llvm::FunctionType::get(
                        GetTypeById(extFun.ReturnTypeId, module.Types, ctx), paramTys, /*isVarArg=*/false);
                    callee = LModule->getOrInsertFunction(extFun.MangledName, fty);
                }
            }

            if (!callee) {
                throw std::runtime_error("call target function not found: " + std::to_string(calleeSymId));
            }

            auto* retTy = callee.getFunctionType()->getReturnType();
            // Marshal arguments collected by 'arg'
            auto* irb = static_cast<llvm::IRBuilder<>*>(BuilderBase.get());
            std::vector<llvm::Value*> args;
            for (int i = 0; i < (int)CurFun->PendingArgs.size(); ++i) {
                if (i >= (int)callee.getFunctionType()->getNumParams()) {
                    throw std::runtime_error("too many call arguments while lowering IR instruction: " + formatInstr());
                }
                auto* paramTy = callee.getFunctionType()->getParamType(i);
                auto* val = CurFun->PendingArgs[i];
                if (paramTy->isStructTy() && val->getType()->isPointerTy()) {
                    val = irb->CreateLoad(paramTy, val, "argstruct");
                } else {
                    val = cast(val, paramTy);
                }
                args.push_back(val);
            }
            if (args.size() != callee.getFunctionType()->getNumParams()) {
                throw std::runtime_error("wrong call argument count while lowering IR instruction: " + formatInstr());
            }
            CurFun->PendingArgs.clear();
            auto makeCallWithAttrs = [&](auto name) -> llvm::CallInst* {
                llvm::CallInst* ci;
                if constexpr (std::is_same_v<decltype(name), const char*>) {
                    ci = irb->CreateCall(callee, args, name);
                } else {
                    ci = irb->CreateCall(callee, args);
                }
                return ci;
            };
            if (retTy->isVoidTy()) {
                // Void call: emit and produce no tmp
                makeCallWithAttrs(nullptr);
                return nullptr;
            } else {
                if (instr.Dest.Idx < 0) throw std::runtime_error("call needs a destination tmp");
                auto call = makeCallWithAttrs("calltmp");
                return storeTmp(call);
            }
        }
        case "jmp"_op: {
            if (operandCount != 1) throw std::runtime_error("jmp needs 1 operand");
            if (!CurFun) throw std::runtime_error("jmp not in function");
            if (instr.Operands[0].Type != TOperand::EType::Label) throw std::runtime_error("jmp operand must be label");
            int64_t lab = instr.Operands[0].Label.Idx;
            auto it = CurFun->LabelToBB.find(lab);
            if (it == CurFun->LabelToBB.end()) throw std::runtime_error("jmp target label not found");
            irb->CreateBr(it->second);
            return nullptr;
        }
        case "cmp"_op: {
            if (operandCount != 3) throw std::runtime_error("cmp needs 3 operands");
            auto condV = GetOp(instr.Operands[0], module);
            if (instr.Operands[1].Type != TOperand::EType::Label || instr.Operands[2].Type != TOperand::EType::Label) {
                throw std::runtime_error("cmp branch targets must be labels");
            }
            int64_t tLab = instr.Operands[1].Label.Idx;
            int64_t fLab = instr.Operands[2].Label.Idx;
            auto itT = CurFun->LabelToBB.find(tLab);
            auto itF = CurFun->LabelToBB.find(fLab);
            if (itT == CurFun->LabelToBB.end() || itF == CurFun->LabelToBB.end()) throw std::runtime_error("cmp branch target not found");
            llvm::Value* cmpNZ = nullptr;
            if (condV->getType()->isFloatingPointTy()) {
                auto zero = llvm::ConstantFP::get(condV->getType(), 0.0);
                cmpNZ = irb->CreateFCmpUNE(condV, zero, "cmptmp");
            } else {
                auto zero = llvm::ConstantInt::get(condV->getType(), 0, true);
                cmpNZ = irb->CreateICmpNE(condV, zero, "cmptmp");
            }
            if (itT->second == itF->second) {
                irb->CreateBr(itT->second);
            } else {
                irb->CreateCondBr(cmpNZ, itT->second, itF->second);
            }
            return nullptr;
        }
    };
    std::cerr << "LLVMCodeGen: unhandled instruction: '" << instr.Op.ToString() << "'\n";
    return nullptr;
}

void TLLVMCodeGen::CreateTargetMachine() {
    auto triple = LModule->getTargetTriple();
    std::string errStr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, errStr);
    if (!target) {
        throw std::runtime_error(std::string("lookupTarget failed: ") + errStr);
    }
    llvm::TargetOptions opt;
    auto RM = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
    std::string cpu = "generic";
    std::string features;
    if (Opts.NativeCode) {
        auto [nativeCpu, nativeFeatures] = GetNativeCpuAndFeatures();
        if (!nativeCpu.empty()) {
            cpu = nativeCpu;
        }
        features = std::move(nativeFeatures);
    }
    TM.reset(target->createTargetMachine(triple, cpu, features, opt, RM));

    LModule->setDataLayout(TM->createDataLayout());
}

void TLLVMCodeGen::RunCoroutinePasses() {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(/*TM=*/nullptr); // coro passes are IR-level, no TM needed
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager CoroMPM;
    if (auto err = PB.parsePassPipeline(CoroMPM, "coro-early,coro-split,coro-elide,coro-cleanup")) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << err;
        os.flush();
        throw std::runtime_error("failed to build coroutine pass pipeline: " + msg);
    }
    CoroMPM.run(*LModule, MAM);
}

void TLLVMCodeGen::Optimize(int optLevel) {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB(TM.get());
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager CoroMPM;
    if (auto err = PB.parsePassPipeline(CoroMPM, "coro-early,coro-split,coro-elide,coro-cleanup")) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << err;
        os.flush();
        throw std::runtime_error("failed to build LLVM coroutine pass pipeline: " + msg);
    }
    CoroMPM.run(*LModule, MAM);

    auto OL = llvm::OptimizationLevel::O0;
    switch (optLevel) {
        case 0: OL = llvm::OptimizationLevel::O0; break;
        case 1: OL = llvm::OptimizationLevel::O1; break;
        case 2: OL = llvm::OptimizationLevel::O2; break;
        case 3: OL = llvm::OptimizationLevel::O3; break;
        case 4: OL = llvm::OptimizationLevel::Oz; break;
        default: OL = llvm::OptimizationLevel::O2; break;
    }
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OL);
    MPM.run(*LModule, MAM);
}

void TLLVMCodeGen::PrintFunction(int symId, std::ostream& os) const {
    auto it = SymIdToLFun.find(symId);
    if (it == SymIdToLFun.end()) {
        os << "[PrintFunction] function not found by SymId " << symId << "\n";
        return;
    }
    auto* f = it->second;
    if (!f) {
        os << "[PrintFunction] function pointer is null for SymId " << symId << "\n";
        return;
    }
    std::string str;
    llvm::raw_string_ostream rso(str);
    f->print(rso);
    rso.flush();
    os << str << "\n";
}

void TLLVMModuleArtifacts::PrintModule(std::ostream& os) const {
    if (!Module) {
        os << "[PrintModule] module is null\n";
        return;
    }
    std::string str;
    llvm::raw_string_ostream rso(str);
    Module->print(rso, nullptr);
    rso.flush();
    os << str << "\n";
}

void TLLVMModuleArtifacts::GenerateBitcode(std::ostream& os) const {
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream stream(buffer);
    llvm::WriteBitcodeToFile(*Module, stream);
    os.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}

} // namespace NQumir::NCodeGen
