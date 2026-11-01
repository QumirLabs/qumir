#pragma once

#include <qumir/ir/builder.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace NQumir {
namespace NIR {

enum class EVMOp : uint8_t {
    // Generic scalar forms also accept immediate-left operands.
    // integer ALU ops
    INeg, // unary -
    INot, // unary !
    IBitNot, // unary ~
    IAdd, // +
    ISub, // -
    IMulS, // * signed
    IDivS, // / signed
    IDivU, // / unsigned
    IRemS, // % signed
    IRemU, // % unsigned
    IAnd, // &
    IOr,  // |
    IXor, // xor
    IShl, // <<
    IShrS, // >> signed
    IShrU, // >> unsigned
    ICmpLTS, // < signed
    ICmpLTU, // < unsigned
    ICmpGTS, // > signed
    ICmpGTU, // > unsigned
    ICmpLES, // <= signed
    ICmpLEU, // <= unsigned
    ICmpGES, // >= signed
    ICmpGEU, // >= unsigned
    ICmpEQ, // ==
    ICmpNE, // !=

    // float ALU ops
    FNeg, // unary -
    FAdd, // +
    FSub, // -
    FMul, // *
    FDiv, // /
    FCmpLT, // <
    FCmpGT, // >
    FCmpLE, // <=
    FCmpGE, // >=
    FCmpEQ, // ==
    FCmpNE, // !=

    // f32 ALU ops: the value is the low four bytes of an eight-byte register
    FNeg32,
    FAdd32,
    FSub32,
    FMul32,
    FDiv32,
    FCmpLT32,
    FCmpGT32,
    FCmpLE32,
    FCmpGE32,
    FCmpEQ32,
    FCmpNE32,
    I2F32, // int to f32
    B2F32, // bool to f32, true is -1.0 as in LLVM
    F2I32, // f32 to int
    FExt32, // f32 to f64
    FTrunc64, // f64 to f32

    // load/store
    Load,
    Store,

    // tmp assignment
    Mov,
    Cmov, // convert imm to tmp
    I2F, // int to float
    B2F, // bool to f64, true is -1.0 as in LLVM
    F2I, // float to int
    Bitcast,

    // control flow
    Jmp,
    Cmp,
    ArgTmp, // temporary to argument
    ArgConst, // constant to argument
    Call,
    ECall, // external call
    Await,
    AwaitVoid,
    Ret,
    RetVoid,

    // pointer arithmetic
    Ste, // store by address (*a = i)
    Lde, // load by address (a = *i)
    LdeS, // like Lde, sign-extends a value narrower than 8 bytes
    Lea, // load effective address (a = &i)
    Copy,        // copy(dst_ptr, src, size_bytes_imm); src may be a pointer or packed value
    StructStore, // struct_store(dst_local, src_tmp, size_imm) - memcpy from Tmp into Local frame slot
    SAlloc,      // salloc(dst_tmp, frame_offset_imm, size_imm) - zero frame storage and return its address

    // All register operands are byte offsets in the same aligned register file.
    INeg128,
    INot128,
    IBitNot128,
    IAdd128,
    ISub128,
    IMul128,
    IDivS128,
    IDivU128,
    IRemS128,
    IRemU128,
    IAnd128,
    IOr128,
    IXor128,
    IShl128,
    IShrS128,
    IShrU128,
    ICmpLTS128,
    ICmpLTU128,
    ICmpGTS128,
    ICmpGTU128,
    ICmpLES128,
    ICmpLEU128,
    ICmpGES128,
    ICmpGEU128,
    ICmpEQ128,
    ICmpNE128,

    CmovS128, // sign-extend a 64-bit immediate into a 128-bit register
    CmovU128, // zero-extend a 64-bit immediate into a 128-bit register
    SExt128,  // sign-extend a 64-bit register into a 128-bit one
    ZExt128,  // zero-extend a 64-bit register into a 128-bit one
    B2I128,   // bool register into 128 bits, true is all ones as in LLVM
    Trunc128, // low 64 bits of a 128-bit register into a 64-bit one
    I2B128,
    I2F128S,
    I2F128U,
    F2I128,
    Lde128,
    Ste128,
    ArgTmp128,
    Ret128,
    ArgVec, // push the bytes of a vector register as a call argument
    RetVec, // return the bytes of a vector register

