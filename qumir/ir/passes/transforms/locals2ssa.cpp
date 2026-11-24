#include "locals2ssa.h"
#include "qumir/ir/builder.h"

#include <qumir/ir/passes/analysis/cfg.h>
#include <unordered_set>
#include <utility>
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

// A phi by block and position, not a TPhi*: appending a phi moves the others of its block,
// but the pass never erases phis, so the indices stay valid.
struct TPhiRef {
    int Block;
    int Index;
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

    TPhi* MaterializePhiInstr(TLabel blockLabel, const PhiInfo& ph) {
        const int blockIdx = Function.GetBlockIdx(blockLabel);
        auto& block = Function.Blocks[blockIdx];
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
        auto& phi = block.Phis.emplace_back(instr);
        AddPhiUsers(blockIdx, (int)block.Phis.size() - 1);
        return &phi;
    }

    void AddPhiUsers(int blockIdx, int phiIdx) {
        for (const auto& op : Function.Blocks[blockIdx].Phis[phiIdx].Operands) {
            if (op.Type == TOperand::EType::Tmp) {
                PhiUsers[op.Tmp.Idx].push_back({blockIdx, phiIdx});
            }
        }
    }

    // Follows removed trivial phis and promoted loads to the value that replaced them.
    // Path compression keeps repeated lookups cheap.
    TOperand Resolve(TOperand value) {
        auto root = value;
        while (root.Type == TOperand::EType::Tmp) {
            auto it = Replaced.find(root.Tmp.Idx);
            if (it == Replaced.end()) {
                break;
            }
            root = it->second;
        }
        while (!(value == root)) {
            value = std::exchange(Replaced.find(value.Tmp.Idx)->second, root);
        }
        return root;
    }

    // Braun et al. 2013 (doi:10.1007/978-3-642-37051-9_6) reroute all uses at once through def-use
    // chains. Without them, Resolve() follows the replacement, phi users move to `to` for rechecks,
    // and Run() rewrites all operands at the end.
    std::vector<TPhiRef> Replace(TTmp from, TOperand to) {
        Replaced[from.Idx] = to;
        auto users = PhiUsers.extract(from.Idx);
        if (users.empty()) {
            return {};
        }
        if (to.Type == TOperand::EType::Tmp) {
            auto& toUsers = PhiUsers[to.Tmp.Idx];
            toUsers.insert(toUsers.end(), users.mapped().begin(), users.mapped().end());
        }
        return std::move(users.mapped());
    }

    std::optional<TOperand> MaybeCurrent(int localIdx, TLabel blockLabel) {
        auto it = CurrentDef.find(localIdx);
        if (it != CurrentDef.end()) {
            auto it2 = it->second.find(blockLabel);
            if (it2 != it->second.end()) {
                return Resolve(it2->second);
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
            auto op = Resolve(phi.Operands[i]);
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

        // Removed before its users: a user that becomes trivial can lead back
        // here, and `same` itself can be removed on the way.
        auto phiUsers = Replace(phi.Dest, *same);
        phi.Clear();

        // Try to recursively remove all phi users, which might have become trivial
        for (auto [blockIdx, phiIdx] : phiUsers) {
            auto& use = Function.Blocks[blockIdx].Phis[phiIdx];
            if (use.Op == "phi"_op) {
                tryRemoveTrivialPhi(use);
            }
        }
        return Resolve(*same);
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
        // Phis that exist before the pass (if-expressions, short-circuit operators) are users too.
        for (int b = 0; b < (int)Function.Blocks.size(); ++b) {
            for (int i = 0; i < (int)Function.Blocks[b].Phis.size(); ++i) {
                AddPhiUsers(b, i);
            }
        }
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
                            auto valueTmp = Resolve(instr.Operands[1]);
                            if (IsStructLocal(localIdx) && !IsStructValueOperand(valueTmp)) {
                                // Pointer-backed struct stores stay in memory.
                                break;
                            }
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
                            Replace(oldDest, valueTmp);
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

        // One linear rewrite of all operands instead of a function scan per replacement.
        for (auto& block : Function.Blocks) {
            for (auto& phi : block.Phis) {
                for (auto& op : phi.Operands) {
                    op = Resolve(op);
                }
            }
            for (auto& instr : block.Instrs) {
                for (int i = 0; i < instr.OperandCount; ++i) {
                    instr.Operands[i] = Resolve(instr.Operands[i]);
                }
            }
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
    // Removed trivial phi or promoted load -> the operand that replaced it
    std::unordered_map<int, TOperand> Replaced;
    // Tmp -> phis that use it, rechecked when the tmp is replaced
    std::unordered_map<int, std::vector<TPhiRef>> PhiUsers;
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
