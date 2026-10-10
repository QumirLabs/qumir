#include "vminstr.h"

namespace NQumir {
namespace NIR {

std::ostream& operator<<(std::ostream& os, EVMOp op) {
    switch (op) {
    case EVMOp::INeg: return os << "INeg";
    case EVMOp::INot: return os << "INot";
    case EVMOp::IBitNot: return os << "IBitNot";
    case EVMOp::IAdd: return os << "IAdd";
    case EVMOp::ISub: return os << "ISub";
    case EVMOp::IMulS: return os << "IMulS";
    case EVMOp::IDivS: return os << "IDivS";
    case EVMOp::IDivU: return os << "IDivU";
    case EVMOp::IRemS: return os << "IRemS";
    case EVMOp::IRemU: return os << "IRemU";
    case EVMOp::IAnd: return os << "IAnd";
    case EVMOp::IOr: return os << "IOr";
    case EVMOp::IXor: return os << "IXor";
    case EVMOp::IShl: return os << "IShl";
    case EVMOp::IShrS: return os << "IShrS";
    case EVMOp::IShrU: return os << "IShrU";
    case EVMOp::ICmpLTS: return os << "ICmpLTS";
    case EVMOp::ICmpLTU: return os << "ICmpLTU";
    case EVMOp::ICmpGTS: return os << "ICmpGTS";
    case EVMOp::ICmpGTU: return os << "ICmpGTU";
    case EVMOp::ICmpLES: return os << "ICmpLES";
    case EVMOp::ICmpLEU: return os << "ICmpLEU";
    case EVMOp::ICmpGES: return os << "ICmpGES";
    case EVMOp::ICmpGEU: return os << "ICmpGEU";
    case EVMOp::ICmpEQ: return os << "ICmpEQ";
    case EVMOp::ICmpNE: return os << "ICmpNE";

    case EVMOp::FNeg: return os << "FNeg";
    case EVMOp::FAdd: return os << "FAdd";
    case EVMOp::FSub: return os << "FSub";
    case EVMOp::FMul: return os << "FMul";
    case EVMOp::FDiv: return os << "FDiv";
    case EVMOp::FCmpLT: return os << "FCmpLT";
    case EVMOp::FCmpGT: return os << "FCmpGT";
    case EVMOp::FCmpLE: return os << "FCmpLE";
    case EVMOp::FCmpGE: return os << "FCmpGE";
    case EVMOp::FCmpEQ: return os << "FCmpEQ";
    case EVMOp::FCmpNE: return os << "FCmpNE";
    case EVMOp::Load: return os << "Load";
    case EVMOp::Store: return os << "Store";
    case EVMOp::Mov: return os << "Mov";
    case EVMOp::Cmov: return os << "Cmov";
    case EVMOp::I2F: return os << "I2F";
    case EVMOp::F2I: return os << "F2I";
    case EVMOp::Bitcast: return os << "Bitcast";
    case EVMOp::Jmp: return os << "Jmp";
    case EVMOp::Cmp: return os << "Cmp";
    case EVMOp::ArgTmp: return os << "ArgTmp";
    case EVMOp::ArgConst: return os << "ArgConst";
    case EVMOp::Call: return os << "Call";
    case EVMOp::ECall: return os << "ECall";
    case EVMOp::Await: return os << "Await";
    case EVMOp::AwaitVoid: return os << "AwaitVoid";
    case EVMOp::Ret: return os << "Ret";
    case EVMOp::RetVoid: return os << "RetVoid";
    case EVMOp::Ste: return os << "Ste";
    case EVMOp::Lde: return os << "Lde";
    case EVMOp::Lea: return os << "Lea";
    case EVMOp::Copy: return os << "Copy";
    case EVMOp::StructStore: return os << "StructStore";
    case EVMOp::SAlloc: return os << "SAlloc";

    case EVMOp::INeg128: return os << "INeg128";
    case EVMOp::INot128: return os << "INot128";
    case EVMOp::IBitNot128: return os << "IBitNot128";
    case EVMOp::IAdd128: return os << "IAdd128";
    case EVMOp::ISub128: return os << "ISub128";
    case EVMOp::IMul128: return os << "IMul128";
    case EVMOp::IDivS128: return os << "IDivS128";
    case EVMOp::IDivU128: return os << "IDivU128";
    case EVMOp::IRemS128: return os << "IRemS128";
    case EVMOp::IRemU128: return os << "IRemU128";
    case EVMOp::IAnd128: return os << "IAnd128";
    case EVMOp::IOr128: return os << "IOr128";
    case EVMOp::IXor128: return os << "IXor128";
    case EVMOp::IShl128: return os << "IShl128";
    case EVMOp::IShrS128: return os << "IShrS128";
    case EVMOp::IShrU128: return os << "IShrU128";
    case EVMOp::ICmpLTS128: return os << "ICmpLTS128";
    case EVMOp::ICmpLTU128: return os << "ICmpLTU128";
    case EVMOp::ICmpGTS128: return os << "ICmpGTS128";
    case EVMOp::ICmpGTU128: return os << "ICmpGTU128";
    case EVMOp::ICmpLES128: return os << "ICmpLES128";
    case EVMOp::ICmpLEU128: return os << "ICmpLEU128";
    case EVMOp::ICmpGES128: return os << "ICmpGES128";
    case EVMOp::ICmpGEU128: return os << "ICmpGEU128";
    case EVMOp::ICmpEQ128: return os << "ICmpEQ128";
    case EVMOp::ICmpNE128: return os << "ICmpNE128";
    case EVMOp::CmovS128: return os << "CmovS128";
    case EVMOp::CmovU128: return os << "CmovU128";
    case EVMOp::SExt128: return os << "SExt128";
    case EVMOp::ZExt128: return os << "ZExt128";
    case EVMOp::Trunc128: return os << "Trunc128";
    case EVMOp::I2B128: return os << "I2B128";
    case EVMOp::I2F128S: return os << "I2F128S";
    case EVMOp::I2F128U: return os << "I2F128U";
    case EVMOp::F2I128: return os << "F2I128";
    case EVMOp::Lde128: return os << "Lde128";
    case EVMOp::Ste128: return os << "Ste128";
    case EVMOp::ArgTmp128: return os << "ArgTmp128";
    case EVMOp::Ret128: return os << "Ret128";
    case EVMOp::VIAdd: return os << "VIAdd";
    case EVMOp::VISub: return os << "VISub";
    case EVMOp::VIMul: return os << "VIMul";
    case EVMOp::VFAdd: return os << "VFAdd";
    case EVMOp::VFSub: return os << "VFSub";
    case EVMOp::VFMul: return os << "VFMul";
    case EVMOp::VIndex: return os << "VIndex";
    case EVMOp::VIAnd: return os << "VIAnd";
    case EVMOp::VIOr: return os << "VIOr";
    case EVMOp::VIXor: return os << "VIXor";
    case EVMOp::VIShl: return os << "VIShl";
    case EVMOp::VIShrS: return os << "VIShrS";
    case EVMOp::VIShrU: return os << "VIShrU";
    case EVMOp::VIBitNot: return os << "VIBitNot";
    case EVMOp::VINot: return os << "VINot";
    case EVMOp::VFNot: return os << "VFNot";
    case EVMOp::VICmpLTS: return os << "VICmpLTS";
    case EVMOp::VICmpLTU: return os << "VICmpLTU";
    case EVMOp::VICmpGTS: return os << "VICmpGTS";
    case EVMOp::VICmpGTU: return os << "VICmpGTU";
    case EVMOp::VICmpLES: return os << "VICmpLES";
    case EVMOp::VICmpLEU: return os << "VICmpLEU";
    case EVMOp::VICmpGES: return os << "VICmpGES";
    case EVMOp::VICmpGEU: return os << "VICmpGEU";
    case EVMOp::VICmpEQ: return os << "VICmpEQ";
    case EVMOp::VICmpNE: return os << "VICmpNE";
    case EVMOp::VFCmpLT: return os << "VFCmpLT";
    case EVMOp::VFCmpGT: return os << "VFCmpGT";
    case EVMOp::VFCmpLE: return os << "VFCmpLE";
    case EVMOp::VFCmpGE: return os << "VFCmpGE";
    case EVMOp::VFCmpEQ: return os << "VFCmpEQ";
    case EVMOp::VFCmpNE: return os << "VFCmpNE";
    case EVMOp::VICast: return os << "VICast";
    case EVMOp::VI2F: return os << "VI2F";
    case EVMOp::VF2I: return os << "VF2I";
    case EVMOp::VFCast: return os << "VFCast";
    case EVMOp::VI2B: return os << "VI2B";
    case EVMOp::VF2B: return os << "VF2B";
    case EVMOp::VAll: return os << "VAll";
    case EVMOp::VAny: return os << "VAny";
    case EVMOp::VShuffle: return os << "VShuffle";
    default: return os << "EVMOp(" << static_cast<int>(op) << ")";
    }
}


std::ostream& operator<<(std::ostream& os, const TVMInstr& instr) {
    os << instr.Op;
    if (instr.Op == EVMOp::Load || instr.Op == EVMOp::Store || instr.Format != 3) {
        os << '<' << instr.ElementSizeInBytes() << 'x' << instr.LaneCount() << '>';
        const bool conversion = instr.Op == EVMOp::VICast || instr.Op == EVMOp::VI2F
            || instr.Op == EVMOp::VF2I || instr.Op == EVMOp::VFCast;
        if ((instr.Op == EVMOp::VIndex || conversion) && (instr.Format & TVMInstr::SignedElement)) {
            os << "[signed]";
        } else if (!conversion && (instr.Format & TVMInstr::BroadcastLeft)) {
            os << "[broadcast-left]";
        }
        if (conversion && (instr.Format & TVMInstr::SignedResult)) {
            os << "[signed-result]";
        } else if (!conversion && (instr.Format & TVMInstr::BroadcastRight)) {
            os << "[broadcast-right]";
        }
    }
    os << ' ';
    for (size_t i = 0; i < instr.Operands.size(); ++i) {
        if (instr.Operands[i].Type == TVMOperand::EType::Tmp && instr.Operands[i].Tmp.Idx >= 0) {
            os << "reg(" << instr.Operands[i].Tmp.Idx << ") ";
        } else if (instr.Operands[i].Type == TVMOperand::EType::Slot && instr.Operands[i].Slot.Idx >= 0) {
            os << "slot(" << instr.Operands[i].Slot.Idx << ") ";
        } else if (instr.Operands[i].Type == TVMOperand::EType::Imm) {
            os << "imm(" << instr.Operands[i].Imm.Value << ") ";
        }
    }
    return os;
}

} // namespace NIR
} // namespace NQumir