    // Vector arithmetic uses Format; scalar arithmetic keeps its fixed width.
    VIAdd,
    VISub,
    VIMul,
    VFAdd,
    VFSub,
    VFMul,
    VIndex,
    VIAnd,
    VIOr,
    VIXor,
    VIShl,
    VIShrS,
    VIShrU,
    VIBitNot,
    VINot,
    VFNot,
    VICmpLTS,
    VICmpLTU,
    VICmpGTS,
    VICmpGTU,
    VICmpLES,
    VICmpLEU,
    VICmpGES,
    VICmpGEU,
    VICmpEQ,
    VICmpNE,
    VFCmpLT,
    VFCmpGT,
    VFCmpLE,
    VFCmpGE,
    VFCmpEQ,
    VFCmpNE,
    VICast,
    VI2F,
    VF2I,
    VFCast,
    VI2B,
    VF2B,
    VAll,
    VAny,
    VShuffle,
    VShuffleDynamic,
    VMovemask,
    VSelect,
    VGather,

#define QUMIR_VM_ALU(name, type, operation) name##RR, name##RI,
#include <qumir/ir/scalar_alu.inc>
#undef QUMIR_VM_ALU
};

std::ostream& operator<<(std::ostream& os, EVMOp op);

struct TUntypedImm {
    int64_t Value;
};

struct TVMOperand {
    union {
        TTmp  Tmp;
        TSlot Slot; // TODO: replace Slot/Local with Address
        TLocal Local;
        TUntypedImm  Imm;
    };

    enum class EType : uint8_t {
        Tmp,
        Slot,
        Local,
        Imm,
    } Type;

    TVMOperand() : Type(EType::Tmp), Tmp({-1}) {}
    TVMOperand(const TTmp& t) : Type(EType::Tmp), Tmp(t) {}
    TVMOperand(const TSlot& s) : Type(EType::Slot), Slot(s) {}
    TVMOperand(const TLocal& l) : Type(EType::Local), Local(l) {}
    TVMOperand(const TImm& i) : Type(EType::Imm), Imm(i.Value) {}
    TVMOperand(const TUntypedImm& i) : Type(EType::Imm), Imm(i) {}

    template<typename T>
    void Visit(T&& visitor) const {
        switch (Type) {
        case EType::Tmp: visitor(Tmp); break;
        case EType::Slot: visitor(Slot); break;
        case EType::Local: visitor(Local); break;
        case EType::Imm: visitor(Imm); break;
        }
    }
};

union TVMValue {
    TTmp Tmp;
    TSlot Slot;
    TLocal Local;
    TUntypedImm Imm;

    TVMValue() : Tmp{-1} {}
};

struct TVMInstr {
    std::array<TVMValue, 3> Operands;
    EVMOp Op{};
    // Bits 0..2: log2(element bytes), 3..5: log2(lanes), 6..7: opcode flags.
    uint8_t Format = 3;
    // Keeping the three two-bit tags together avoids padding each operand to 16 bytes.
    uint8_t OperandTypes = 0;
    TTmp Mask{-1};

    TVMInstr() = default;

    TVMInstr(std::array<TVMOperand, 3> operands, EVMOp op, uint8_t format = 3)
        : Op(op)
        , Format(format)
    {
        for (size_t i = 0; i < operands.size(); ++i) {
            SetOperand(i, operands[i]);
        }
    }

    TVMOperand::EType GetOperandType(size_t index) const {
        assert(index < Operands.size());
        return static_cast<TVMOperand::EType>((OperandTypes >> (2 * index)) & 3);
    }

    void SetOperand(size_t index, const TVMOperand& operand) {
        assert(index < Operands.size());
        switch (operand.Type) {
            case TVMOperand::EType::Tmp: Operands[index].Tmp = operand.Tmp; break;
            case TVMOperand::EType::Slot: Operands[index].Slot = operand.Slot; break;
            case TVMOperand::EType::Local: Operands[index].Local = operand.Local; break;
            case TVMOperand::EType::Imm: Operands[index].Imm = operand.Imm; break;
        }
        const auto shift = 2 * index;
        OperandTypes = static_cast<uint8_t>((OperandTypes & ~(3u << shift))
            | (static_cast<unsigned>(operand.Type) << shift));
    }

    static constexpr uint8_t BroadcastLeft = 1 << 6;
    static constexpr uint8_t BroadcastRight = 1 << 7;
    static constexpr uint8_t SignedElement = 1 << 6;
    static constexpr uint8_t SignedResult = 1 << 7;

    static constexpr uint8_t MakeFormat(uint8_t elementLog2, uint8_t lanesLog2 = 0) {
        assert(elementLog2 <= 4 && lanesLog2 <= 5);
        return static_cast<uint8_t>(elementLog2 | (lanesLog2 << 3));
    }

    constexpr size_t ElementSizeInBytes() const {
        assert((Format & 7) <= 4);
        return size_t{1} << (Format & 7);
    }

    constexpr size_t IndexElementSizeInBytes() const {
        return size_t{1} << (Format >> 6);
    }

    constexpr size_t LaneCount() const {
        const auto lanesLog2 = (Format >> 3) & 7;
        assert(lanesLog2 <= 5);
        return size_t{1} << lanesLog2;
    }

    constexpr size_t SizeInBytes() const {
        return ElementSizeInBytes() * LaneCount();
    }
};

std::ostream& operator<<(std::ostream& os, const TVMInstr& instr);

static_assert(sizeof(TVMValue) == 8, "VM operand values must be eight bytes");
static_assert(sizeof(TVMInstr) == 32, "TVMInstr must be 32 bytes");

} // namespace NIR
} // namespace NQumir
