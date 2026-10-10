#include "vmcompiler.h"
#include <qumir/align.h>
#include <qumir/ir/type.h>
#include <qumir/ir/vminstr.h>
#include <qumir/ir/passes/transforms/pipeline.h>

#include <cassert>
#include <algorithm>
#include <bit>
#include <iostream>
#include <iomanip>
#include <dlfcn.h>
#include <limits>
#include <unordered_set>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

namespace {

struct TPackedFunction : public NFFI::IFunction {
    using TPacked = uint64_t(*)(const uint64_t* args, size_t argCount);

    explicit TPackedFunction(TPacked packed)
        : Packed(packed)
    { }

    uint64_t operator() (const uint64_t* args, size_t argCount) override {
        return Packed(args, argCount);
    }

    TPacked Packed;
};

// SysV register class of a struct (interpreted per target ABI inside the FFI):
// >16 bytes is Memory; otherwise each eightbyte is SSE only when every field
// overlapping it is floating point.
NFFI::EStructKind ClassifyStruct(int typeId, const TTypeTable& tt, size_t size) {
    if (size == 0 || size > 16) {
        return NFFI::EStructKind::Memory;
    }
    bool sse[2] = {true, true};
    size_t offset = 0;
    for (int fieldType : tt.GetStructFields(typeId)) {
        size_t fieldSize = static_cast<size_t>(tt.SizeInBytes(fieldType));
        if (fieldSize == 0) {
            continue;
        }
        offset = (offset + fieldSize - 1) / fieldSize * fieldSize; // natural alignment
        bool isFloat = tt.IsFloat(fieldType);
        for (size_t eb = offset / 8; eb <= (offset + fieldSize - 1) / 8 && eb < 2; ++eb) {
            if (!isFloat) {
                sse[eb] = false;
            }
        }
        offset += fieldSize;
    }
    if (size <= 8) {
        return sse[0] ? NFFI::EStructKind::Sse : NFFI::EStructKind::Int;
    }
    if (sse[0] && sse[1]) {
        return NFFI::EStructKind::SseSse;
    }
    if (sse[0]) {
        return NFFI::EStructKind::SseInt;
    }
    if (sse[1]) {
        return NFFI::EStructKind::IntSse;
    }
    return NFFI::EStructKind::IntInt;
}

bool Is128BitInteger(const TTypeTable& tt, int typeId) {
    if (typeId < 0) {
        return false;
    }
    auto kind = tt.GetKind(typeId);
    return kind == EKind::I128 || kind == EKind::U128;
}

int OperandType(const TFunction& function, const TModule& module, const TOperand& operand) {
    switch (operand.Type) {
        case TOperand::EType::Tmp: return function.GetTmpType(operand.Tmp.Idx);
        case TOperand::EType::Imm: return operand.Imm.TypeId;
        case TOperand::EType::Local: return function.LocalTypes.at(operand.Local.Idx);
        case TOperand::EType::Slot: return module.GlobalTypes.at(operand.Slot.Idx);
        case TOperand::EType::Label: return -1;
    }
    return -1;
}

uint8_t VectorFormat(const TTypeTable& types, int vectorType) {
    const int elementType = types.UnderlyingType(vectorType);
    const auto elementSize = static_cast<unsigned>(types.SizeInBytes(elementType));
    const auto count = static_cast<unsigned>(types.VectorSize(vectorType));
    if ((!types.IsInteger(elementType) && !types.IsFloat(elementType))
        || !std::has_single_bit(elementSize) || elementSize > 8
        || !std::has_single_bit(count) || count > 32)
    {
        throw std::runtime_error("Unsupported VM vector shape");
    }
    return TVMInstr::MakeFormat(std::countr_zero(elementSize), std::countr_zero(count));
}

int RegisterAlignment(const TTypeTable& types, int typeId) {
    return Is128BitInteger(types, typeId)
        ? 16
        : 8;
}

EKind ClassifyKind(int typeId, const TTypeTable& tt, NFFI::EStructKind& structKind) {
    EKind kind = tt.GetKind(typeId);
    structKind = (kind == EKind::Struct)
        ? ClassifyStruct(typeId, tt, static_cast<size_t>(tt.SizeInBytes(typeId)))
        : NFFI::EStructKind::None;
    return kind;
}

} // namespace

