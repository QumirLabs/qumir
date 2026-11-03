#include "locals2ssa.h"
#include "qumir/ir/builder.h"

#include <qumir/ir/passes/analysis/cfg.h>
#include <unordered_set>
#include <vector>

#include <iostream>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

namespace NPasses {

namespace {

struct PhiInfo {
    int Local;
    TTmp DstTmp;
    std::vector<std::pair<TOperand, TLabel>> Incoming;
};

// Implements the sealed-block SSA construction algorithm described by Braun
// et al., "Simple and Efficient Construction of Static Single Assignment Form":
// https://c9x.me/compile/bib/braun13cc.pdf
struct TSSABuilder {
    TSSABuilder(TModule& module, TFunction& function)
        : Module(module)
        , Function(function)
    {}

    void ComputeLocalPromotable() {
        LocalPromotable.assign(Function.LocalTypes.size(), true);
        for (auto& block : Function.Blocks) {
            for (auto& instr : block.Instrs) {
                // If a local's address is taken (lea local), don't promote that local to SSA.
                // Rationale: once the address escapes, the variable can be modified indirectly
                // via that pointer (pass-by-reference arguments, stores through the pointer,
                // or mutations in callees). Without alias/escape analysis we can't prove it is
                // safe to keep the value purely in SSA registers; otherwise we risk reading a
                // stale cached value after a call instead of the memory updated through the pointer.
                if (instr.Op == "lea"_op) {
                    if (instr.OperandCount >= 1 && instr.Operands[0].Type == TOperand::EType::Local) {
                        int localIdx = instr.Operands[0].Local.Idx;
                        if (localIdx >= 0 && localIdx < (int)LocalPromotable.size()) {
                            LocalPromotable[localIdx] = false;
                        }
                    }
                }
                if (instr.Op == "stre"_op
                    && instr.OperandCount == 2
                    && instr.Operands[0].Type == TOperand::EType::Local
                    && IsStructLocal(instr.Operands[0].Local.Idx)
                    && !IsStructValueOperand(instr.Operands[1]))
                {
                    int localIdx = instr.Operands[0].Local.Idx;
                    if (localIdx >= 0 && localIdx < (int)LocalPromotable.size()) {
                        LocalPromotable[localIdx] = false;
                    }
                }
            }
        }
    }

    bool IsStructLocal(int localIdx) const {
        if (localIdx < 0 || localIdx >= (int)Function.LocalTypes.size()) {
            return false;
        }
        const int typeId = Function.LocalTypes[localIdx];
        return typeId >= 0 && Module.Types.GetKind(typeId) == EKind::Struct;
    }

    bool IsStructValueOperand(const TOperand& op) const {
        const int typeId = OperandType(Function, Module, op);
        return typeId >= 0 && Module.Types.GetKind(typeId) == EKind::Struct;
    }

    void ReplaceTmpEverywhere(TOperand fromTmp, TOperand toTmp) {
        for (auto& block : Function.Blocks) {
            ReplaceTmpInBlock(block.Label, fromTmp, toTmp);
        }
    }

    void ReplaceTmpInBlock(TLabel blockLabel, TOperand fromTmp, TOperand toTmp) {
        auto& block = Function.Blocks[Function.GetBlockIdx(blockLabel)];
        auto replace = [&](auto& instrs) {
            for (auto& inst : instrs) {
                if (inst.Dest == fromTmp) {
                    inst.Clear();
                    continue;
                }
                for (int i = 0; i < inst.Size(); ++i) {
                    auto& op = inst.Operands[i];
                    if (op == fromTmp) {
                        op = toTmp;
                    }
                }
            }
        };
        replace(block.Phis);
        replace(block.Instrs);
    }

