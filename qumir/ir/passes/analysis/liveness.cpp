#include "liveness.h"

#include <qumir/ir/passes/analysis/cfg.h>

#include <algorithm>
#include <iterator>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

namespace {

TLiveSet Union(const TLiveSet& a, const TLiveSet& b) {
    TLiveSet out;
    out.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

void SortUnique(TLiveSet& set) {
    std::sort(set.begin(), set.end());
    set.erase(std::unique(set.begin(), set.end()), set.end());
}

} // namespace

std::vector<TLiveSet> ComputeLiveOut(const TFunction& function) {
    const auto count = function.Blocks.size();
    // phiUses[b]: sources that phis of b's successors take on the edges from b.
    std::vector<TLiveSet> uses(count), defs(count), phiUses(count), liveIn(count), liveOut(count);
    std::vector<int> definedIn; // tmp -> last block that defines it
    auto define = [&](int tmp, size_t b) {
        if (tmp >= (int)definedIn.size()) {
            definedIn.resize(tmp + 1, -1);
        }
        definedIn[tmp] = b;
        defs[b].push_back(tmp);
    };
    for (size_t b = 0; b < count; ++b) {
        const auto& block = function.Blocks[b];
        for (const auto& phi : block.Phis) {
            if (phi.Op == "phi"_op) {
                define(phi.Dest.Idx, b);
            }
        }
        for (const auto& instr : block.Instrs) {
            for (size_t i = 0; i < instr.OperandCount; ++i) {
                const auto& operand = instr.Operands[i];
                if (operand.Type == TOperand::EType::Tmp && operand.Tmp.Idx >= 0
                    && (operand.Tmp.Idx >= (int)definedIn.size() || definedIn[operand.Tmp.Idx] != (int)b))
                {
                    uses[b].push_back(operand.Tmp.Idx);
                }
            }
            if (instr.Dest.Idx >= 0) {
                define(instr.Dest.Idx, b);
            }
        }
        for (auto successor : block.Succ) {
            for (const auto& phi : function.Blocks[function.GetBlockIdx(successor)].Phis) {
                if (phi.Op != "phi"_op) {
                    continue;
                }
                for (size_t i = 0; i < phi.Operands.size(); i += 2) {
                    const auto& source = phi.Operands[i];
                    if (phi.Operands[i + 1].Label == block.Label
                        && source.Type == TOperand::EType::Tmp && source.Tmp.Idx >= 0)
                    {
                        phiUses[b].push_back(source.Tmp.Idx);
                    }
                }
            }
        }
        SortUnique(uses[b]);
        SortUnique(defs[b]);
        SortUnique(phiUses[b]);
    }
    // Post-order visits successors first. Blocks unreachable from the entry never run
    // and keep empty sets.
    auto order = ComputeRPO(function);
    std::reverse(order.begin(), order.end());
    bool changed;
    do {
        changed = false;
        for (const auto& label : order) {
            const auto b = function.GetBlockIdx(label);
            auto out = phiUses[b];
            for (auto successor : function.Blocks[b].Succ) {
                out = Union(out, liveIn[function.GetBlockIdx(successor)]);
            }
            TLiveSet through;
            std::set_difference(out.begin(), out.end(), defs[b].begin(), defs[b].end(), std::back_inserter(through));
            auto in = Union(uses[b], through);
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
