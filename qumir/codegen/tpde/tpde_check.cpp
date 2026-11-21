#include "tpde_check.h"

#include <algorithm>
#include <unordered_set>

namespace NQumir {
namespace NCodeGen {

using namespace NIR;
using namespace NIR::NLiterals;

namespace NTpde {
namespace {

bool IsTerminator(TOp op) {
    return op == "jmp"_op || op == "cmp"_op || op == "ret"_op;
}

// Finds the first construct that the TPDE compilers do not implement.
struct TChecker {
    TModuleInfo& Info;
    bool CStructs;
    std::optional<std::string> Error;

    void Bad(const std::string& what) {
        if (!Error) {
            Error = what;
        }
    }

    const TTypeShape& Shape(int type) const {
        return Info.Shape(type);
    }
    const TLayout& Layout(int type) const {
        return Info.Layout(type);
    }
    bool Scalar(int type) const {
        return Layout(type).Kind == TLayout::Scalar;
    }
    bool Valued(int type) const {
        auto k = Layout(type).Kind;
        return k == TLayout::Scalar || k == TLayout::Aggregate || k == TLayout::Vector;
    }
    bool IsVector(int type) const {
        return Layout(type).Kind == TLayout::Vector;
    }
    bool IsAggregate(int type) const {
        return Layout(type).Kind == TLayout::Aggregate;
    }

    bool HasVector(int type) const {
        const auto& tt = Info.Module.Types;
        auto kind = tt.GetKind(type);
        if (kind != EKind::Struct) {
            return kind == EKind::Vec;
        }
        return std::ranges::any_of(tt.GetStructFields(type), [&](int field) {
            return HasVector(field);
        });
    }
    // The C ABI passes a vector field as a short vector, not by its lanes.
    bool CStruct(int type) const {
        return CStructs && IsAggregate(type) && !HasVector(type);
    }
    std::string VectorNote(int type) const {
        return IsAggregate(type) && HasVector(type)
            ? " (vector field)"
            : "";
    }

    std::string TypeKind(int type) const {
        if (type < 0) {
            return "untyped";
        }
        switch (Info.Module.Types.GetKind(type)) {
            case EKind::Struct: return "struct";
            case EKind::Vec: return "vector";
            case EKind::Undef: return "undef";
            default: return "other";
        }
    }

    void Function(const TFunction& fun) {
        if (fun.Blocks.empty()) {
            return;
        }
        if (fun.IsCoroutine) {
            Bad("coroutine function");
            return;
        }
        if (!Valued(fun.ReturnTypeId) && Layout(fun.ReturnTypeId).Kind != TLayout::Void) {
            Bad("return type " + TypeKind(fun.ReturnTypeId));
        }
        for (const auto& arg : fun.ArgLocals) {
            if (!Valued(fun.LocalTypes[arg.Idx])) {
                Bad("argument type " + TypeKind(fun.LocalTypes[arg.Idx]));
            }
        }
        std::vector<u8> defined(std::max<size_t>(fun.NextTmpIdx, fun.TmpTypes.size()), 0);
        auto define = [&](int tmp) {
            if (tmp < 0 || tmp >= (int)defined.size()) {
                return;
            }
            if (defined[tmp]++) {
                Bad("temporary defined twice");
            }
            int type = fun.GetTmpType(tmp);
            if (!Valued(type) && Layout(type).Kind != TLayout::Void) {
                Bad("value type " + TypeKind(type));
            }
        };
        for (const auto& block : fun.Blocks) {
            for (const auto& phi : block.Phis) {
                if (phi.Op == "phi"_op) {
                    define(phi.Dest.Idx);
                }
            }
            if (block.Instrs.empty() || !IsTerminator(block.Instrs.back().Op)) {
                Bad("block without terminator");
            }
            for (const auto& instr : block.Instrs) {
                if (instr.Op == "nop"_op) {
                    continue;
                }
                if (instr.Op != "salloc"_op) {
                    define(instr.Dest.Idx);
                }
                Instr(fun, instr);
            }
        }
    }