bool TVMCompiler::CompileVectorInstruction(const TFunction& function, const TInstr& instr, TVMInstr& out) {
    const auto& types = Module.Types;
    int vectorType = function.GetTmpType(instr.Dest.Idx);
    if (!types.IsVector(vectorType)) {
        vectorType = -1;
    }
    for (size_t i = 0; i < instr.OperandCount; ++i) {
        const auto& operand = instr.Operands[i];
        const int typeId = OperandType(function, Module, operand);
        if (types.IsVector(typeId)) {
            vectorType = typeId;
            if (operand.Type == TOperand::EType::Slot) {
                throw std::runtime_error("VM vector globals are not supported yet");
            }
        }
    }
    if (instr.Op == "ste"_op && types.IsPointer(OperandType(function, Module, instr.Operands[0]))) {
        const int pointee = types.UnderlyingType(OperandType(function, Module, instr.Operands[0]));
        if (types.IsVector(pointee)) {
            vectorType = pointee;
        }
    }
    if (vectorType < 0) {
        return false;
    }
    out.Format = VectorFormat(types, vectorType);
    switch (instr.Op) {
        case "all"_op:
        case "any"_op:
        case "movemask"_op: {
            const bool packed = instr.Op == "movemask"_op;
            if (types.GetKind(types.UnderlyingType(vectorType)) != EKind::I1
                || types.GetKind(function.GetTmpType(instr.Dest.Idx)) != (packed ? EKind::U32 : EKind::I1))
            {
                throw std::runtime_error("Invalid VM vector reduction types");
            }
            out.Op = packed
                ? EVMOp::VMovemask
                : (instr.Op == "all"_op ? EVMOp::VAll : EVMOp::VAny);
            return true;
        }
        case "select"_op: {
            const int maskType = OperandType(function, Module, instr.Operands[0]);
            const int resultType = function.GetTmpType(instr.Dest.Idx);
            if (instr.OperandCount != 3 || resultType != vectorType
                || OperandType(function, Module, instr.Operands[1]) != resultType
                || !types.IsVector(maskType) || types.VectorSize(maskType) != types.VectorSize(resultType)
                || types.GetKind(types.UnderlyingType(maskType)) != EKind::I1
                || (instr.Operands[0].Type != TOperand::EType::Tmp
                    && (instr.Operands[0].Type != TOperand::EType::Imm || instr.Operands[0].Imm.Value != 0)))
            {
                throw std::runtime_error("Invalid VM vector select types");
            }
            out.Op = EVMOp::VSelect;
            if (instr.Operands[0].Type == TOperand::EType::Tmp) {
                out.Mask = instr.Operands[0].Tmp;
            }
            return true;
        }
        case "gather"_op: {
            const int resultType = function.GetTmpType(instr.Dest.Idx);
            const int pointerType = OperandType(function, Module, instr.Operands[0]);
            if (!types.IsPointer(pointerType) || types.UnderlyingType(pointerType) != types.UnderlyingType(resultType)
                || types.VectorSize(resultType) != types.VectorSize(vectorType)
                || types.GetKind(types.UnderlyingType(vectorType)) != EKind::I64)
            {
                throw std::runtime_error("Invalid VM vector gather types");
            }
            out.Op = EVMOp::VGather;
            out.Format = VectorFormat(types, resultType);
            return true;
        }
        case "shuffle"_op: {
            const int sourceType = OperandType(function, Module, instr.Operands[0]);
            if (function.GetTmpType(instr.Dest.Idx) != sourceType) {
                throw std::runtime_error("Invalid VM vector shuffle result");
            }
            out.Format = VectorFormat(types, sourceType);
            const int maskType = OperandType(function, Module, instr.Operands[1]);
            if (types.IsVector(maskType)) {
                const int elementType = types.UnderlyingType(maskType);
                if (!types.IsInteger(elementType) || types.GetKind(elementType) == EKind::I1
                    || types.VectorSize(sourceType) != types.VectorSize(maskType))
                {
                    throw std::runtime_error("Invalid VM dynamic shuffle mask");
                }
                out.Op = EVMOp::VShuffleDynamic;
                out.Format |= std::countr_zero(static_cast<unsigned>(types.SizeInBytes(elementType))) << 6;
                return true;
            }
            const auto& mask = Module.VectorShuffleMasks.at(instr.Operands[1].Imm.Value);
            if (mask.size() != static_cast<size_t>(types.VectorSize(sourceType))
                || std::any_of(mask.begin(), mask.end(), [&](int index) {
                    return index < 0 || index >= types.VectorSize(sourceType);
                }))
            {
                throw std::runtime_error("Invalid VM vector shuffle shape");
            }
            out.Op = EVMOp::VShuffle;
            out.Operands[2] = TUntypedImm{reinterpret_cast<int64_t>(mask.data())};
            return true;
        }
        case "index"_op: {
            const int elementType = types.UnderlyingType(vectorType);
            if (function.GetTmpType(instr.Dest.Idx) != elementType
                || OperandType(function, Module, instr.Operands[0]) != vectorType
                || types.GetKind(OperandType(function, Module, instr.Operands[1])) != EKind::I64)
            {
                throw std::runtime_error("Invalid VM vector index types");
            }
            out.Op = EVMOp::VIndex;
            if (types.IsSigned(elementType)) {
                out.Format |= TVMInstr::SignedElement;
            }
            return true;
        }
        case "lea"_op:
            out.Op = EVMOp::Lea;
            out.Format = 3;
            return true;
        case "load"_op:
        case "lde"_op:
            out.Op = EVMOp::Load;
            return true;
        case "stre"_op:
        case "ste"_op:
            out.Op = EVMOp::Store;
            return true;
        case "mov"_op:
        case "bitcast"_op: {
            const auto& source = instr.Operands[0];
            const int destType = function.GetTmpType(instr.Dest.Idx);
            const int sourceType = OperandType(function, Module, source);
            const bool isZero = source.Type == TOperand::EType::Imm && source.Imm.Value == 0;
            if (instr.Op == "mov"_op && types.IsVector(sourceType) && types.IsVector(destType)
                && destType != sourceType)
            {
                const int sourceElement = types.UnderlyingType(sourceType);
                const int destElement = types.UnderlyingType(destType);
                if (types.VectorSize(sourceType) != types.VectorSize(destType)
                    || types.IsFloat(sourceElement) != types.IsFloat(destElement))
                {
                    throw std::runtime_error("Invalid VM vector conversion types");
                }
                out.Op = types.IsFloat(sourceElement) ? EVMOp::VFCast : EVMOp::VICast;
                if (types.IsSigned(sourceElement)) {
                    out.Format |= TVMInstr::SignedElement;
                }
                if (types.IsSigned(destElement)) {
                    out.Format |= TVMInstr::SignedResult;
                }
                out.Operands[2] = TUntypedImm{types.SizeInBytes(destElement)};
                return true;
            }
            const bool isBitcast = instr.Op == "bitcast"_op;
            const bool compatible = isBitcast
                ? types.SizeInBytes(destType) == types.SizeInBytes(sourceType)
                : destType == sourceType;
            // A scalar immediate reinterpreted as a vector is just its bits.
            if ((!isZero && !isBitcast && source.Type != TOperand::EType::Tmp) || (!isZero && !compatible)) {
                throw std::runtime_error("Unsupported VM vector copy or cast");
            }
            const auto destSize = static_cast<unsigned>(types.SizeInBytes(destType));
            if (isBitcast && !types.IsVector(destType) && destSize < 8) {
                // Read as one lane: that writes the whole register, extended by
                // the result type, where a plain copy would leave its top stale.
                out.Op = EVMOp::VIndex;
                out.Format = TVMInstr::MakeFormat(std::countr_zero(destSize));
                if (types.IsSigned(destType)) {
                    out.Format |= TVMInstr::SignedElement;
                }
                out.Operands[2] = TUntypedImm{0};
                return true;
            }
            out.Op = EVMOp::Mov;
            return true;
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
            const int elementType = types.UnderlyingType(vectorType);
            if (types.GetKind(elementType) == EKind::I1
                && (instr.Op == "+"_op || instr.Op == "-"_op || instr.Op == "*"_op))
            {
                throw std::runtime_error("VM vector arithmetic requires numeric lanes");
            }
            const bool comparison = instr.Op == "<"_op || instr.Op == ">"_op
                || instr.Op == "<="_op || instr.Op == ">="_op || instr.Op == "=="_op || instr.Op == "!="_op;
            const int resultType = function.GetTmpType(instr.Dest.Idx);
            const bool compatible = comparison
                ? types.IsVector(resultType) && types.VectorSize(resultType) == types.VectorSize(vectorType)
                    && types.GetKind(types.UnderlyingType(resultType)) == EKind::I1
                : resultType == vectorType;
            if (!compatible) {
                throw std::runtime_error("Invalid VM vector result type");
            }
            for (size_t i = 0; i < 2; ++i) {
                const int operandType = OperandType(function, Module, instr.Operands[i]);
                if (operandType == elementType) {
                    out.Format |= i == 0 ? TVMInstr::BroadcastLeft : TVMInstr::BroadcastRight;
                } else if (operandType != vectorType) {
                    throw std::runtime_error("VM vector arithmetic requires matching element types");
                }
            }
            const bool floating = types.IsFloat(elementType);
            const bool signedElement = types.IsSigned(elementType);
            switch (instr.Op) {
                case "+"_op: out.Op = floating ? EVMOp::VFAdd : EVMOp::VIAdd; break;
                case "-"_op: out.Op = floating ? EVMOp::VFSub : EVMOp::VISub; break;
                case "*"_op: out.Op = floating ? EVMOp::VFMul : EVMOp::VIMul; break;
                case "&"_op:
                case "&&"_op: out.Op = EVMOp::VIAnd; break;
                case "|"_op:
                case "||"_op: out.Op = EVMOp::VIOr; break;
                case "^"_op: out.Op = EVMOp::VIXor; break;
                case "<<"_op: out.Op = EVMOp::VIShl; break;
                case ">>"_op: out.Op = signedElement ? EVMOp::VIShrS : EVMOp::VIShrU; break;
                case "<"_op: out.Op = floating ? EVMOp::VFCmpLT : (signedElement ? EVMOp::VICmpLTS : EVMOp::VICmpLTU); break;
                case ">"_op: out.Op = floating ? EVMOp::VFCmpGT : (signedElement ? EVMOp::VICmpGTS : EVMOp::VICmpGTU); break;
                case "<="_op: out.Op = floating ? EVMOp::VFCmpLE : (signedElement ? EVMOp::VICmpLES : EVMOp::VICmpLEU); break;
                case ">="_op: out.Op = floating ? EVMOp::VFCmpGE : (signedElement ? EVMOp::VICmpGES : EVMOp::VICmpGEU); break;
                case "=="_op: out.Op = floating ? EVMOp::VFCmpEQ : EVMOp::VICmpEQ; break;
                case "!="_op: out.Op = floating ? EVMOp::VFCmpNE : EVMOp::VICmpNE; break;
            }
            return true;
        }
        case "!"_op:
        case "~"_op:
        case "i2b"_op:
        case "f2b"_op:
        case "i2f"_op:
        case "f2i"_op: {
            const int sourceElement = types.UnderlyingType(vectorType);
            const int destType = function.GetTmpType(instr.Dest.Idx);
            if (!types.IsVector(destType) || types.VectorSize(destType) != types.VectorSize(vectorType)) {
                throw std::runtime_error("Invalid VM vector unary result type");
            }
            switch (instr.Op) {
                case "!"_op: out.Op = types.IsFloat(sourceElement) ? EVMOp::VFNot : EVMOp::VINot; break;
                case "~"_op: out.Op = EVMOp::VIBitNot; break;
                case "i2b"_op: out.Op = EVMOp::VI2B; break;
                case "f2b"_op: out.Op = EVMOp::VF2B; break;
                case "i2f"_op: out.Op = EVMOp::VI2F; break;
                case "f2i"_op: out.Op = EVMOp::VF2I; break;
            }
            const int destElement = types.UnderlyingType(destType);
            if (instr.Op == "i2f"_op || instr.Op == "f2i"_op) {
                if (types.IsSigned(sourceElement)) {
                    out.Format |= TVMInstr::SignedElement;
                }
                if (types.IsSigned(destElement)) {
                    out.Format |= TVMInstr::SignedResult;
                }
                out.Operands[2] = TUntypedImm{types.SizeInBytes(destElement)};
            }
            return true;
        }
        default:
            throw std::runtime_error("Unsupported VM vector instruction: " + instr.Op.ToString());
    }
}