    TPhi* MaterializePhiInstr(TLabel blockLabel, const PhiInfo& ph) {
        auto& block = Function.Blocks[Function.GetBlockIdx(blockLabel)];
        TPhi instr = {"phi"_op};
        instr.Dest = ph.DstTmp;
        // Args: [ (arg0, pred0), (arg1, pred1), ... ]
        instr.Operands.reserve(ph.Incoming.size() * 2);
        bool hasUndef = false;
        int typeId = -1;
        for (const auto& [value, label] : ph.Incoming) {
            if (value.Type == TOperand::EType::Imm) {
                auto& imm = value.Imm;
                if (Module.Types.GetKind(imm.TypeId) == EKind::Undef) {
                    hasUndef = true;
                } else {
                    typeId = imm.TypeId;
                }
            } else if (value.Type == TOperand::EType::Tmp) {
                typeId = Function.GetTmpType(value.Tmp.Idx);
            }
            instr.Operands.push_back(value);
            instr.Operands.push_back(label);
        }
        if (hasUndef && typeId >= 0) {
            // Replace undefs
            for (auto& op : instr.Operands) {
                if (op.Type == TOperand::EType::Imm) {
                    auto& imm = op.Imm;
                    if (Module.Types.GetKind(imm.TypeId) == EKind::Undef) {
                        imm.TypeId = typeId;
                    }
                }
            }
        }
        return &block.Phis.emplace_back(instr);
    }

    // Follows removed trivial phis to the value that replaced them.
    TOperand Resolve(TOperand value) const {
        while (value.Type == TOperand::EType::Tmp) {
            auto it = RemovedPhis.find(value.Tmp.Idx);
            if (it == RemovedPhis.end()) {
                break;
            }
            value = it->second;
        }
        return value;
    }

    std::optional<TOperand> MaybeCurrent(int localIdx, TLabel blockLabel) {
        auto it = CurrentDef.find(localIdx);
        if (it != CurrentDef.end()) {
            auto it2 = it->second.find(blockLabel);
            if (it2 != it->second.end()) {
                return it2->second;
            }
        }
        return {};
    }

    void WriteVariable(int localIdx, TLabel blockLabel, TOperand value) {
        CurrentDef[localIdx][blockLabel] = value;
    }

    TOperand ReadVariable(int localIdx, TLabel blockLabel) {
        if (auto maybeCurrent = MaybeCurrent(localIdx, blockLabel)) {
            return *maybeCurrent;
        }

        return ReadVariableRecursive(localIdx, blockLabel);
    }

    TOperand ReadVariableRecursive(int localIdx, TLabel blockLabel) {
        TOperand result;
        auto& block = Function.Blocks[Function.GetBlockIdx(blockLabel)];
        const int numPreds = (int)block.Pred.size();

        if (numPreds == 0 || !ReachableBlocks.contains(blockLabel)) {
            // Braun et al. assume every block is reachable. A cycle of single-predecessor
            // blocks unreachable from entry would recurse forever, so cut it with undef.
            return TOperand{TImm{0, Module.Types.I(EKind::Undef)}};
        }

        if (numPreds == 1) {
            result = ReadVariable(localIdx, block.Pred.front());
        } else if (!SealedBlocks.contains(blockLabel)) {
            auto newTmp = TTmp{Function.NextTmpIdx++};
            IncompletePhis[blockLabel].push_back(PhiInfo {
                .Local = localIdx,
                .DstTmp = newTmp,
            });
            Function.SetType(newTmp, Function.LocalTypes[localIdx]);
            result = newTmp;
        } else {
            auto dst = TTmp{Function.NextTmpIdx++};
            PhiInfo phi {
                .Local = localIdx,
                .DstTmp = dst,
            };
            Function.SetType(dst, Function.LocalTypes[localIdx]);
            WriteVariable(localIdx, blockLabel, dst);
            result = AddPhiOperands(localIdx, blockLabel, phi);
        }

        WriteVariable(localIdx, blockLabel, result);
        return result;
    }

    TOperand AddPhiOperands(int localIdx, TLabel blockLabel, PhiInfo& phi) {
        auto blockIdx = Function.GetBlockIdx(blockLabel);
        for (auto pred : Function.Blocks[blockIdx].Pred) {
            phi.Incoming.push_back({ReadVariable(localIdx, pred), pred});
        }

        auto* instr = MaterializePhiInstr(blockLabel, phi);
        return tryRemoveTrivialPhi(*instr);
    }

