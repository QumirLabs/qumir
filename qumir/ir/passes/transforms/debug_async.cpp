#include "debug_async.h"

#include <unordered_set>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

void LowerDebugAsync(TModule& module) {
    const int futureType = module.Types.Ptr(module.Types.I(EKind::Void));
    std::unordered_set<int> promoted;
    for (auto& external : module.ExternalFunctions) {
        if (
            module.DebugOptions.EmitDebugPoints
            && external.Name == "__qumir_debug_point"
            && external.ReturnTypeId != futureType)
        {
            external.ReturnTypeId = futureType;
            promoted.insert(external.SymId);
        }
    }
    // Discover the full caller closure before changing instructions so recursive
    // cycles and forward calls use the same physical ABI on every edge.
    bool changed;
    do {
        changed = false;
        for (auto& function : module.Functions) {
            if (function.IsCoroutine) {
                continue;
            }
            bool suspends = false;
            for (const auto& block : function.Blocks) {
                for (const auto& instr : block.Instrs) {
                    suspends |= instr.Op == "await"_op
                        || (instr.Op == "call"_op && instr.Size() > 0
                            && instr.Operands[0].Type == TOperand::EType::Imm
                            && promoted.contains(instr.Operands[0].Imm.Value));
                }
            }
            if (suspends) {
                function.IsCoroutine = true;
                function.CoroutineResultTypeId = function.ReturnTypeId;
                function.ReturnTypeId = futureType;
                promoted.insert(function.SymId);
                changed = true;
            }
        }
    } while (changed);

    if (promoted.empty()) {
        return;
    }
    for (auto& function : module.Functions) {
        for (auto& block : function.Blocks) {
            std::vector<TInstr> instructions;
            std::vector<TInstrDebugInfo> locations;
            const bool hasLocations = !block.InstrDebugInfo.empty();
            for (size_t i = 0; i < block.Instrs.size(); ++i) {
                auto instr = block.Instrs[i];
                const auto location = i < block.InstrDebugInfo.size()
                    ? block.InstrDebugInfo[i]
                    : TInstrDebugInfo{};
                if (
                    instr.Op == "call"_op && instr.Size() > 0
                    && instr.Operands[0].Type == TOperand::EType::Imm
                    && promoted.contains(instr.Operands[0].Imm.Value))
                {
                    const auto result = instr.Dest;
                    const TTmp future{function.NextTmpIdx++};
                    function.SetType(future, futureType);
                    instr.Dest = future;
                    instructions.push_back(instr);
                    TInstr await{
                        .Op = "await"_op,
                        .Dest = result,
                        .Operands = {future},
                        .OperandCount = 1,
                    };
                    instructions.push_back(await);
                    if (hasLocations) {
                        locations.push_back(location);
                        locations.push_back(location);
                    }
                } else {
                    instructions.push_back(instr);
                    if (hasLocations) {
                        locations.push_back(location);
                    }
                }
            }
            block.Instrs = std::move(instructions);
            block.InstrDebugInfo = std::move(locations);
        }
    }
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