NFFI::IFunction* TVMCompiler::GetOrCreateExternalThunk(int externIdx) {
    if (auto it = ExternalThunkCache.find(externIdx); it != ExternalThunkCache.end()) {
        return it->second;
    }

    const TExternalFunction& ext = Module.ExternalFunctions[externIdx];
    std::unique_ptr<NFFI::IFunction> thunk;
    if (ext.Packed) {
        thunk = std::make_unique<TPackedFunction>(ext.Packed);
    } else {
        void* symbol = dlsym(RTLD_DEFAULT, ext.MangledName.c_str());
        if (!symbol) {
            return nullptr;
        }
        NFFI::EStructKind retStruct = NFFI::EStructKind::None;
        EKind retKind = ClassifyKind(ext.ReturnTypeId, Module.Types, retStruct);
        size_t retSize = static_cast<size_t>(Module.Types.SizeInBytes(ext.ReturnTypeId));
        std::vector<EKind> argKinds;
        std::vector<NFFI::EStructKind> argStructs;
        std::vector<size_t> argSizes;
        argKinds.reserve(ext.ArgTypes.size());
        argStructs.reserve(ext.ArgTypes.size());
        argSizes.reserve(ext.ArgTypes.size());
        for (int argType : ext.ArgTypes) {
            NFFI::EStructKind argStruct = NFFI::EStructKind::None;
            argKinds.push_back(ClassifyKind(argType, Module.Types, argStruct));
            argStructs.push_back(argStruct);
            argSizes.push_back(static_cast<size_t>(Module.Types.SizeInBytes(argType)));
        }
        thunk.reset(NFFI::BuildFFI(symbol, retKind, retStruct, retSize, argKinds, argStructs, argSizes));
        if (!thunk) {
            return nullptr;
        }
    }

    NFFI::IFunction* raw = thunk.get();
    ExternalThunks.push_back(std::move(thunk));
    ExternalThunkCache[externIdx] = raw;
    return raw;
}

