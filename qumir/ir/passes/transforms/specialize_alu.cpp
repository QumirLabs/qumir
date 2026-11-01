#include "specialize_alu.h"

namespace NQumir {
namespace NIR {
namespace NPasses {

void SpecializeAlu(std::span<TVMInstr> code) {
    using EType = TVMOperand::EType;
    for (auto& instr : code) {
        const auto right = instr.GetOperandType(2);
        if (instr.GetOperandType(1) != EType::Tmp || (right != EType::Tmp && right != EType::Imm)) {
            continue;
        }
        switch (instr.Op) {
#define QUMIR_VM_ALU(name, type, operation) \
            case EVMOp::name: \
                instr.Op = right == EType::Imm \
                    ? EVMOp::name##RI \
                    : EVMOp::name##RR; \
                break;
#include <qumir/ir/scalar_alu.inc>
#undef QUMIR_VM_ALU
            default: break;
        }
    }
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
