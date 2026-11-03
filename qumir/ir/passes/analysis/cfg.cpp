#include "cfg.h"
#include <qumir/ir/builder.h>

#include <cassert>
#include <set>
#include <stack>
#include <algorithm>


namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

void BuildCfg(TFunction& function)
{
    if (function.CfgBuilt) {
        return;
    }
    function.CfgBuilt = true;
    function.Label2Idx.assign(function.Blocks.size(), -1);

    int idx = 0;
    for (auto& block : function.Blocks) {
        block.Succ.clear();
        block.Pred.clear();
        // Passes index blocks by label, so labels must be a permutation of 0..N-1.
        assert(block.Label.Idx >= 0 && block.Label.Idx < (int)function.Label2Idx.size()
            && function.Label2Idx[block.Label.Idx] == -1);
        function.Label2Idx[block.Label.Idx] = idx++;
    }

    for (auto& block : function.Blocks) {
        if (block.Instrs.empty()) {
            throw std::runtime_error("Block has no instructions");
        }
        const auto& lastInstr = block.Instrs.back();
        switch (lastInstr.Op) {
            case "jmp"_op:
                if (lastInstr.OperandCount != 1) {
                    throw std::runtime_error("Jmp instruction must have exactly one operand");
                }
                if (lastInstr.Operands[0].Type != TOperand::EType::Label) {
                    throw std::runtime_error("Jmp instruction operand must be a label");
                }
                block.Succ.push_back(lastInstr.Operands[0].Label);
                break;
            case "cmp"_op:
                if (lastInstr.OperandCount != 3) {
                    throw std::runtime_error("Cmp instruction must have exactly three operands");
                }
                if (lastInstr.Operands[1].Type != TOperand::EType::Label ||
                    lastInstr.Operands[2].Type != TOperand::EType::Label)
                {
                    throw std::runtime_error("Cmp instruction operands must be labels");
                }
                block.Succ.push_back(lastInstr.Operands[1].Label);
                block.Succ.push_back(lastInstr.Operands[2].Label);
                break;
            case "ret"_op:
                break;
            default:
                throw std::runtime_error("Block does not end with a terminator instruction");
        }
    }

    for (auto& block : function.Blocks) {
        for (const auto& succLabel : block.Succ) {
            auto idx = function.Label2Idx[succLabel.Idx];
            function.Blocks[idx].Pred.push_back(block.Label);
        }
    }
}

void BuildCfg(TModule& module) {
    for (auto& func : module.Functions) {
        BuildCfg(func);
    }
}

std::vector<TLabel> ComputeRPO(const TFunction& function) {
    std::vector<TLabel> rpo;
    std::set<TLabel> seen;
    std::stack<TLabel> stack;

    const std::vector<TBlock>& blocks = function.Blocks;

    auto dfs = [&](TLabel label, auto&& dfs_ref) -> void {
        seen.insert(label);
        auto idx = function.Label2Idx[label.Idx];
        for (auto succ : blocks[idx].Succ) {
            if (!seen.contains(succ)) {
                dfs_ref(succ, dfs_ref);
            }
        }
        rpo.push_back(label);
    };

    dfs({0}, dfs);
    std::reverse(rpo.begin(), rpo.end());
    return rpo;
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir