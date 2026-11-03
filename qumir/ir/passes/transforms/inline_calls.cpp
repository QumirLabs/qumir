#include "inline_calls.h"

#include <qumir/ir/passes/analysis/cfg.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace NQumir {
namespace NIR {
namespace NPasses {

using namespace NLiterals;

namespace {

// Nested inline functions multiply their size; a larger body stays a call.
constexpr size_t MaxInlineInstructions = 1000;

// Passes index blocks by label, so a copied block must take the next free label.
bool HasDenseLabels(const TFunction& function) {
    std::vector<bool> seen(function.Blocks.size(), false);
    for (const auto& block : function.Blocks) {
        const int label = block.Label.Idx;
        if (label < 0 || label >= (int)seen.size() || seen[label]) {
            return false;
        }
        seen[label] = true;
    }
    return true;
}

TInstrDebugInfo DebugInfoAt(const TBlock& block, size_t index) {
    if (index < block.InstrDebugInfo.size()) {
        return block.InstrDebugInfo[index];
    }
    return {};
}

// Like the builder, keeps debug info only up to the last located instruction.
void Append(TBlock& block, const TInstr& instr, const TInstrDebugInfo& debugInfo) {
    block.Instrs.push_back(instr);
    if (debugInfo) {
        block.InstrDebugInfo.resize(block.Instrs.size());
        block.InstrDebugInfo.back() = debugInfo;
    }
}

TInstr MakeInstr(TOp op, std::initializer_list<TOperand> operands, TTmp dest = TTmp{-1}) {
    TInstr instr{.Op = op, .Dest = dest};
    for (const auto& operand : operands) {
        instr.Operands[instr.OperandCount++] = operand;
    }
    return instr;
}

int OperandType(const TFunction& function, const TOperand& operand) {
    if (operand.Type == TOperand::EType::Tmp) {
        return function.GetType(operand.Tmp);
    }
    if (operand.Type == TOperand::EType::Imm) {
        return operand.Imm.TypeId;
    }
    return -1;
}

// A call frame starts zeroed, so a local that some path reads (or takes the
// address of) before storing it must be zeroed on every inlined entry.
std::vector<bool> LocalsReadBeforeStore(TFunction& function) {
    BuildCfg(function);
    const size_t localCount = function.LocalTypes.size();
    const auto& blockByLabel = function.Label2Idx;
    auto isStore = [](const TInstr& instr) {
        return instr.Op == "stre"_op && instr.Operands[0].Type == TOperand::EType::Local;
    };

    // Locals stored on every path to the start of a block, if the block is reached.
    std::vector<std::vector<bool>> storedAtStart(function.Blocks.size());
    std::vector<bool> reached(function.Blocks.size(), false);
    std::vector<bool> entry(localCount, false);
    for (const auto& arg : function.ArgLocals) {
        entry[arg.Idx] = true;
    }
    const size_t entryBlock = blockByLabel[0];
    storedAtStart[entryBlock] = std::move(entry);
    reached[entryBlock] = true;
    std::vector<size_t> pending{entryBlock};
    while (!pending.empty()) {
        const size_t blockIndex = pending.back();
        pending.pop_back();
        auto stored = storedAtStart[blockIndex];
        for (const auto& instr : function.Blocks[blockIndex].Instrs) {
            if (isStore(instr)) {
                stored[instr.Operands[0].Local.Idx] = true;
            }
        }
        for (auto label : function.Blocks[blockIndex].Succ) {
            auto& next = storedAtStart[blockByLabel[label.Idx]];
            if (!reached[blockByLabel[label.Idx]]) {
                reached[blockByLabel[label.Idx]] = true;
                next = stored;
                pending.push_back(blockByLabel[label.Idx]);
                continue;
            }
            bool changed = false;
            for (size_t local = 0; local < localCount; ++local) {
                if (next[local] && !stored[local]) {
                    next[local] = false;
                    changed = true;
                }
            }
            if (changed) {
                pending.push_back(blockByLabel[label.Idx]);
            }
        }
    }

    std::vector<bool> result(localCount, false);
    for (size_t blockIndex = 0; blockIndex < function.Blocks.size(); ++blockIndex) {
        auto stored = storedAtStart[blockIndex];
        if (!reached[blockIndex]) {
            continue;
        }
        for (const auto& instr : function.Blocks[blockIndex].Instrs) {
            for (int i = 0; i < instr.OperandCount; ++i) {
                const auto& operand = instr.Operands[i];
                const bool storedHere = i == 0 && isStore(instr);
                if (operand.Type == TOperand::EType::Local && !storedHere && !stored[operand.Local.Idx]) {
                    result[operand.Local.Idx] = true;
                }
            }
            if (isStore(instr)) {
                stored[instr.Operands[0].Local.Idx] = true;
            }
        }
    }
    return result;
}

class TInliner {
public:
    explicit TInliner(TModule& module)
        : Module_(module)
        , Order_(module.Functions.size(), -1)
        , Low_(module.Functions.size(), -1)
        , OnStack_(module.Functions.size(), false)
        , Recursive_(module.Functions.size(), false)
        , Inlinable_(module.Functions.size(), false)
        , ZeroedLocals_(module.Functions.size())
    {}

