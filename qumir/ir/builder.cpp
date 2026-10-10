#include "builder.h"
#include "debug_data.h"

#include <iostream>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

namespace {
std::ostream& Print_(std::ostream& out, const TTmp& t, int typeId, const TModule& module) {
    out << "tmp(" << t.Idx;
    if (typeId >= 0) {
        out << ",";
        const TTypeTable& tt = module.Types;
        tt.Print(out, typeId);
    }
    out << ")";
    return out;
}

std::ostream& Print_(std::ostream& out, const TSlot& s, int typeId, const TModule& module) {
    out << "slot(" << s.Idx;
    if (typeId >= 0) {
        out << ",";
        const TTypeTable& tt = module.Types;
        tt.Print(out, typeId);
    }
    out << ")";
    return out;
}

std::ostream& Print_(std::ostream& out, const TLocal& l, int typeId, const TModule& module) {
    out << "local(" << l.Idx;
    if (typeId >= 0) {
        out << ",";
        const TTypeTable& tt = module.Types;
        tt.Print(out, typeId);
    }
    out << ")";
    return out;
}

std::ostream& Print_(std::ostream& out, const TLabel& l, int, const TModule& module) {
    out << "label(" << l.Idx << ")";
    return out;
}

std::ostream& Print_(std::ostream& out, const TImm& i, int typeId, const TModule& module) {
    bool isCall = typeId < 0;
    if (isCall) {
        if (module.SymIdToFuncIdx.find(i.Value) != module.SymIdToFuncIdx.end()) {
            out << module.Functions[module.SymIdToFuncIdx.at(i.Value)].Name;
            return out;
        }
        if (module.SymIdToExtFuncIdx.find(i.Value) != module.SymIdToExtFuncIdx.end()) {
            out << module.ExternalFunctions[module.SymIdToExtFuncIdx.at(i.Value)].Name;
            return out;
        }
    }
    out << "imm(" << i.Value;
    if (typeId >= 0) {
        out << ",";
        const TTypeTable& tt = module.Types;
        tt.Print(out, typeId);
    }
    out << ")";
    return out;
}

std::string Escape(const std::string& s) {
    std::string res;
    for (char c : s) {
        switch (c) {
            case '\n': res += "\\n"; break;
            case '\t': res += "\\t"; break;
            case '\r': res += "\\r"; break;
            case '\"': res += "\\\""; break;
            case '\\': res += "\\\\"; break;
            default:
                if (isprint(static_cast<unsigned char>(c))) {
                    res += c;
                } else {
                    char buf[5];
                    snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                    res += buf;
                }
        }
    }
    return res;
}

} // namespace

std::ostream& operator<<(std::ostream& out, TOp op) {
    // in code to string
    if (op.Code < 256) {
        out << (char)op.Code;
    } else {
        // multi-char operator
        int64_t code = op.Code;
        std::string str;
        while (code > 0) {
            str = (char)(code & 0xFF) + str;
            code >>= 8;
        }
        out << str;
    }
    return out;
}