    void Instr(const TFunction& fun, const TInstr& instr) {
        static const std::unordered_set<uint64_t> known = {
            "+"_op, "-"_op, "*"_op, "/"_op, "%"_op, "&"_op, "|"_op, "^"_op, "<<"_op, ">>"_op,
            "&&"_op, "||"_op,
            "<"_op, "<="_op, ">"_op, ">="_op, "=="_op, "!="_op, "!"_op, "~"_op, "neg"_op,
            "lde"_op, "ste"_op, "salloc"_op, "copy"_op, "load"_op, "lea"_op, "stre"_op,
            "ret"_op, "i2b"_op, "f2b"_op, "i2f"_op, "f2i"_op, "bitcast"_op, "mov"_op,
            "arg"_op, "call"_op, "jmp"_op, "cmp"_op, "prefetch"_op,
            "select"_op, "any"_op, "all"_op, "movemask"_op, "shuffle"_op, "index"_op, "gather"_op,
        };
        const std::string name = instr.Op.ToString();
        if (!known.count(instr.Op)) {
            Bad("op " + name);
            return;
        }
        for (int i = 0; i < instr.OperandCount; ++i) {
            const auto& op = instr.Operands[i];
            if (op.Type == TOperand::EType::Imm && IsValueImm(instr, i)) {
                int t = op.Imm.TypeId;
                if (t < 0) {
                    continue;
                }
                auto k = Layout(t).Kind;
                if (
                    (k == TLayout::Aggregate || k == TLayout::Vector) &&
                    op.Imm.Value != 0 &&
                    !(instr.Op == "shuffle"_op && i == 1))
                {
                    Bad("non-zero " + TypeKind(t) + " immediate");
                }
                if (Shape(t).Class == TTypeShape::Ptr && op.Imm.Value != 0 && t != Info.Str) {
                    Bad("non-null pointer immediate");
                }
            } else if (op.Type == TOperand::EType::Local) {
                int t = fun.LocalTypes[op.Local.Idx];
                if (!Valued(t) && instr.Op != "lea"_op) {
                    Bad(name + " of " + TypeKind(t) + " local");
                }
            } else if (op.Type == TOperand::EType::Slot) {
                int t = Info.Module.GlobalTypes[op.Slot.Idx];
                if (!Valued(t) && instr.Op != "lea"_op) {
                    Bad(name + " of " + TypeKind(t) + " global");
                }
            }
        }
        int res = instr.Dest.Idx >= 0
            ? fun.GetTmpType(instr.Dest.Idx)
            : -1;
        const auto& rs = Shape(res);
        auto opType = [&](int i) {
            return OperandType(fun, Info.Module, instr.Operands[i]);
        };
        bool vecRes = instr.Dest.Idx >= 0 && IsVector(res);
        bool aggRes = instr.Dest.Idx >= 0 && IsAggregate(res);
        switch (instr.Op) {
            case "/"_op:
            case "%"_op:
                if (rs.Class == TTypeShape::Float && instr.Op == "%"_op) {
                    Bad("float %");
                }
                [[fallthrough]];
            case "+"_op:
            case "-"_op:
            case "*"_op:
            case "&"_op:
            case "|"_op:
            case "^"_op:
            case "<<"_op:
            case ">>"_op:
            case "&&"_op:
            case "||"_op: {
                bool arith = instr.Op == "+"_op || instr.Op == "-"_op || instr.Op == "*"_op || instr.Op == "/"_op;
                if (aggRes) {
                    Bad(name + " on struct");
                }
                if (vecRes) {
                    const auto& es = Shape(Layout(res).Elem);
                    if (es.Class == TTypeShape::Float && !arith) {
                        Bad("vector float " + name);
                    }
                    if ((instr.Op == "/"_op || instr.Op == "%"_op) && es.Class != TTypeShape::Float) {
                        Bad("vector integer " + name);
                    }
                    break;
                }
                if (instr.Op == "&&"_op || instr.Op == "||"_op) {
                    Bad("scalar " + name);
                }
                if (rs.Class == TTypeShape::Ptr && instr.Op != "+"_op && instr.Op != "-"_op) {
                    Bad("pointer " + name);
                }
                if (!arith && rs.Class != TTypeShape::Int) {
                    Bad(name + " on non-integer");
                }
                break;
            }
            case "<"_op:
            case "<="_op:
            case ">"_op:
            case ">="_op:
            case "=="_op:
            case "!="_op:
                if (IsAggregate(opType(0)) || IsAggregate(opType(1))) {
                    Bad("compare of struct");
                }
                if (!vecRes && (IsVector(opType(0)) || IsVector(opType(1)))) {
                    Bad("vector compare to scalar");
                }
                break;
            case "~"_op:
            case "neg"_op:
                if (aggRes || (!vecRes && rs.Class != TTypeShape::Int && rs.Class != TTypeShape::Float)) {
                    Bad(name + " on " + TypeKind(res));
                }
                break;
            case "!"_op:
            case "i2b"_op:
            case "f2b"_op:
                if (IsAggregate(opType(0)) || (IsVector(opType(0)) && !vecRes)) {
                    Bad(name + " of " + TypeKind(opType(0)));
                }
                break;
            case "mov"_op:
            case "bitcast"_op:
                if (
                    (aggRes || vecRes || IsAggregate(opType(0)) || IsVector(opType(0))) &&
                    !(vecRes && IsVector(opType(0)) && instr.Op == "mov"_op) &&
                    opType(0) != res)
                {
                    Bad(name + " of " + TypeKind(aggRes || vecRes ? res : opType(0)));
                }
                break;
            case "select"_op:
                if (!vecRes) {
                    Bad("scalar select");
                }
                break;
            case "any"_op:
            case "all"_op:
            case "movemask"_op:
                if (!IsVector(opType(0))) {
                    Bad(name + " of scalar");
                }
                break;
            case "shuffle"_op:
                if (IsVector(opType(1))) {
                    Bad("dynamic shuffle");
                }
                break;
            case "index"_op:
                if (instr.Operands[1].Type != TOperand::EType::Imm) {
                    Bad("dynamic vector index");
                }
                break;
            case "copy"_op:
                if (IsVector(opType(1))) {
                    Bad("copy of vector value");
                }
                break;
            case "call"_op: {
                int sym = instr.Operands[0].Imm.Value;
                const auto& module = Info.Module;
                if (auto it = module.SymIdToFuncIdx.find(sym); it != module.SymIdToFuncIdx.end()) {
                    if (module.Functions[it->second].IsCoroutine) {
                        Bad("call of coroutine");
                    }
                } else if (auto jt = module.SymIdToExtFuncIdx.find(sym); jt != module.SymIdToExtFuncIdx.end()) {
                    const auto& ext = module.ExternalFunctions[jt->second];
                    for (int t : ext.ArgTypes) {
                        if (!Scalar(t) && !CStruct(t)) {
                            Bad("external call with " + TypeKind(t) + " argument" + VectorNote(t));
                        }
                    }
                    int ret = ext.ReturnTypeId;
                    if (Layout(ret).Kind != TLayout::Void && !Scalar(ret) && !CStruct(ret)) {
                        Bad("external call returning " + TypeKind(ret) + VectorNote(ret));
                    }
                } else {
                    Bad("call of unknown symbol");
                }
                break;
            }
            default:
                break;
        }
    }
};

} // namespace

std::optional<std::string> FindUnsupported(TModuleInfo& info, bool cStructs) {
    TChecker checker{info, cStructs, std::nullopt};
    for (const auto& fun : info.Module.Functions) {
        checker.Function(fun);
        if (checker.Error) {
            break;
        }
    }
    return checker.Error;
}

} // namespace NTpde
} // namespace NCodeGen
} // namespace NQumir