    TOperand tryRemoveTrivialPhi(TPhi& phi) {
        std::optional<TOperand> same;
        for (int i = 0; i < (int)phi.Operands.size(); ++i) {
            auto& op = phi.Operands[i];
            if (op.Type == TOperand::EType::Label) {
                continue;
            }
            if (op == TOperand{phi.Dest}) {
                continue;
            }
            if (same.has_value()) {
                if (op == *same) {
                    continue;
                } else {
                    // not trivial
                    return TOperand{phi.Dest};
                }
            } else {
                same = op;
            }
        }
        if (!same) {
            // undef: TODO
            same = TOperand{TImm{0, Module.Types.I(EKind::Undef)}};
        }
        //
        auto [users, phiUsers] = GetUsers(phi.Dest);
        // Reroute all uses of phi to same and remove phi
        for (auto* use : users) {
            for (int i = 0; i < use->OperandCount; ++i) {
                if (use->Operands[i] == TOperand{phi.Dest}) {
                    use->Operands[i] = *same;
                }
            }
        }
        for (auto* use : phiUsers) {
            if (use == &phi) {
                continue;
            }
            for (int i = 0; i < (int)use->Operands.size(); ++i) {
                if (use->Operands[i] == TOperand{phi.Dest}) {
                    use->Operands[i] = *same;
                }
            }
        }
        for (auto& [loc, byBlock] : CurrentDef) {
            for (auto& [b, val] : byBlock) {
                if (val == TOperand{phi.Dest}) {
                    val = *same;
                }
            }
        }

        // Removed before its users: a user that becomes trivial can lead back
        // here, and `same` itself can be removed on the way.
        RemovedPhis[phi.Dest.Idx] = *same;
        phi.Clear();

        // Try to recursively remove all phi users, which might have become trivial
        for (auto* use : phiUsers) {
            if (use == &phi) {
                continue;
            }
            if (use->Op == "phi"_op) {
                tryRemoveTrivialPhi(*use);
            }
        }
        return Resolve(*same);
    }

    // TODO: optimize by caching users per tmp
    std::pair<std::vector<TInstr*>,std::vector<TPhi*>> GetUsers(TTmp tmp) {
        std::vector<TInstr*> users;
        std::vector<TPhi*> phiUsers;
        for (int i = 0; i < (int)Function.Blocks.size(); ++i) {
            auto& block = Function.Blocks[i];
            for (auto& phi : block.Phis) {
                for (int j = 0; j < (int)phi.Operands.size(); ++j) {
                    if (phi.Operands[j].Type == TOperand::EType::Tmp &&
                        phi.Operands[j].Tmp == tmp) {
                        phiUsers.push_back(&phi);
                    }
                }
            }
            for (auto& instr : block.Instrs) {
                for (int j = 0; j < instr.OperandCount; ++j) {
                    if (instr.Operands[j].Type == TOperand::EType::Tmp &&
                        instr.Operands[j].Tmp == tmp) {
                        users.push_back(&instr);
                    }
                }
            }
        }
        return {users, phiUsers};
    }

    void SealBlock(TLabel blockLabel) {
        if (SealedBlocks.contains(blockLabel)) {
            return;
        }
        SealedBlocks.insert(blockLabel);

        auto maybeIncompletePhis = IncompletePhis.find(blockLabel);
        if (maybeIncompletePhis == IncompletePhis.end()) {
            return;
        }
        auto& incompletePhis = maybeIncompletePhis->second;
        for (auto& phi : incompletePhis) {
            AddPhiOperands(phi.Local, blockLabel, phi);
        }
    }

