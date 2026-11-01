#include "liveness.h"

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

std::vector<TLiveSet> ComputeLiveOut(const TFunction& function) {
    const auto count = function.Blocks.size();
    std::vector<TLiveSet> uses(count), defs(count), liveIn(count), liveOut(count);
    for (size_t b = 0; b < count; ++b) {
        const auto& block = function.Blocks[b];
        for (const auto& phi : block.Phis) {
            if (phi.Op == "phi"_op) {
                defs[b].insert(phi.Dest.Idx);
            }
        }
        for (const auto& instr : block.Instrs) {
            for (size_t i = 0; i < instr.OperandCount; ++i) {
                const auto& operand = instr.Operands[i];
                if (operand.Type == TOperand::EType::Tmp && operand.Tmp.Idx >= 0
                    && !defs[b].contains(operand.Tmp.Idx))
                {
                    uses[b].insert(operand.Tmp.Idx);
                }
            }
            if (instr.Dest.Idx >= 0) {
                defs[b].insert(instr.Dest.Idx);
            }
        }
    }
    bool changed;
    do {
        changed = false;
        for (size_t b = count; b-- > 0;) {
            TLiveSet out;
            const auto& block = function.Blocks[b];
            for (auto successor : block.Succ) {
                const auto s = function.GetBlockIdx(successor);
                out.insert(liveIn[s].begin(), liveIn[s].end());
                for (const auto& phi : function.Blocks[s].Phis) {
                    if (phi.Op != "phi"_op) {
                        continue;
                    }
                    for (size_t i = 0; i < phi.Operands.size(); i += 2) {
                        const auto& source = phi.Operands[i];
                        if (phi.Operands[i + 1].Label == block.Label
                            && source.Type == TOperand::EType::Tmp && source.Tmp.Idx >= 0)
                        {
                            out.insert(source.Tmp.Idx);
                        }
                    }
                }
            }
            auto in = uses[b];
            for (int tmp : out) {
                if (!defs[b].contains(tmp)) {
                    in.insert(tmp);
                }
            }
            changed |= in != liveIn[b] || out != liveOut[b];
            liveIn[b] = std::move(in);
            liveOut[b] = std::move(out);
        }
    } while (changed);
    return liveOut;
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