void TFunction::Print(std::ostream& out, const TModule& module) const
{
    out << "function " << Name << " (";
    for (size_t i = 0; i < ArgLocals.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "local(" << ArgLocals[i].Idx << ")";
    }
    out << ") { ; ";
    module.Types.Print(out, ReturnTypeId);
    if (IsCoroutine) {
        out << " coroutine";
        if (CoroutineResultTypeId >= 0) {
            out << " result ";
            module.Types.Print(out, CoroutineResultTypeId);
        }
    }
    out << "\n";

    auto typeId = [&](auto idx, auto& cache) -> int {
        if (idx < 0) return -1;
        if (idx >= cache.size()) return -1;
        return cache[idx];
    };

    for (const auto& b : Blocks) {
        out << "  block {";
        if (!b.Succ.empty()) {
            out << " ; succ: ";
            bool first = true;
            for (const auto& s : b.Succ) {
                if (!first) out << ", ";
                Print_(out, s, -1, module);
                first = false;
            }
        }
        if (!b.Pred.empty()) {
            out << " ; pred: ";
            bool first = true;
            for (const auto& p : b.Pred) {
                if (!first) out << ", ";
                Print_(out, p, -1, module);
                first = false;
            }
        }
        out << "\n";
        out << "    label: ";
        Print_(out, b.Label, -1, module) << "\n";

        auto printInstrs = [&]<typename T>(const std::vector<T>& instrs) {
            for (const auto& i : instrs) {
                if (i.Op == "nop"_op) {
                    continue;
                }
                out << "    " << i.Op << " ";
                if (i.Dest.Idx >= 0) {
                    Print_(out, i.Dest, typeId(i.Dest.Idx, TmpTypes), module) << " = ";
                }
                int count = 0;
                if constexpr (std::is_same_v<T, TInstr>) {
                    count = i.OperandCount;
                } else if constexpr (std::is_same_v<T, TPhi>) {
                    count = i.Operands.size();
                }
                bool isCall = (i.Op == "call"_op || i.Op == "await"_op);
                for (int j = 0; j < count; j++) {
                    const auto& o = i.Operands[j];
                    switch (o.Type) {
                        case TOperand::EType::Tmp:
                            Print_(out, o.Tmp, typeId(o.Tmp.Idx, TmpTypes), module) << " ";
                            break;
                        case TOperand::EType::Slot:
                            Print_(out, o.Slot, typeId(o.Slot.Idx, module.GlobalTypes), module) << " ";
                            break;
                        case TOperand::EType::Local:
                            Print_(out, o.Local, typeId(o.Local.Idx, LocalTypes), module) << " ";
                            break;
                        case TOperand::EType::Label:
                            Print_(out, o.Label, -1, module) << " ";
                            break;
                        case TOperand::EType::Imm:
                            Print_(out, o.Imm, o.Imm.TypeId, module) << " ";
                            break;
                    }
                }
                out << "\n";
            }
        };
        printInstrs(b.Phis);
        printInstrs(b.Instrs);
        out << "  }\n";
    }
    out << "}\n";
}

int TFunction::GetTmpType(int tmpId) const {
    if (tmpId < 0 || tmpId >= (int)TmpTypes.size()) {
        return -1;
    }
    return TmpTypes[tmpId];
}

int TFunction::GetType(TTmp tmp) const {
    if (tmp.Idx < 0 || tmp.Idx >= (int)TmpTypes.size()) {
        return -1;
    }
    return TmpTypes[tmp.Idx];
}

void TFunction::SetType(TTmp tmp, int typeId) {
    if (tmp.Idx < 0) {
        throw std::out_of_range("Invalid temporary index");
    }

    if (tmp.Idx >= (int)TmpTypes.size()) {
        TmpTypes.resize(tmp.Idx + 1, -1);
    }
    TmpTypes[tmp.Idx] = typeId;
}

void TModule::Print(std::ostream& out) const
{
    for (size_t i = 0; i < VectorShuffleMasks.size(); ++i) {
        out << "; shuffle_mask " << i << ":";
        for (int index : VectorShuffleMasks[i]) {
            out << " " << index;
        }
        out << "\n";
    }
    if (DebugOptions.EmitDebugPoints) {
        out << "; qumir.debug " << SerializeDebugData(*this) << "\n";
    }
    for (const auto& f : Functions) {
        f.Print(out, *this);
    }
}

TFunction* TModule::GetFunctionByName(const std::string& name)
{
    for (auto& f : Functions) {
        if (f.Name == name) {
            return &f;
        }
    }
    return nullptr;
}

TFunction* TModule::GetEntryPoint() {
    auto* f = GetFunctionByName("<main>");
    if (f) {
        return f;
    }
    for (auto& f : Functions) {
        if (f.Name.substr(0, 2) == "__" || f.Name.substr(0, 2) == "$$") {
            // skip generated functions
            continue;
        }
        if (f.ArgLocals.empty()) {
            return &f;
        }
    }
    return nullptr;
}


TBuilder::TBuilder(TModule& m)
    : Module(m)
{
}

