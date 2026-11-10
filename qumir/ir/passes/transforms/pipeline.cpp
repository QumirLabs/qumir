#include "pipeline.h"

#include <qumir/ir/passes/transforms/locals2ssa.h>
#include <qumir/ir/passes/transforms/const_fold.h>
#include <qumir/ir/passes/transforms/inline_calls.h>

#include <algorithm>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

void Pipeline(TFunction& function, TModule& module) {
    PromoteLocalsToSSA(function, module);
    ConstFold(function, module);
    // remove str_release(nullptr)
    // TODO: dont'generate them in the first place
    auto strDtor = std::find_if(
        module.ExternalFunctions.begin(),
        module.ExternalFunctions.end(),
        [](const TExternalFunction& f) {
            return f.Name == "str_release";
        }
    );
    if (strDtor != module.ExternalFunctions.end()) {
        int strDtorSymId = strDtor->SymId;
        for (auto& block : function.Blocks) {
            for (int i = 1; i < (int)block.Instrs.size(); ++i) {
                auto& instr = block.Instrs[i];
                auto& prevInstr = block.Instrs[i-1];
                if (instr.Op == "call"_op && instr.Operands[0].Imm.Value == strDtor->SymId
                    && prevInstr.Op == "arg"_op
                    && prevInstr.Operands[0].Type == TOperand::EType::Imm
                    && prevInstr.Operands[0].Imm.Value == 0)
                {
                    prevInstr.Clear();
                    instr.Clear();
                }
            }
        }
    }

    // remove nops
    for (auto& block : function.Blocks) {
        block.Instrs.erase(
            std::remove_if(
                block.Instrs.begin(),
                block.Instrs.end(),
                [](const TInstr& instr) {
                    return instr.Op == "nop"_op;
                }
            ),
            block.Instrs.end()
        );
    }
}

void Pipeline(TModule& module) {
    InlineCalls(module);
    for (auto& function : module.Functions) {
        Pipeline(function, module);
    }
}


} // namespace NPasses
} // namespace NIR
} // namespace NQumir