TExecFunc& TVMCompiler::Compile(TFunction& function, bool printByteCode) {
    auto it = CodeCache.find(function.SymId);
    if (it != CodeCache.end() && it->second.UniqueId == function.UniqueId) {
        return it->second;
    }

    CodeCache[function.SymId] = TExecFunc {
        .UniqueId = function.UniqueId
    };

    auto& execFunc = CodeCache[function.SymId];
    NPasses::BeforeCompile(function, Module);
    CompileUltraLow(function, execFunc);
    if (printByteCode) {
        std::cerr << "Compiled function " << function.Name << " (symId=" << function.SymId << ", uniqueId=" << function.UniqueId << "):\n";
        std::cerr << "Start address: " << (uint64_t)execFunc.VMCode.data() << ", insrt size: " << sizeof(TVMInstr) << " bytes\n";
        char* p = reinterpret_cast<char*>(execFunc.VMCode.data());
        for (size_t i = 0; i < execFunc.VMCode.size(); ++i) {
            std::cerr << std::setw(4) << (uint64_t)(p + i * sizeof(TVMInstr)) << ": "<< execFunc.VMCode[i] << "\n";
        }
    }
    return execFunc;
}

void TVMCompiler::CompileUltraLow(const TFunction& function, TExecFunc& funcOut)
{
    int lowStringTypeId = Module.Types.Ptr(Module.Types.I(EKind::I8));
    std::unordered_map<int64_t, int64_t> labelToPC;
    std::unordered_map<int64_t, int64_t> labelToLastPC;

    auto& code = funcOut.VMCode;
    std::unordered_set<int> usedTmps;
    for (const auto& block : function.Blocks) {
        labelToPC[block.Label.Idx] = code.size();
        for (const auto& instr : block.Instrs) {
            if (instr.Op == "prefetch"_op) {
                continue;
            }
            if (instr.Dest.Idx >= 0) {
                usedTmps.insert(instr.Dest.Idx);
            }
            for (int i = 0; i < instr.OperandCount; ++i) {
                if (instr.Operands[i].Type == TOperand::EType::Tmp && instr.Operands[i].Tmp.Idx >= 0) {
                    usedTmps.insert(instr.Operands[i].Tmp.Idx);
                }
            }
            code.emplace_back(); // placeholder
        }
        labelToLastPC[block.Label.Idx] = code.size() - 1;
    }

    // Compute byte offset for each local variable and address-backed temporary.
    // VM pointers must refer to memory owned by the current call frame; allocating
    // per instruction would make struct-heavy loops grow runtime-owned buffers.
    std::vector<int> tmpFrameOffsets(function.TmpTypes.size(), -1);
    {
        int offset = 0;
        for (int typeId : function.LocalTypes) {
            offset = AlignUp(offset, 8);
            funcOut.LocalByteOffsets.push_back(offset);
            offset += Module.Types.SizeInBytes(typeId);
        }

        for (int tmpIdx = 0; tmpIdx < (int)function.TmpTypes.size(); ++tmpIdx) {
            const int typeId = function.TmpTypes[tmpIdx];
            if (usedTmps.contains(tmpIdx) && typeId >= 0 && Module.Types.GetKind(typeId) == EKind::Struct) {
                offset = AlignUp(offset, 8);
                tmpFrameOffsets[tmpIdx] = offset;
                offset += Module.Types.SizeInBytes(typeId);
            }
        }

        for (const auto& block : function.Blocks) {
            for (const auto& instr : block.Instrs) {
                if (instr.Op != "salloc"_op || instr.Dest.Idx < 0 || instr.OperandCount != 1) {
                    continue;
                }
                offset = AlignUp(offset, 8);
                if (instr.Dest.Idx >= (int)tmpFrameOffsets.size()) {
                    tmpFrameOffsets.resize(instr.Dest.Idx + 1, -1);
                }
                tmpFrameOffsets[instr.Dest.Idx] = offset;
                offset += static_cast<int>(instr.Operands[0].Imm.Value);
            }
        }

        funcOut.NumLocals = AlignUp(offset, 8); // frame size in bytes
    }

    // Populate ArgByteOffsets and ArgTypeIds for eval
    for (const auto& argLocal : function.ArgLocals) {
        if (argLocal.Idx >= 0 && argLocal.Idx < (int)funcOut.LocalByteOffsets.size()) {
            funcOut.ArgByteOffsets.push_back(funcOut.LocalByteOffsets[argLocal.Idx]);
            int typeId = (argLocal.Idx < (int)function.LocalTypes.size())
                ? function.LocalTypes[argLocal.Idx] : -1;
            if (Module.Types.IsVector(typeId)) {
                throw std::runtime_error("VM vector arguments are not supported yet");
            }
            funcOut.ArgTypeIds.push_back(typeId);
        }
    }

    auto require = [&](const TInstr& ins, int requireDest, size_t requireOperands) {
        // requireDest = -1/0/1 = no, optional, required
        if (requireDest == 1) {
            if (ins.Dest.Idx < 0) {
                throw std::runtime_error("Instruction " + ins.Op.ToString() + " needs a destination tmp");
            }
        } else if (requireDest == 0) {
            // optional
        } else {
            // no dest
            if (ins.Dest.Idx >= 0) {
                throw std::runtime_error("Instruction " + ins.Op.ToString() + " must not have a destination tmp");
            }
        }
        if (ins.OperandCount != requireOperands) {
            throw std::runtime_error("Instruction " + ins.Op.ToString() + " needs " + std::to_string(requireOperands) + " operands");
        }
    };

    auto typeId = [&](const TTmp& t) -> int {
        if (t.Idx < 0 || t.Idx >= function.TmpTypes.size()) return -1;
        return function.TmpTypes[t.Idx];
    };

    auto typeIdOp = [&](const TOperand& s) -> int {
        return OperandType(function, Module, s);
    };

    auto cmpType = [&](const TInstr& ins) -> int {
        auto leftType = typeIdOp(ins.Operands[0]);
        auto rightType = typeIdOp(ins.Operands[1]);
        // -1 - signed
        //  0 - float
        //  1 - unsigned
        if (Module.Types.IsFloat(leftType) || Module.Types.IsFloat(rightType)) {
            return 0;
        }
        return Module.Types.IsUnsigned(leftType) ? 1 : -1;
    };

    auto cmp128 = [&](const TInstr& ins) -> bool {
        return Is128BitInteger(Module.Types, typeIdOp(ins.Operands[0]))
            || Is128BitInteger(Module.Types, typeIdOp(ins.Operands[1]));
    };

    auto isSignedInteger = [&](int typeId) -> bool {
        // The IR type table has unsigned kinds reserved, but source integer
        // lowering currently produces signed integer kinds only.
        return Module.Types.GetKind(typeId) != EKind::U8
            && Module.Types.GetKind(typeId) != EKind::U16
            && Module.Types.GetKind(typeId) != EKind::U32
            && Module.Types.GetKind(typeId) != EKind::U64
            && Module.Types.GetKind(typeId) != EKind::U128;
    };

    auto is128 = [&](int typeId) -> bool {
        return Is128BitInteger(Module.Types, typeId);
    };

    auto ins2vm = [&](const TInstr& ins, TVMInstr*& ptr) {
        if (ins.Op == "prefetch"_op) {
            return;
        }
        auto& out = *ptr++;
        int offset = 0;
        if (ins.Dest.Idx >= 0) {
            out.Operands[0] = ins.Dest;
            offset = 1;
        }
        // Select stores its first argument in Mask; the two values use regular operands.
        const size_t first = ins.Op == "select"_op
            ? 1
            : 0;
        for (size_t i = first; i < ins.OperandCount && i - first + offset < out.Operands.size(); ++i) {
            auto& operand = out.Operands[i - first + offset];
            switch (ins.Operands[i].Type) {
                case TOperand::EType::Tmp:
                    operand = ins.Operands[i].Tmp;
                    break;
                case TOperand::EType::Slot:
                    operand = ins.Operands[i].Slot;
                    break;
                case TOperand::EType::Local: {
                    // Translate var index -> byte offset in frame
                    int varIdx = ins.Operands[i].Local.Idx;
                    if (varIdx < 0 || varIdx >= (int)function.LocalTypes.size()) {
                        throw std::runtime_error("Local variable index out of bounds");
                    }
                    int byteOffset = funcOut.LocalByteOffsets[varIdx];
                    operand = TLocal{byteOffset};
                    break;
                }
                case TOperand::EType::Imm:
                    operand = ins.Operands[i].Imm;
                    break;
                case TOperand::EType::Label:
                    TVMInstr* pc = &code[labelToPC.at(ins.Operands[i].Label.Idx)];
                    operand = TImm{reinterpret_cast<int64_t>(pc)};
                    break;
            };
        }

        if (CompileVectorInstruction(function, ins, out)) {
            return;
        }

        // TODO: check operand types
        switch (ins.Op) {
            case "salloc"_op: {
                require(ins, 1, 1);
                out.Op = EVMOp::SAlloc;
                const int tmpIdx = ins.Dest.Idx;
                if (tmpIdx < 0 || tmpIdx >= (int)tmpFrameOffsets.size()
                    || tmpFrameOffsets[tmpIdx] < 0)
                {
                    throw std::runtime_error("salloc temporary has no frame storage");
                }
                out.Operands[1] = TUntypedImm{tmpFrameOffsets[tmpIdx]};
                out.Operands[2] = TUntypedImm{ins.Operands[0].Imm.Value};
                break;
            }
            case "ste"_op: {
                require(ins, 0, 2);
                int storeTypeId = typeIdOp(ins.Operands[1]);
                const int ptrTypeId = typeIdOp(ins.Operands[0]);
                if (Module.Types.IsPointer(ptrTypeId)) {
                    storeTypeId = Module.Types.UnderlyingType(ptrTypeId);
                }
                out.Op = is128(storeTypeId) ? EVMOp::Ste128 : EVMOp::Ste;
                out.Operands[2] = TUntypedImm{Module.Types.SizeInBytes(storeTypeId)};
                break;
            }
            case "lde"_op: {
                require(ins, 1, 1);
                out.Op = is128(typeId(ins.Dest)) ? EVMOp::Lde128 : EVMOp::Lde;
                out.Operands[2] = TUntypedImm{Module.Types.SizeInBytes(typeId(ins.Dest))};
                break;
            }
            case "lea"_op: {
                require(ins, 1, 1);
                out.Op = EVMOp::Lea;
                break;
            }
            case '+'_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    out.Op = EVMOp::FAdd;
                } else {
                    out.Op = is128(t) ? EVMOp::IAdd128 : EVMOp::IAdd;
                }
                break;
            }
            case '-'_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    out.Op = EVMOp::FSub;
                } else {
                    out.Op = is128(t) ? EVMOp::ISub128 : EVMOp::ISub;
                }
                break;
            }
            case '*'_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    out.Op = EVMOp::FMul;
                } else if (is128(t)) {
                    out.Op = EVMOp::IMul128;
                } else {
                    out.Op = EVMOp::IMulS;
                }
                break;
            }
            case '/'_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    out.Op = EVMOp::FDiv;
                } else if (is128(t)) {
                    out.Op = isSignedInteger(t) ? EVMOp::IDivS128 : EVMOp::IDivU128;
                } else {
                    out.Op = isSignedInteger(t) ? EVMOp::IDivS : EVMOp::IDivU;
                }
                break;
            }
            case '%'_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    throw std::runtime_error("Remainder '%' is not defined for float types");
                }
                if (is128(t)) {
                    out.Op = isSignedInteger(t) ? EVMOp::IRemS128 : EVMOp::IRemU128;
                } else {
                    out.Op = isSignedInteger(t) ? EVMOp::IRemS : EVMOp::IRemU;
                }
                break;
            }
            case '&'_op: {
                require(ins, 1, 2);
                if (Module.Types.IsFloat(typeId(out.Operands[0].Tmp))) {
                    throw std::runtime_error("Bitwise '&' is not defined for float types");
                }
                out.Op = is128(typeId(out.Operands[0].Tmp)) ? EVMOp::IAnd128 : EVMOp::IAnd;
                break;
            }
            case '|'_op: {
                require(ins, 1, 2);
                if (Module.Types.IsFloat(typeId(out.Operands[0].Tmp))) {
                    throw std::runtime_error("Bitwise '|' is not defined for float types");
                }
                out.Op = is128(typeId(out.Operands[0].Tmp)) ? EVMOp::IOr128 : EVMOp::IOr;
                break;
            }
            case '^'_op: {
                require(ins, 1, 2);
                if (Module.Types.IsFloat(typeId(out.Operands[0].Tmp))) {
                    throw std::runtime_error("Bitwise '^' is not defined for float types");
                }
                out.Op = is128(typeId(out.Operands[0].Tmp)) ? EVMOp::IXor128 : EVMOp::IXor;
                break;
            }
            case "<<"_op: {
                require(ins, 1, 2);
                if (Module.Types.IsFloat(typeId(out.Operands[0].Tmp))) {
                    throw std::runtime_error("Bitwise '<<' is not defined for float types");
                }
                out.Op = is128(typeId(out.Operands[0].Tmp)) ? EVMOp::IShl128 : EVMOp::IShl;
                break;
            }
            case ">>"_op: {
                require(ins, 1, 2);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    throw std::runtime_error("Bitwise '>>' is not defined for float types");
                }
                if (is128(t)) {
                    out.Op = isSignedInteger(t) ? EVMOp::IShrS128 : EVMOp::IShrU128;
                } else {
                    out.Op = isSignedInteger(t) ? EVMOp::IShrS : EVMOp::IShrU;
                }
                break;
            }
            case '<'_op: {
                require(ins, 1, 2);
                auto cType = cmpType(ins);
                if (cType == 0) {
                    out.Op = EVMOp::FCmpLT;
                } else if (cType == 1) {
                    out.Op = cmp128(ins) ? EVMOp::ICmpLTU128 : EVMOp::ICmpLTU;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpLTS128 : EVMOp::ICmpLTS;
                }
                break;
            }
            case '>'_op: {
                require(ins, 1, 2);
                auto cType = cmpType(ins);
                if (cType == 0) {
                    out.Op = EVMOp::FCmpGT;
                } else if (cType == 1) {
                    out.Op = cmp128(ins) ? EVMOp::ICmpGTU128 : EVMOp::ICmpGTU;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpGTS128 : EVMOp::ICmpGTS;
                }
                break;
            }
            case "<="_op: {
                require(ins, 1, 2);
                auto cType = cmpType(ins);
                if (cType == 0) {
                    out.Op = EVMOp::FCmpLE;
                } else if (cType == 1) {
                    out.Op = cmp128(ins) ? EVMOp::ICmpLEU128 : EVMOp::ICmpLEU;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpLES128 : EVMOp::ICmpLES;
                }
                break;
            }
            case ">="_op: {
                require(ins, 1, 2);
                auto cType = cmpType(ins);
                if (cType == 0) {
                    out.Op = EVMOp::FCmpGE;
                } else if (cType == 1) {
                    out.Op = cmp128(ins) ? EVMOp::ICmpGEU128 : EVMOp::ICmpGEU;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpGES128 : EVMOp::ICmpGES;
                }
                break;
            }
            case "=="_op: {
                require(ins, 1, 2);
                if (cmpType(ins) == 0) {
                    out.Op = EVMOp::FCmpEQ;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpEQ128 : EVMOp::ICmpEQ;
                }
                break;
            }
            case "!="_op: {
                require(ins, 1, 2);
                if (cmpType(ins) == 0) {
                    out.Op = EVMOp::FCmpNE;
                } else {
                    out.Op = cmp128(ins) ? EVMOp::ICmpNE128 : EVMOp::ICmpNE;
                }
                break;
            }
            case "neg"_op: {
                require(ins, 1, 1);
                auto t = typeId(out.Operands[0].Tmp);
                if (Module.Types.IsFloat(t)) {
                    out.Op = EVMOp::FNeg;
                } else {
                    out.Op = is128(t) ? EVMOp::INeg128 : EVMOp::INeg;
                }
                break;
            }
            case "!"_op: {
                require(ins, 1, 1);
                if (Module.Types.IsFloat(typeIdOp(ins.Operands[0]))) {
                    out.Op = EVMOp::FCmpEQ;
                    out.Operands[2] = TUntypedImm{0};
                } else {
                    out.Op = is128(typeIdOp(ins.Operands[0])) ? EVMOp::INot128 : EVMOp::INot;
                }
                break;
            }
            case '~'_op: {
                require(ins, 1, 1);
                if (Module.Types.IsFloat(typeId(out.Operands[0].Tmp))) {
                    throw std::runtime_error("Bitwise '~' is not defined for float types");
                }
                out.Op = is128(typeId(out.Operands[0].Tmp)) ? EVMOp::IBitNot128 : EVMOp::IBitNot;
                break;
            }
            case "jmp"_op: {
                require(ins, -1, 1);
                out.Op = EVMOp::Jmp;
                break;
            }
            case "cmp"_op: {
                require(ins, -1, 3);
                out.Op = EVMOp::Cmp;
                break;
            }
            case "mov"_op: {
                require(ins, 1, 1);
                const bool isImm = ins.Operands[0].Type == TOperand::EType::Imm;
                const int dstType = typeId(ins.Dest);
                const int srcType = typeIdOp(ins.Operands[0]);
                if (is128(dstType)) {
                    // A 128-bit literal carries its low half only, so an immediate
                    // widens exactly like a 64-bit register does.
                    const bool signExtend = isImm ? isSignedInteger(dstType) : isSignedInteger(srcType);
                    if (is128(srcType) && !isImm) {
                        out.Op = EVMOp::Mov;
                        out.Format = TVMInstr::MakeFormat(4);
                    } else if (isImm) {
                        out.Op = signExtend ? EVMOp::CmovS128 : EVMOp::CmovU128;
                    } else {
                        out.Op = signExtend ? EVMOp::SExt128 : EVMOp::ZExt128;
                    }
                } else if (is128(srcType) && !isImm) {
                    out.Op = EVMOp::Trunc128;
                } else {
                    out.Op = isImm ? EVMOp::Cmov : EVMOp::Mov;
                }
                break;
            }
            case "i2b"_op: {
                require(ins, 1, 1);
                out.Op = is128(typeIdOp(ins.Operands[0])) ? EVMOp::I2B128 : EVMOp::ICmpNE;
                out.Operands[2] = TUntypedImm{0};
                break;
            }
            case "i2f"_op: {
                require(ins, 1, 1);
                const int srcType = typeIdOp(ins.Operands[0]);
                if (is128(srcType)) {
                    out.Op = isSignedInteger(srcType) ? EVMOp::I2F128S : EVMOp::I2F128U;
                } else {
                    out.Op = EVMOp::I2F;
                }
                break;
            }
            case "f2b"_op: {
                require(ins, 1, 1);
                out.Op = EVMOp::FCmpNE;
                out.Operands[2] = TUntypedImm{0};
                break;
            }
            case "f2i"_op: {
                require(ins, 1, 1);
                out.Op = is128(typeId(ins.Dest)) ? EVMOp::F2I128 : EVMOp::F2I;
                break;
            }
            case "bitcast"_op: {
                require(ins, 1, 1);
                if (is128(typeId(ins.Dest)) || is128(typeIdOp(ins.Operands[0]))) {
                    out.Op = EVMOp::Mov;
                    out.Format = TVMInstr::MakeFormat(4);
                } else {
                    out.Op = EVMOp::Bitcast;
                }
                break;
            }
            case "arg"_op: {
                require(ins, -1, 1);
                if (ins.Operands[0].Type == TOperand::EType::Tmp) {
                    out.Op = is128(typeIdOp(ins.Operands[0])) ? EVMOp::ArgTmp128 : EVMOp::ArgTmp;
                } else if (ins.Operands[0].Type == TOperand::EType::Imm) {
                    out.Op = EVMOp::ArgConst;
                } else {
                    throw std::runtime_error("arg operand must be Imm or Tmp");
                }
                // convert id to pointer
                if (ins.Operands[0].Type == TOperand::EType::Imm) {
                    auto imm = ins.Operands[0].Imm;
                    if (imm.TypeId == lowStringTypeId) { // string literal
                        int id = (int)imm.Value;
                        if (id < 0 || id >= Module.StringLiterals.size()) {
                            throw std::runtime_error("Invalid string literal id in outs");
                        }
                        auto& str = Module.StringLiterals[id];
                        out.Operands[0] = TImm{(int64_t)str.c_str(), lowStringTypeId};
                    }
                }
                break;
            }
            case "call"_op: {
                require(ins, 0, 1);

                const int64_t calleeId = ins.Operands[0].Imm.Value;

                if (ins.Dest.Idx < 0) {
                    out.Operands[0] = TTmp{-1}; // no dest
                }

                if (Module.SymIdToFuncIdx.contains(calleeId)) {
                    const int64_t calleeIdx = Module.SymIdToFuncIdx.at(calleeId);
                    assert(calleeIdx >=0 && calleeIdx < Module.Functions.size() && "Invalid callee idx");
                    out.Operands[1] = TImm{calleeIdx};
                    out.Op = EVMOp::Call;
                } else if (Module.SymIdToExtFuncIdx.contains(calleeId)) {
                    const int64_t calleeIdx = Module.SymIdToExtFuncIdx.at(calleeId);
                    assert(calleeIdx >=0 && calleeIdx < Module.ExternalFunctions.size() && "Invalid callee idx");
                    NFFI::IFunction* thunk = GetOrCreateExternalThunk(static_cast<int>(calleeIdx));
                    if (!thunk) {
                        throw std::runtime_error(
                            "cannot resolve external function `"
                            + Module.ExternalFunctions[calleeIdx].MangledName + "'");
                    }
                    out.Operands[1] = TImm{reinterpret_cast<int64_t>(thunk)};
                    out.Op = EVMOp::ECall;
                } else {
                    throw std::runtime_error("Unknown callee id in call: " + std::to_string(calleeId));
                }

                break;
            }
            case "await"_op: {
                if (ins.Dest.Idx < 0) {
                    out.Op = EVMOp::AwaitVoid;
                } else {
                    out.Op = EVMOp::Await;
                }
                break;
            }
            case "ret"_op: {
                if (ins.OperandCount == 0) {
                    out.Op = EVMOp::RetVoid;
                } else if (is128(typeIdOp(ins.Operands[0]))) {
                    out.Op = EVMOp::Ret128;
                } else {
                    out.Op = EVMOp::Ret;
                }
                break;
            }
            case "load"_op: {
                require(ins, 1, 1);
                int destType = typeId(ins.Dest);
                if (destType >= 0 && Module.Types.GetKind(destType) == EKind::Struct) {
                    // IR load is a struct value; VM represents struct values as 64-bit addresses.
                    out.Op = EVMOp::Lea;
                } else {
                    out.Op = EVMOp::Load;
                    out.Format = TVMInstr::MakeFormat(is128(destType) ? 4 : 3);
                }
                break;
            }
            case "stre"_op: {
                require(ins, 0, 2);
                // If destination local has struct type, emit StructStore (dst=Local, src=Tmp ptr, size)
                if (ins.Operands[0].Type == TOperand::EType::Local) {
                    int varIdx = ins.Operands[0].Local.Idx;
                    int dstTypeId = (varIdx >= 0 && varIdx < (int)function.LocalTypes.size())
                        ? function.LocalTypes[varIdx] : -1;
                    if (dstTypeId >= 0 && Module.Types.GetKind(dstTypeId) == EKind::Struct) {
                        out.Op = EVMOp::StructStore;
                        out.Operands[2] = TUntypedImm{Module.Types.SizeInBytes(dstTypeId)};
                        break;
                    }
                }
                out.Op = EVMOp::Store;
                out.Format = TVMInstr::MakeFormat(is128(typeIdOp(ins.Operands[1])) ? 4 : 3);
                break;
            }
            case "copy"_op: {
                require(ins, -1, 3); // no dest, args: dst_ptr(Tmp), src_ptr(Tmp), size_imm
                out.Op = EVMOp::Copy;
                break;
            }
            default:
                throw std::runtime_error("Unknown instruction in CompileUltraLow: " + ins.Op.ToString());
        }
    };

    auto* ptr = code.data();
    for (const auto& block : function.Blocks) {
        for (size_t i = 0; i < block.Instrs.size(); ++i) {
            const size_t firstVMIndex = ptr - code.data();
            ins2vm(block.Instrs[i], ptr);
            if (!block.InstrDebugInfo.empty()) {
                const size_t lastVMIndex = ptr - code.data();
                funcOut.InstrDebugInfo.resize(lastVMIndex);
                const auto debugInfo = i < block.InstrDebugInfo.size()
                    ? block.InstrDebugInfo[i] : TInstrDebugInfo{};
                // 1 instr may produce > 1 VM instr
                for (size_t vmIndex = firstVMIndex; vmIndex < lastVMIndex; ++vmIndex) {
                    funcOut.InstrDebugInfo[vmIndex] = debugInfo;
                }
            }
        }
    }
    if (!funcOut.InstrDebugInfo.empty()) {
        funcOut.InstrDebugInfo.resize(ptr - code.data());
    }
    AllocateRegisters(function, tmpFrameOffsets, funcOut);
}