int TBuilder::NewFunction(std::string name, std::vector<TLocal> args, int symId) {
    auto maybeIdx = Module.SymIdToFuncIdx.find(symId);
    if (maybeIdx != Module.SymIdToFuncIdx.end()) {
        // redefine function
        Module.Functions[maybeIdx->second] = {
            .Name = name,
            .ArgLocals = args,
            .Blocks = {},
            .SymId = symId,
            .UniqueId = NextUniqueFunctionId++,
            .NextTmpIdx = 0
        };
        CurrentFunction = &Module.Functions[maybeIdx->second];
        NewBlock();
        CurrentBlock = &CurrentFunction->Blocks.back();
        return maybeIdx->second;
    } else {
        Module.Functions.push_back({
            .Name = name,
            .ArgLocals = args,
            .Blocks = {},
            .SymId = symId,
            .UniqueId = NextUniqueFunctionId++
        });
        Module.SymIdToFuncIdx[symId] = Module.Functions.size() - 1;
        CurrentFunction = &Module.Functions.back();
        NewBlock();
        CurrentBlock = &CurrentFunction->Blocks.back();
        return Module.Functions.size() - 1;
    }
}

std::pair<TLabel, int> TBuilder::NewBlock(TLabel label) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    CurrentFunction->Blocks.push_back({
        .Label = label.Idx >= 0 ? label : NewLabel(),
        .Instrs = {}
    });
    CurrentBlock = &CurrentFunction->Blocks.back();
    CurrentFunction->LabelToBlockIdx[CurrentBlock->Label] = CurrentFunction->Blocks.size() - 1;
    return {CurrentBlock->Label, CurrentFunction->Blocks.size() - 1};
}

int TBuilder::CurrentBlockIdx() const
{
    if (!CurrentFunction || !CurrentBlock) {
        throw std::runtime_error("No current function or block");
    }

    return CurrentBlock - CurrentFunction->Blocks.data();
}

TLabel TBuilder::CurrentBlockLabel() const {
    if (!CurrentBlock) {
        throw std::runtime_error("No current block");
    }
    return CurrentBlock->Label;
}

void TBuilder::SetCurrentBlock(int idx) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    if (idx == -1) {
        idx = CurrentFunction->Blocks.size() - 1;
    }
    if (idx < 0 || idx >= CurrentFunction->Blocks.size()) {
        throw std::runtime_error("Block index out of range");
    }
    CurrentBlock = &CurrentFunction->Blocks[idx];
}

void TBuilder::SetCurrentBlock(TLabel label) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    auto it = CurrentFunction->LabelToBlockIdx.find(label);
    if (it == CurrentFunction->LabelToBlockIdx.end()) {
        throw std::runtime_error("No block with the given label in current function");
    }
    SetCurrentBlock(it->second);
}

int TBuilder::CurrentFunctionIdx() const
{
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }

    return CurrentFunction - Module.Functions.data();
}

void TBuilder::SetCurrentFunction(int idx) {
    if (Module.Functions.empty()) {
        throw std::runtime_error("No functions in module");
    }
    if (idx == -1) {
        idx = Module.Functions.size() - 1;
    }
    if (idx < 0 || idx >= Module.Functions.size()) {
        throw std::runtime_error("Function index out of range");
    }
    CurrentFunction = &Module.Functions[idx];
    if (CurrentFunction->Blocks.empty()) {
        NewBlock();
    }
    CurrentBlock = &CurrentFunction->Blocks.back();
}

void TBuilder::SetScopeDebugInfo(int scopeId, int parentScopeId) {
    if (!CurrentFunction) {
        return;
    }
    if (scopeId < 0) {
        return;
    }
    if (CurrentFunction->ScopeParents.size() <= scopeId) {
        CurrentFunction->ScopeParents.resize(scopeId + 1, -1);
    }
    CurrentFunction->ScopeParents[scopeId] = parentScopeId;
}

TTmp TBuilder::NewTmp() {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    return TTmp{CurrentFunction->NextTmpIdx++};
}

TLabel TBuilder::NewLabel() {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    return TLabel{CurrentFunction->NextLabelIdx++};
}

TTmp TBuilder::Emit1(TOp op, std::span<const TOperand> operands, const TInstrDebugInfo& debugInfo) {
    if (!CurrentBlock) {
        throw std::runtime_error("No current block");
    }
    auto t = NewTmp();
    if (op == "phi"_op) {
        if (!CurrentBlock->Instrs.empty()) {
            throw std::runtime_error("Phi instructions must be emitted before other instructions in the block");
        }
        CurrentBlock->Phis.push_back(TPhi {
            .Op = op,
            .Dest = t,
            .Operands = {operands.begin(), operands.end()},
        });
    } else {
        Emit0(op, operands, debugInfo);
        CurrentBlock->Instrs.back().Dest = t;
    }
    return t;
}

