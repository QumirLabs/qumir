#include "debug_async.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    // Each call edge is visited once, including recursive cycles.
    std::unordered_map<int, std::vector<int>> callers;
    std::vector<int> pending(promoted.begin(), promoted.end());
    auto promote = [&](TFunction& function) {
        if (!function.IsCoroutine) {
            function.IsCoroutine = true;
            function.CoroutineResultTypeId = function.ReturnTypeId;
            function.ReturnTypeId = futureType;
            promoted.insert(function.SymId);
            pending.push_back(function.SymId);
        }
    };
    for (int index = 0; index < static_cast<int>(module.Functions.size()); ++index) {
        auto& function = module.Functions[index];
        for (const auto& block : function.Blocks) {
            for (const auto& instr : block.Instrs) {
                if (instr.Op == "await"_op) {
                    promote(function);
                } else if (
                    instr.Op == "call"_op && instr.Size() > 0
                    && instr.Operands[0].Type == TOperand::EType::Imm)
                {
                    callers[instr.Operands[0].Imm.Value].push_back(index);
                }
            }
        }
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        if (auto it = callers.find(pending[i]); it != callers.end()) {
            for (int index : it->second) {
                promote(module.Functions[index]);
            }
        }
    }

    if (promoted.empty()) {
        return;
    }
    for (auto& function : module.Functions) {
        for (auto& block : function.Blocks) {
            for (auto& instr : block.Instrs) {
                if (
                    instr.Op == "call"_op && instr.Size() > 0
                    && instr.Operands[0].Type == TOperand::EType::Imm
                    && promoted.contains(instr.Operands[0].Imm.Value))
                {
                    instr.Op = "await"_op;
                }
            }
        }
    }
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