void TVMCompiler::AllocateRegisters(
    const TFunction& function,
    const std::vector<int>& tmpFrameOffsets,
    TExecFunc& out)
{
    int offset = 0;
    auto allocate = [&](TTmp& tmp) {
        if (tmp.Idx < 0) {
            return;
        }
        const int tmpId = tmp.Idx;
        if (auto it = out.TmpByteOffsets.find(tmpId); it != out.TmpByteOffsets.end()) {
            tmp.Idx = it->second;
            return;
        }
        const int typeId = function.GetTmpType(tmpId);
        const bool isStruct = typeId >= 0 && Module.Types.GetKind(typeId) == EKind::Struct;
        // Existing scalar opcodes read/write a full machine word; struct values are addresses.
        const int size = isStruct
            ? 8
            : std::max(8, Module.Types.SizeInBytes(typeId));
        const int alignment = RegisterAlignment(Module.Types, typeId);
        if (offset > std::numeric_limits<int32_t>::max() - alignment - size) {
            throw std::runtime_error("VM register file is too large");
        }
        offset = AlignUp(offset, alignment);
        out.TmpByteOffsets[tmpId] = offset;
        if (isStruct) {
            const int frameOffset = tmpFrameOffsets.at(tmpId);
            out.StructRegisters[offset] = {frameOffset, Module.Types.SizeInBytes(typeId)};
        }
        tmp.Idx = offset;
        offset += size;
        out.RegisterFileAlignment = std::max(out.RegisterFileAlignment, alignment);
    };
    for (auto& instr : out.VMCode) {
        for (auto& operand : instr.Operands) {
            if (operand.Type == TVMOperand::EType::Tmp) {
                allocate(operand.Tmp);
            }
        }
        allocate(instr.Mask);
    }
    out.RegisterFileSize = offset;
}

} // namespace NIR
} // namespace NQumir
