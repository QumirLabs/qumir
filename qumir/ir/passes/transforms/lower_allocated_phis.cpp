#include "lower_allocated_phis.h"

#include <qumir/ir/passes/analysis/cfg.h>

#include <algorithm>
#include <map>
#include <utility>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

void LowerAllocatedPhis(TFunction& function, TRegisterAllocation& allocation) {
    using TCopy = std::pair<TTmp, TOperand>;
    std::map<std::pair<TLabel, TLabel>, std::vector<TCopy>> edges;
    for (auto& block : function.Blocks) {
        for (const auto& phi : block.Phis) {
            if (phi.Op != "phi"_op) {
                continue;
            }
            for (size_t i = 0; i < phi.Operands.size(); i += 2) {
                edges[{phi.Operands[i + 1].Label, block.Label}].emplace_back(phi.Dest, phi.Operands[i]);
            }
        }
        block.Phis.clear();
    }
    std::map<std::pair<int, int>, int> scratchOffsets;
    auto offset = [&](TTmp tmp) {
        return allocation.Slots.at(tmp.Idx).Offset;
    };
    for (auto& [edge, copies] : edges) {
        std::erase_if(copies, [&](const TCopy& copy) {
            return copy.second.Type == TOperand::EType::Tmp && offset(copy.first) == offset(copy.second.Tmp);
        });
        std::vector<TInstr> moves;
        auto emit = [&](TTmp dest, TOperand source) {
            moves.push_back(MakeInstr("mov"_op, {source}, dest));
        };
        while (!copies.empty()) {
            auto ready = std::find_if(copies.begin(), copies.end(), [&](const TCopy& copy) {
                return std::none_of(copies.begin(), copies.end(), [&](const TCopy& other) {
                    return other.second.Type == TOperand::EType::Tmp
                        && offset(copy.first) == offset(other.second.Tmp);
                });
            });
            if (ready != copies.end()) {
                emit(ready->first, ready->second);
                copies.erase(ready);
                continue;
            }
            // Save one source to break a cycle. Equal-sized colors never partially overlap.
            const auto source = std::find_if(copies.begin(), copies.end(), [](const TCopy& copy) {
                return copy.second.Type == TOperand::EType::Tmp;
            })->second.Tmp;
            const auto slot = allocation.Slots.at(source.Idx);
            const auto key = std::pair{slot.Size, slot.Alignment};
            const int typeId = function.GetTmpType(source.Idx);
            const TTmp scratch{function.NextTmpIdx++};
            function.SetType(scratch, typeId);
            if (auto it = scratchOffsets.find(key); it != scratchOffsets.end()) {
                allocation.Slots[scratch.Idx] = {it->second, slot.Size, slot.Alignment};
            } else {
                scratchOffsets[key] = allocation.Reserve(scratch.Idx, slot.Size, slot.Alignment).Offset;
            }
            emit(scratch, source);
            for (auto& [dest, value] : copies) {
                if (value.Type == TOperand::EType::Tmp && offset(value.Tmp) == slot.Offset) {
                    value = scratch;
                }
            }
        }
        if (moves.empty()) {
            continue;
        }
        auto& predecessor = function.Blocks[function.GetBlockIdx(edge.first)];
        if (predecessor.Succ.size() == 1) {
            predecessor.Instrs.insert(predecessor.Instrs.end() - 1, moves.begin(), moves.end());
            if (!predecessor.InstrDebugInfo.empty()) {
                predecessor.InstrDebugInfo.resize(predecessor.Instrs.size() - moves.size());
                predecessor.InstrDebugInfo.insert(predecessor.InstrDebugInfo.end() - 1, moves.size(), TInstrDebugInfo{});
            }
        } else {
            // BuildCfg indexes blocks by label, so labels stay 0..N-1.
            const TLabel label{static_cast<int>(function.Blocks.size())};
            const size_t last = predecessor.Instrs.size() - 1;
            auto& terminator = predecessor.Instrs[last];
            for (size_t i = 0; i < terminator.OperandCount; ++i) {
                if (terminator.Operands[i] == TOperand{edge.second}) {
                    terminator.Operands[i] = label;
                }
            }
            const auto debugInfo = last < predecessor.InstrDebugInfo.size()
                ? predecessor.InstrDebugInfo[last]
                : TInstrDebugInfo{};
            TBlock split{.Label = label, .Instrs = std::move(moves)};
            AppendInstr(split, MakeInstr("jmp"_op, {edge.second}), debugInfo);
            function.Blocks.push_back(std::move(split));
        }
    }
    function.NextLabelIdx = function.Blocks.size();
    function.CfgBuilt = false;
    BuildCfg(function);
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