    void Run() {
        BuildCfg(Function);
        auto rpo = ComputeRPO(Function);
        ReachableBlocks.insert(rpo.begin(), rpo.end());
        std::vector<int> openPredCount(Function.Blocks.size());
        for (auto& block : Function.Blocks) {
            openPredCount[block.Label.Idx] = block.Pred.size();
            if (openPredCount[block.Label.Idx] == 0) {
                SealBlock(block.Label);
            }
        }

        // Pre-compute which locals are safe to promote
        ComputeLocalPromotable();

        // SSA conversion
        for (auto blockLabel : rpo) {
            auto& block = Function.Blocks[Function.GetBlockIdx(blockLabel)];
            for (auto& instr : block.Instrs) {
                switch (instr.Op) {
                    case "stre"_op: {
                        if (instr.OperandCount != 2) {
                            throw std::runtime_error("Store instruction must have exactly two operands");
                        }
                        if (instr.Operands[0].Type  == TOperand::EType::Slot) {
                            break;
                        }
                        int localIdx = instr.Operands[0].Local.Idx;
                        if (localIdx >= Function.ArgLocals.size() &&
                            (localIdx < (int)LocalPromotable.size() && LocalPromotable[localIdx])) {
                            if (IsStructLocal(localIdx) && !IsStructValueOperand(instr.Operands[1])) {
                                // Pointer-backed struct stores stay in memory.
                                break;
                            }
                            auto valueTmp = instr.Operands[1];
                            instr.Clear();
                            WriteVariable(localIdx, blockLabel, valueTmp);
                        }
                        break;
                    }
                    case "load"_op: {
                        if (instr.OperandCount != 1) {
                            throw std::runtime_error("Load instruction must have exactly one operand");
                        }
                        if (instr.Operands[0].Type  == TOperand::EType::Slot) {
                            break;
                        }
                        if (instr.Operands[0].Type != TOperand::EType::Local) {
                            throw std::runtime_error("Load instruction operand must be a local");
                        }
                        int localIdx = instr.Operands[0].Local.Idx;
                        if (localIdx >= Function.ArgLocals.size() &&
                            (localIdx < (int)LocalPromotable.size() && LocalPromotable[localIdx])) {
                            auto valueTmp = ReadVariable(localIdx, blockLabel);
                            auto oldDest = instr.Dest;
                            instr.Clear();
                            ReplaceTmpEverywhere(TOperand{oldDest}, valueTmp); // TODO: check replace
                            CurrentDef[localIdx][blockLabel] = valueTmp;
                        }
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
            for (const auto& succ : block.Succ) {
                int s = succ.Idx;
                if (openPredCount[s] > 0) {
                    --openPredCount[s];
                    if (openPredCount[s] == 0) {
                        SealBlock(succ);
                    }
                }
            }
        }

        // Predecessors unreachable from entry never decrement openPredCount,
        // so some blocks may still be unsealed here. Force-seal them;
        // ReadVariable resolves contributions from such preds as `undef`.
        for (auto& block : Function.Blocks) {
            SealBlock(block.Label);
        }
    }

    TModule& Module;
    TFunction& Function;

    // local -> (block -> tmp)
    std::unordered_map<int, std::map<TLabel, TOperand>> CurrentDef;
    // block -> incomplete phis
    std::map<TLabel, std::vector<PhiInfo>> IncompletePhis;
    // block indices
    std::set<TLabel> SealedBlocks;
    // Blocks reachable from entry
    std::set<TLabel> ReachableBlocks;
    // Deferred tmp replacements collected while processing blocks (from tmp -> to operand)
    std::vector<std::pair<TOperand, TOperand>> GlobalPendingReplacements;
    // Locals that are safe to promote (no address taken)
    std::vector<char> LocalPromotable;
    // Removed trivial phi -> the operand that replaced it
    std::unordered_map<int, TOperand> RemovedPhis;
};

} // anonymous namespace

// Simple and Efficient Construction of Static Single Assignment Form
// Matthias Braun1, Sebastian Buchwald1, Sebastian Hack2, Roland Leissa2, Christoph Mallon2, and Andreas Zwinkau
void PromoteLocalsToSSA(TFunction& function, TModule& module)
{
    TSSABuilder(module, function).Run();
}

void PromoteLocalsToSSA(TModule& module) {
    for (auto& function : module.Functions) {
        PromoteLocalsToSSA(function, module);
    }
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