    void Run() {
        for (int index = 0; index < (int)Module_.Functions.size(); ++index) {
            if (Order_[index] < 0) {
                Visit(index);
            }
        }
    }

private:
    std::optional<int> CalleeIndex(const TInstr& instr) const {
        if (instr.Op != "call"_op || instr.Operands[0].Type != TOperand::EType::Imm) {
            return std::nullopt;
        }
        auto it = Module_.SymIdToFuncIdx.find(instr.Operands[0].Imm.Value);
        if (it == Module_.SymIdToFuncIdx.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    // Tarjan's walk over the call graph: callees are finished first, so the body
    // copied into a caller is final, and a function on a cycle is known as one
    // before it finishes.
    void Visit(int index) {
        Order_[index] = Low_[index] = NextOrder_++;
        Stack_.push_back(index);
        OnStack_[index] = true;
        for (const auto& block : Module_.Functions[index].Blocks) {
            for (const auto& instr : block.Instrs) {
                const auto callee = CalleeIndex(instr);
                if (!callee) {
                    continue;
                }
                if (Order_[*callee] < 0) {
                    Visit(*callee);
                    Low_[index] = std::min(Low_[index], Low_[*callee]);
                } else if (OnStack_[*callee]) {
                    // The stack from the callee up is one cycle: these functions keep their calls.
                    Low_[index] = std::min(Low_[index], Order_[*callee]);
                    auto it = std::find(Stack_.begin(), Stack_.end(), *callee);
                    for (; it != Stack_.end(); ++it) {
                        Recursive_[*it] = true;
                    }
                }
            }
        }
        InlineInto(Module_.Functions[index]);
        Inlinable_[index] = !Recursive_[index] && IsInlinable(Module_.Functions[index]);
        if (Inlinable_[index]) {
            ZeroedLocals_[index] = LocalsReadBeforeStore(Module_.Functions[index]);
        }
        if (Low_[index] == Order_[index]) {
            int top = -1;
            while (top != index) {
                top = Stack_.back();
                Stack_.pop_back();
                OnStack_[top] = false;
            }
        }
    }

    bool IsInlinable(const TFunction& function) const {
        if (!function.Inline || function.IsCoroutine || function.Blocks.empty()) {
            return false;
        }
        // The head becomes a new predecessor of the copied entry, which its phis would not name.
        const auto& entry = function.Blocks[0];
        if (!HasDenseLabels(function) || entry.Label.Idx != 0 || !entry.Phis.empty()) {
            return false;
        }
        size_t size = 0;
        for (const auto& block : function.Blocks) {
            size += block.Instrs.size();
            for (const auto& instr : block.Instrs) {
                if (instr.Op == "await"_op) {
                    return false;
                }
            }
        }
        return size <= MaxInlineInstructions;
    }

    // The arguments are the `arg`s right before the call; their types must be
    // the parameter types, or an address for a struct passed by value.
    bool CanInline(const TFunction& caller, const TBlock& block, size_t callIndex, const TFunction& callee) const {
        const auto& types = Module_.Types;
        const size_t argCount = callee.ArgLocals.size();
        if (callIndex < argCount || (callIndex > argCount && block.Instrs[callIndex - argCount - 1].Op == "arg"_op)) {
            return false;
        }
        for (size_t i = 0; i < argCount; ++i) {
            const auto& arg = block.Instrs[callIndex - argCount + i];
            if (arg.Op != "arg"_op) {
                return false;
            }
            const int argType = OperandType(caller, arg.Operands[0]);
            const int paramType = callee.LocalTypes[callee.ArgLocals[i].Idx];
            if (paramType < 0) {
                return false;
            }
            const bool structAddress = types.GetKind(paramType) == EKind::Struct && types.IsPointer(argType);
            if (argType != paramType && !structAddress) {
                return false;
            }
        }
        const auto dest = block.Instrs[callIndex].Dest;
        return dest.Idx < 0 || caller.GetType(dest) == callee.ReturnTypeId;
    }

    void InlineInto(TFunction& caller) {
        if (caller.IsCoroutine || !HasDenseLabels(caller)) {
            return;
        }
        for (size_t blockIndex = 0; blockIndex < caller.Blocks.size(); ++blockIndex) {
            const auto& block = caller.Blocks[blockIndex];
            for (size_t i = 0; i < block.Instrs.size(); ++i) {
                const auto callee = CalleeIndex(block.Instrs[i]);
                if (callee && Inlinable_[*callee] && CanInline(caller, block, i, Module_.Functions[*callee])) {
                    // The copied body is final: continue with the rest of the split block.
                    blockIndex = InlineCall(caller, blockIndex, i, *callee) - 1;
                    break;
                }
            }
        }
    }

    // Splits the block at the call: the head stores the arguments and jumps to the copied
    // callee, every copied `ret` jumps to the tail. Returns the tail's block index.
    size_t InlineCall(TFunction& caller, size_t blockIndex, size_t callIndex, int calleeIndex) {
        const auto& callee = Module_.Functions[calleeIndex];
        // Earlier inlines into this caller left Succ/Pred stale; the rebuild is a no-op when current.
        BuildCfg(caller);
        TBlock block = std::move(caller.Blocks[blockIndex]);
        const TInstr call = block.Instrs[callIndex];
        const auto callDebugInfo = DebugInfoAt(block, callIndex);
        const size_t argsBegin = callIndex - callee.ArgLocals.size();

        const int localBase = caller.LocalTypes.size();
        caller.LocalTypes.insert(caller.LocalTypes.end(), callee.LocalTypes.begin(), callee.LocalTypes.end());
        if (!callee.LocalDebugInfo.empty()) {
            caller.LocalDebugInfo.resize(localBase);
            caller.LocalDebugInfo.insert(
                caller.LocalDebugInfo.end(),
                callee.LocalDebugInfo.begin(),
                callee.LocalDebugInfo.end());
        }
        // Scope ids are module-wide, so the callee's scope chain joins the caller's as is.
        // Debug points keep the callee's function: its enter/leave points make a frame.
        if (caller.ScopeParents.size() < callee.ScopeParents.size()) {
            caller.ScopeParents.resize(callee.ScopeParents.size(), -1);
        }
        for (size_t i = 0; i < callee.ScopeParents.size(); ++i) {
            if (caller.ScopeParents[i] < 0) {
                caller.ScopeParents[i] = callee.ScopeParents[i];
            }
        }
        const TLocal result{(int)caller.LocalTypes.size()};
        if (call.Dest.Idx >= 0) {
            caller.LocalTypes.push_back(callee.ReturnTypeId);
        }
        const int tmpBase = std::max<int>(caller.NextTmpIdx, caller.TmpTypes.size());
        const int calleeTmps = std::max<int>(callee.NextTmpIdx, callee.TmpTypes.size());
        caller.TmpTypes.resize(tmpBase + calleeTmps, -1);
        std::copy(callee.TmpTypes.begin(), callee.TmpTypes.end(), caller.TmpTypes.begin() + tmpBase);
        caller.NextTmpIdx = tmpBase + calleeTmps;
        const int labelBase = caller.Blocks.size();
        const TLabel tailLabel{labelBase + (int)callee.Blocks.size()};

        auto remap = [&](TOperand& operand) {
            if (operand.Type == TOperand::EType::Tmp && operand.Tmp.Idx >= 0) {
                operand.Tmp.Idx += tmpBase;
            } else if (operand.Type == TOperand::EType::Local) {
                operand.Local.Idx += localBase;
            } else if (operand.Type == TOperand::EType::Label) {
                operand.Label.Idx += labelBase;
            }
        };

        TBlock head{.Label = block.Label, .Phis = std::move(block.Phis)};
        for (size_t i = 0; i < argsBegin; ++i) {
            Append(head, block.Instrs[i], DebugInfoAt(block, i));
        }
        for (size_t i = 0; i < callee.ArgLocals.size(); ++i) {
            const TLocal param{localBase + callee.ArgLocals[i].Idx};
            Append(head, MakeInstr("stre"_op, {param, block.Instrs[argsBegin + i].Operands[0]}), DebugInfoAt(block, argsBegin + i));
        }
        const auto& zeroed = ZeroedLocals_[calleeIndex];
        for (size_t i = 0; i < zeroed.size(); ++i) {
            if (zeroed[i]) {
                EmitZero(caller, head, TLocal{localBase + (int)i}, callee.LocalTypes[i], callDebugInfo);
            }
        }
        Append(head, MakeInstr("jmp"_op, {TLabel{labelBase}}), callDebugInfo);
        caller.Blocks[blockIndex] = std::move(head);

        std::vector<TBlock> inserted;
        for (const auto& calleeBlock : callee.Blocks) {
            TBlock copy{.Label = TLabel{calleeBlock.Label.Idx + labelBase}};
            for (auto phi : calleeBlock.Phis) {
                if (phi.Dest.Idx >= 0) {
                    phi.Dest.Idx += tmpBase;
                }
                for (auto& operand : phi.Operands) {
                    remap(operand);
                }
                copy.Phis.push_back(std::move(phi));
            }
            for (size_t i = 0; i < calleeBlock.Instrs.size(); ++i) {
                auto instr = calleeBlock.Instrs[i];
                for (int j = 0; j < instr.OperandCount; ++j) {
                    remap(instr.Operands[j]);
                }
                const auto debugInfo = DebugInfoAt(calleeBlock, i);
                if (instr.Op == "ret"_op) {
                    if (call.Dest.Idx >= 0 && instr.OperandCount > 0) {
                        Append(copy, MakeInstr("stre"_op, {result, instr.Operands[0]}), debugInfo);
                    }
                    Append(copy, MakeInstr("jmp"_op, {tailLabel}), debugInfo);
                    continue;
                }
                if (instr.Dest.Idx >= 0) {
                    instr.Dest.Idx += tmpBase;
                }
                Append(copy, instr, debugInfo);
            }
            inserted.push_back(std::move(copy));
        }

        TBlock tail{.Label = tailLabel};
        if (call.Dest.Idx >= 0) {
            Append(tail, MakeInstr("load"_op, {result}, call.Dest), callDebugInfo);
        }
        for (size_t i = callIndex + 1; i < block.Instrs.size(); ++i) {
            Append(tail, block.Instrs[i], DebugInfoAt(block, i));
        }
        // The split block's successors are now reached from the tail.
        const auto successors = block.Succ;
        inserted.push_back(std::move(tail));
        const size_t tailIndex = blockIndex + inserted.size();
        caller.Blocks.insert(
            caller.Blocks.begin() + blockIndex + 1,
            std::make_move_iterator(inserted.begin()),
            std::make_move_iterator(inserted.end()));
        for (auto label : successors) {
            for (auto& successor : caller.Blocks) {
                if (successor.Label.Idx != label.Idx) {
                    continue;
                }
                for (auto& phi : successor.Phis) {
                    for (auto& operand : phi.Operands) {
                        if (operand.Type == TOperand::EType::Label && operand.Label.Idx == block.Label.Idx) {
                            operand.Label = tailLabel;
                        }
                    }
                }
            }
        }
        caller.NextLabelIdx = caller.Blocks.size();
        caller.CfgBuilt = false;
        return tailIndex;
    }

    // Zero values in the form the frontend emits them: an immediate, a pointer
    // moved from an integer, a struct copied from a zeroed stack slot.
    void EmitZero(TFunction& caller, TBlock& block, TLocal local, int typeId, const TInstrDebugInfo& debugInfo) {
        auto& types = Module_.Types;
        const int i64 = types.I(EKind::I64);
        if (types.GetKind(typeId) == EKind::Struct) {
            const TTmp zero{caller.NextTmpIdx++};
            caller.SetType(zero, types.Ptr(typeId));
            Append(block, MakeInstr("salloc"_op, {TImm{types.SizeInBytes(typeId), i64}}, zero), debugInfo);
            Append(block, MakeInstr("stre"_op, {local, zero}), debugInfo);
        } else if (types.IsPointer(typeId)) {
            const TTmp zero{caller.NextTmpIdx++};
            caller.SetType(zero, typeId);
            Append(block, MakeInstr("mov"_op, {TImm{0, i64}}, zero), debugInfo);
            Append(block, MakeInstr("stre"_op, {local, zero}), debugInfo);
        } else {
            Append(block, MakeInstr("stre"_op, {local, TImm{0, typeId}}), debugInfo);
        }
    }

    TModule& Module_;
    int NextOrder_ = 0;
    std::vector<int> Order_;
    std::vector<int> Low_;
    std::vector<int> Stack_;
    std::vector<bool> OnStack_;
    std::vector<bool> Recursive_;
    std::vector<bool> Inlinable_;
    std::vector<std::vector<bool>> ZeroedLocals_;
};

} // namespace

void InlineCalls(TModule& module) {
    TInliner(module).Run();
}

} // namespace NPasses
} // namespace NIR
} // namespace NQumir