void TBuilder::Emit0(TOp op, std::span<const TOperand> operands, const TInstrDebugInfo& debugInfo) {
    if (!CurrentBlock) {
        throw std::runtime_error("No current block");
    }
    CurrentBlock->Instrs.push_back(TInstr {
        .Op = op
    });
    auto& instr = CurrentBlock->Instrs.back();
    instr.OperandCount = 0;
    for (size_t i = 0; i < operands.size() && i < instr.Operands.size(); ++i) {
        instr.Operands[i] = *(operands.begin() + i);
        instr.OperandCount++;
    }

    if (debugInfo) {
        if (CurrentBlock->InstrDebugInfo.size() < CurrentBlock->Instrs.size()) {
            CurrentBlock->InstrDebugInfo.resize(CurrentBlock->Instrs.size());
        }
        CurrentBlock->InstrDebugInfo[CurrentBlock->Instrs.size() - 1] = debugInfo;
    }
}

bool TBuilder::IsCurrentBlockTerminated() const {
    if (!CurrentBlock) {
        throw std::runtime_error("No current block");
    }
    if (CurrentBlock->Instrs.empty()) return false;
    const auto& last = CurrentBlock->Instrs.back();
    return last.Op == "jmp"_op || last.Op == "ret"_op || last.Op == "cmp"_op;
}

void TBuilder::SetType(TTmp tmp, int typeId) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    if (tmp.Idx < 0) {
        throw std::runtime_error("Negative tmp index");
    }
    if (tmp.Idx >= CurrentFunction->TmpTypes.size()) {
        CurrentFunction->TmpTypes.resize(tmp.Idx + 1, -1);
    }
    CurrentFunction->TmpTypes[tmp.Idx] = typeId;
}

void TBuilder::SetType(TLocal local, int typeId, const TLocalVarDebugInfo& debugInfo) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    if (local.Idx < 0) {
        throw std::runtime_error("Negative local index");
    }
    if (local.Idx >= CurrentFunction->LocalTypes.size()) {
        CurrentFunction->LocalTypes.resize(local.Idx + 1, -1);
    }
    CurrentFunction->LocalTypes[local.Idx] = typeId;
    if (debugInfo) {
        if (local.Idx >= CurrentFunction->LocalDebugInfo.size()) {
            CurrentFunction->LocalDebugInfo.resize(local.Idx + 1);
        }
        CurrentFunction->LocalDebugInfo[local.Idx] = debugInfo;
    }
}

void TBuilder::ReserveLocals(int count) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    if (count < 0) {
        throw std::runtime_error("Negative local count");
    }
    if (count > CurrentFunction->LocalTypes.size()) {
        CurrentFunction->LocalTypes.resize(count, -1);
    }
}

TLocal TBuilder::AllocLocal(int typeId, const TLocalVarDebugInfo& debugInfo) {
    int idx = (int)CurrentFunction->LocalTypes.size();
    SetType(TLocal{idx}, typeId, debugInfo);
    return TLocal{idx};
}

int TBuilder::GetType(TTmp tmp) const {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    return CurrentFunction->GetTmpType(tmp.Idx);
}

void TBuilder::UnifyTypes(TTmp left, TTmp right) {
    auto leftTypeId = GetType(left);
    auto rightTypeId = GetType(right);
    if (leftTypeId != rightTypeId) {
        // common type
        auto unified = Module.Types.Unify(leftTypeId, rightTypeId);
        SetType(left, unified);
        SetType(right, unified);
    }
}

void TBuilder::SetReturnType(int typeId) {
    if (!CurrentFunction) {
        throw std::runtime_error("No current function");
    }
    CurrentFunction->ReturnTypeId = typeId;
}

int TBuilder::StringLiteral(const std::string& str) {
    int id = (int)Module.StringLiteralsSet.size();
    auto [it, flag] = Module.StringLiteralsSet.emplace(str, id);
    if (flag) {
        Module.StringLiterals.push_back(str);
    }
    return it->second;
}

} // namespace NIR
} // namespace NQumir
