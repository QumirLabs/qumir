#include "lower_allocated_phis.h"

#include <qumir/ir/passes/analysis/cfg.h>

#include <algorithm>

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
    auto offset = [&](TTmp tmp) { return allocation.Slots.at(tmp.Idx).Offset; };
    for (auto& [edge, copies] : edges) {
        std::erase_if(copies, [&](const TCopy& copy) {
            return copy.second.Type == TOperand::EType::Tmp && offset(copy.first) == offset(copy.second.Tmp);
        });
        std::vector<TInstr> moves;
        auto emit = [&](TTmp dest, TOperand source) {
            moves.push_back({.Op = "mov"_op, .Dest = dest, .Operands = {source}, .OperandCount = 1});
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
            const TLabel label{function.NextLabelIdx++};
            auto& terminator = predecessor.Instrs.back();
            for (size_t i = 0; i < terminator.OperandCount; ++i) {
                if (terminator.Operands[i] == TOperand{edge.second}) {
                    terminator.Operands[i] = label;
                }
            }
            moves.push_back({.Op = "jmp"_op, .Operands = {edge.second}, .OperandCount = 1});
            std::vector<TInstrDebugInfo> debugInfo;
            if (!predecessor.InstrDebugInfo.empty()) {
                debugInfo.resize(moves.size());
                debugInfo.back() = predecessor.InstrDebugInfo.back();
            }
            function.Blocks.push_back({.Label = label, .Instrs = std::move(moves), .InstrDebugInfo = std::move(debugInfo)});
        }
    }
    function.CfgBuilt = false;
    BuildCfg(function);
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
