// TPDE backend for AArch64: an IR adaptor over Qumir IR plus a TPDE compiler
// whose instruction selection comes from snippets.c via tpde_encodegen.

#include "tpde_adaptor.h"
#include "tpde_codegen.h"

#include <qumir/runtime/runtime.h>
#include <qumir/runtime/string.h>

#include <tpde/ElfMapper.hpp>
#include <tpde/arm64/CompilerA64.hpp>

#include "snippets_a64.hpp"

#include <dlfcn.h>
#include <setjmp.h>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace NQumir::NCodeGen {

using namespace NIR;
using namespace NIR::NLiterals;

namespace NTpde {

namespace {

double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool IsValueImm(const TInstr& instr, int pos) {
    if (instr.Operands[pos].Type != TOperand::EType::Imm) {
        return false;
    }
    return !(instr.Op == "call"_op && pos == 0) && instr.Op != "salloc"_op;
}

bool IsTerminator(TOp op) {
    return op == "jmp"_op || op == "cmp"_op || op == "ret"_op;
}

TTypeShape ShapeOfKind(EKind kind) {
    switch (kind) {
        case EKind::I1: return {TTypeShape::Int, 1, true};
        case EKind::I8: return {TTypeShape::Int, 8, true};
        case EKind::I16: return {TTypeShape::Int, 16, true};
        case EKind::I32: return {TTypeShape::Int, 32, true};
        case EKind::I64: return {TTypeShape::Int, 64, true};
        case EKind::I128: return {TTypeShape::Int, 128, true};
        case EKind::U8: return {TTypeShape::Int, 8, false};
        case EKind::U16: return {TTypeShape::Int, 16, false};
        case EKind::U32: return {TTypeShape::Int, 32, false};
        case EKind::U64: return {TTypeShape::Int, 64, false};
        case EKind::U128: return {TTypeShape::Int, 128, false};
        case EKind::F32: return {TTypeShape::Float, 32, true};
        case EKind::F64: return {TTypeShape::Float, 64, true};
        case EKind::Void: return {TTypeShape::Void, 0, true};
        case EKind::Ptr: return {TTypeShape::Ptr, 64, false};
        // The LLVM backend lowers function ids to i64.
        case EKind::Func: return {TTypeShape::Int, 64, true};
        default: return {TTypeShape::Other, 0, true};
    }
}

constexpr u32 MaxParts = 64;

} // namespace

TModuleInfo::TModuleInfo(TModule& module)
    : Module(module)
{
    auto& tt = module.Types;
    I1 = tt.I(EKind::I1);
    I64 = tt.I(EKind::I64);
    F64 = tt.I(EKind::F64);
    Str = tt.Ptr(tt.I(EKind::I8));
    // Ptr() above may add types, so shapes are computed after it.
    for (int id = 0;; ++id) {
        EKind kind;
        try {
            kind = tt.GetKind(id);
        } catch (...) {
            break;
        }
        Shapes.push_back(ShapeOfKind(kind));
    }
    Layouts.resize(Shapes.size());
    // Appends the parts of `type` stored at `offset`; false if unsupported.
    std::function<bool(int, u32, TLayout&)> flatten = [&](int type, u32 offset, TLayout& out) -> bool {
        const auto& s = Shape(type);
        switch (s.Class) {
            case TTypeShape::Int:
            case TTypeShape::Ptr:
                if (s.Bits > 64) {
                    out.Parts.push_back({offset, 8, false, type});
                    out.Parts.push_back({offset + 8, 8, false, type});
                } else {
                    out.Parts.push_back({offset, u8(std::max(1, s.Bits / 8)), false, type});
                }
                return true;
            case TTypeShape::Float:
                out.Parts.push_back({offset, u8(s.Bits / 8), true, type});
                return true;
            case TTypeShape::Other:
                break;
            default:
                return false;
        }
        auto kind = tt.GetKind(type);
        if (kind == EKind::Struct) {
            const auto& fields = tt.GetStructFields(type);
            for (int i = 0; i < (int)fields.size(); ++i) {
                if (!flatten(fields[i], offset + tt.FieldOffset(type, i), out)) {
                    return false;
                }
            }
            return true;
        }
        if (kind == EKind::Vec) {
            int elem = tt.UnderlyingType(type);
            const auto& es = Shape(elem);
            if (es.Class == TTypeShape::Other || es.Class == TTypeShape::Void || es.Bits > 64) {
                return false;
            }
            int stride = std::max(1, tt.SizeInBytes(elem));
            for (int i = 0; i < tt.VectorSize(type); ++i) {
                flatten(elem, offset + i * stride, out);
            }
            return true;
        }
        return false;
    };
    for (int id = 0; id < (int)Shapes.size(); ++id) {
        auto& l = Layouts[id];
        const auto& s = Shapes[id];
        if (s.Class == TTypeShape::Void) {
            l.Kind = TLayout::Void;
            continue;
        }
        l.Size = std::max(0, tt.SizeInBytes(id));
        bool ok = flatten(id, 0, l);
        if (!ok || l.Parts.empty() || l.Parts.size() > MaxParts) {
            l.Kind = TLayout::Unsupported;
            l.Parts.clear();
            continue;
        }
        if (s.Class != TTypeShape::Other) {
            l.Kind = TLayout::Scalar;
        } else if (tt.GetKind(id) == EKind::Vec) {
            l.Kind = TLayout::Vector;
            l.Elem = tt.UnderlyingType(id);
        } else {
            l.Kind = TLayout::Aggregate;
        }
        for (const auto& p : l.Parts) {
            (p.Fp ? l.FpParts : l.GpParts)++;
        }
    }
    for (const auto& ext : module.ExternalFunctions) {
        ExternalLinkNames.push_back(ext.MangledName.empty() ? ext.Name : ext.MangledName);
    }
}

TAdaptor::TAdaptor(TModuleInfo& info)
    : Info(info)
    , NFunc(info.Module.Functions.size())
    , NExt(info.Module.ExternalFunctions.size())
{
    SlotRefs.assign(info.Module.GlobalTypes.size(), -1);
    StrRefs.assign(info.Module.StringLiterals.size(), -1);
}

std::string_view TAdaptor::func_link_name(IRFuncRef f) const {
    if (u32(f) < NFunc) {
        return Info.Module.Functions[u32(f)].Name;
    }
    return Info.ExternalLinkNames[u32(f) - NFunc];
}

int TAdaptor::ValueType(IRValueRef v) const {
    u32 id = u32(v);
    if (id & ImmBit) {
        return Imms[id & ~ImmBit].TypeId;
    }
    if (id < NTmp) {
        return Fun->GetTmpType(id);
    }
    if (id < ArgBase) {
        return Info.Str; // address of a local
    }
    if (id < VarRefBase) {
        return Fun->LocalTypes[Fun->ArgLocals[id - ArgBase].Idx];
    }
    return Info.Str;
}

u32 TAdaptor::val_alloca_size(IRValueRef v) const {
    u32 id = u32(v);
    if (id < NTmp) {
        return AllocaSize[id];
    }
    return std::max(1, Info.Module.Types.SizeInBytes(Fun->LocalTypes[id - LocalBase]));
}

u32 TAdaptor::val_alloca_align(IRValueRef v) const {
    u32 id = u32(v);
    if (id < NTmp) {
        return 16;
    }
    int align = Info.Module.Types.AlignInBytes(Fun->LocalTypes[id - LocalBase]);
    return std::clamp(align, 1, 16);
}

int TAdaptor::VarRefFor(bool isString, int index) {
    auto& map = isString ? StrRefs : SlotRefs;
    if (map[index] < 0) {
        map[index] = VarRefs.size();
        VarRefs.push_back({isString, index});
    }
    return map[index];
}

IRValueRef TAdaptor::AddImm(const TImm& imm, int contextType) {
    int type = imm.TypeId;
    auto kind = Info.Layout(type).Kind;
    if (type < 0 || kind == TLayout::Unsupported || kind == TLayout::Void) {
        type = contextType >= 0 ? contextType : Info.I64;
    }
    TImmRec rec{imm.Value, type};
    if (type == Info.Str && imm.Value > 0) {
        rec.VarRef = VarRefFor(true, imm.Value);
    }
    Imms.push_back(rec);
    return IRValueRef(ImmBit | u32(Imms.size() - 1));
}

bool TAdaptor::switch_func(IRFuncRef f) {
    for (const auto& ref : VarRefs) {
        (ref.IsString ? StrRefs : SlotRefs)[ref.Index] = -1;
    }
    VarRefs.clear();
    Imms.clear();
    Insts.clear();
    Operands.clear();
    Phis.clear();
    PhiVals.clear();
    PhiBlocks.clear();
    Succs.clear();
    StaticAllocas.clear();
    Args.clear();

    Fun = &Info.Module.Functions[u32(f)];
    const auto& fun = *Fun;
    HasCalls = false;
    NTmp = std::max<u32>(fun.NextTmpIdx, fun.TmpTypes.size());
    LocalBase = NTmp;
    ArgBase = LocalBase + fun.LocalTypes.size();
    VarRefBase = ArgBase + fun.ArgLocals.size();
    NBlocks = fun.Blocks.size();

    TmpPhi.assign(NTmp, -1);
    IsAlloca.assign(NTmp, 0);
    AllocaSize.assign(NTmp, 0);
    UsedLocals.assign(fun.LocalTypes.size(), 0);
    ArgLocal.assign(fun.LocalTypes.size(), 0);
    for (const auto& a : fun.ArgLocals) {
        ArgLocal[a.Idx] = 1;
    }
    InstBegin.assign(NBlocks + 1, 0);
    PhiBegin.assign(NBlocks + 1, 0);
    SuccBegin.assign(NBlocks + 1, 0);
    BlockInfo.assign(NBlocks, 0);
    BlockInfo2.assign(NBlocks, 0);

    for (u32 i = 0; i < fun.ArgLocals.size(); ++i) {
        Args.push_back(IRValueRef(ArgBase + i));
    }

    for (u32 b = 0; b < NBlocks; ++b) {
        const auto& block = fun.Blocks[b];
        InstBegin[b] = Insts.size();
        PhiBegin[b] = Phis.size();
        SuccBegin[b] = Succs.size();

        for (const auto& phi : block.Phis) {
            if (phi.Op != "phi"_op) {
                continue;
            }
            int phiType = fun.GetTmpType(phi.Dest.Idx);
            TPhiRec rec{IRValueRef(phi.Dest.Idx), (u32)PhiVals.size(), (u32)phi.Operands.size() / 2};
            for (size_t i = 0; i + 1 < phi.Operands.size(); i += 2) {
                const auto& op = phi.Operands[i];
                PhiVals.push_back(op.Type == TOperand::EType::Imm
                    ? AddImm(op.Imm, phiType)
                    : IRValueRef(op.Tmp.Idx));
                PhiBlocks.push_back(BlockOfLabel(phi.Operands[i + 1].Label));
            }
            TmpPhi[phi.Dest.Idx] = Phis.size();
            Phis.push_back(rec);
        }

        if (b == 0) {
            TInstRec entry;
            entry.Pos.fill(INVALID_VALUE_REF);
            entry.Result = INVALID_VALUE_REF;
            entry.OpBegin = Operands.size();
            for (auto a : Args) {
                Operands.push_back(a);
            }
            entry.OpCount = Args.size();
            Insts.push_back(entry);
        }

        for (const auto& instr : block.Instrs) {
            if (instr.Op == "nop"_op) {
                continue;
            }
            // Calls and the memcpy/memset helpers: copy, zeroing of large stack
            // slots, copies of large address-backed structs.
            if (instr.Op == "call"_op || instr.Op == "copy"_op
                || (instr.Op == "salloc"_op && instr.Operands[0].Imm.Value > 256))
            {
                HasCalls = true;
            }
            if (instr.Op == "stre"_op) {
                const auto& dst = instr.Operands[0];
                int type = dst.Type == TOperand::EType::Local ? fun.LocalTypes[dst.Local.Idx] : Info.Module.GlobalTypes[dst.Slot.Idx];
                HasCalls |= Info.Module.Types.SizeInBytes(type) > 256;
            }
            TInstRec rec;
            rec.Instr = &instr;
            rec.Pos.fill(INVALID_VALUE_REF);
            rec.OpBegin = Operands.size();
            for (int i = 0; i < instr.OperandCount; ++i) {
                const auto& op = instr.Operands[i];
                if (op.Type == TOperand::EType::Tmp) {
                    rec.Pos[i] = IRValueRef(op.Tmp.Idx);
                } else if (op.Type == TOperand::EType::Imm && IsValueImm(instr, i)) {
                    rec.Pos[i] = AddImm(op.Imm, -1);
                } else if (op.Type == TOperand::EType::Local) {
                    UsedLocals[op.Local.Idx] = 1;
                    continue;
                } else if (op.Type == TOperand::EType::Slot) {
                    VarRefFor(false, op.Slot.Idx);
                    continue;
                } else {
                    continue;
                }
                Operands.push_back(rec.Pos[i]);
            }
            rec.OpCount = Operands.size() - rec.OpBegin;
            rec.Result = INVALID_VALUE_REF;
            if (instr.Dest.Idx >= 0) {
                int type = fun.GetTmpType(instr.Dest.Idx);
                if (instr.Op == "salloc"_op) {
                    IsAlloca[instr.Dest.Idx] = 1;
                    AllocaSize[instr.Dest.Idx] = std::max<i64>(1, instr.Operands[0].Imm.Value);
                    StaticAllocas.push_back(IRValueRef(instr.Dest.Idx));
                } else if (Info.Layout(type).Kind != TLayout::Void) {
                    rec.Result = IRValueRef(instr.Dest.Idx);
                }
            }
            Insts.push_back(rec);
        }

        if (!block.Instrs.empty()) {
            const auto& term = block.Instrs.back();
            if (term.Op == "jmp"_op) {
                Succs.push_back(BlockOfLabel(term.Operands[0].Label));
            } else if (term.Op == "cmp"_op) {
                auto t = BlockOfLabel(term.Operands[1].Label);
                auto e = BlockOfLabel(term.Operands[2].Label);
                Succs.push_back(t);
                if (e != t) {
                    Succs.push_back(e);
                }
            }
        }
    }
    InstBegin[NBlocks] = Insts.size();
    PhiBegin[NBlocks] = Phis.size();
    SuccBegin[NBlocks] = Succs.size();
    Fused.assign(Insts.size(), 0);

    for (u32 l = 0; l < fun.LocalTypes.size(); ++l) {
        if (UsedLocals[l] || ArgLocal[l]) {
            UsedLocals[l] = 1;
            StaticAllocas.push_back(IRValueRef(LocalBase + l));
            if (Info.Module.Types.SizeInBytes(fun.LocalTypes[l]) > 256) {
                HasCalls = true; // zeroed with memset
            }
        }
    }
    u32 end = VarRefBase + VarRefs.size();
    HighestValIdx = end > 0 ? end - 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------
// Support check: everything not handled below makes the module fall back to
// the LLVM backend; the unsupported kinds are counted.

namespace {

struct TChecker {
    TModuleInfo& Info;
    TTpdeUnsupported& Out;

    void Bad(const std::string& what) {
        ++Out[what];
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
    bool Returnable(int type) const {
        const auto& l = Layout(type);
        return l.Kind == TLayout::Void || (Valued(type) && l.GpParts <= 8 && l.FpParts <= 8);
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
        if (!Returnable(fun.ReturnTypeId)) {
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

    int OpType(const TFunction& fun, const TOperand& op) const {
        return OperandType(fun, Info.Module, op);
    }

    void Instr(const TFunction& fun, const TInstr& instr) {
        static const std::unordered_set<uint64_t> known = {
            "+"_op, "-"_op, "*"_op, "/"_op, "%"_op, "&"_op, "|"_op, "^"_op, "<<"_op, ">>"_op,
            "&&"_op, "||"_op,
            "<"_op, "<="_op, ">"_op, ">="_op, "=="_op, "!="_op, "!"_op, "~"_op, "neg"_op,
            "lde"_op, "ste"_op, "salloc"_op, "copy"_op, "load"_op, "lea"_op, "stre"_op,
            "ret"_op, "i2b"_op, "f2b"_op, "i2f"_op, "f2i"_op, "bitcast"_op, "mov"_op,
            "arg"_op, "call"_op, "jmp"_op, "cmp"_op, "prefetch"_op,
            "select"_op, "any"_op, "all"_op, "movemask"_op, "shuffle"_op, "index"_op,
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
                if ((k == TLayout::Aggregate || k == TLayout::Vector) && op.Imm.Value != 0
                    && !(instr.Op == "shuffle"_op && i == 1))
                {
                    Bad("non-zero " + TypeKind(t) + " immediate");
                }
                if (Shape(t).Class == TTypeShape::Ptr && op.Imm.Value != 0 && t != Info.Str) {
                    Bad("non-null pointer immediate");
                }
            } else if (op.Type == TOperand::EType::Local) {
                if (!Valued(fun.LocalTypes[op.Local.Idx]) && instr.Op != "lea"_op) {
                    Bad(name + " of " + TypeKind(fun.LocalTypes[op.Local.Idx]) + " local");
                }
            } else if (op.Type == TOperand::EType::Slot) {
                if (!Valued(Info.Module.GlobalTypes[op.Slot.Idx]) && instr.Op != "lea"_op) {
                    Bad(name + " of " + TypeKind(Info.Module.GlobalTypes[op.Slot.Idx]) + " global");
                }
            }
        }
        int res = instr.Dest.Idx >= 0 ? fun.GetTmpType(instr.Dest.Idx) : -1;
        const auto& rs = Shape(res);
        auto opType = [&](int i) { return OpType(fun, instr.Operands[i]); };
        bool vecRes = instr.Dest.Idx >= 0 && IsVector(res);
        bool aggRes = instr.Dest.Idx >= 0 && IsAggregate(res);
        switch (instr.Op) {
            case "/"_op:
            case "%"_op:
                if (rs.Class == TTypeShape::Int && rs.Bits > 64) {
                    Bad("i128 " + name);
                }
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
            case "||"_op:
                if (aggRes) {
                    Bad(name + " on struct");
                }
                if (vecRes) {
                    const auto& es = Shape(Layout(res).Elem);
                    if (es.Class == TTypeShape::Float && (instr.Op != "+"_op && instr.Op != "-"_op
                        && instr.Op != "*"_op && instr.Op != "/"_op))
                    {
                        Bad("vector float " + name);
                    }
                    if ((instr.Op == "/"_op || instr.Op == "%"_op) && es.Class != TTypeShape::Float) {
                        Bad("vector integer " + name);
                    }
                    break;
                }
                if ((instr.Op == "&&"_op || instr.Op == "||"_op)) {
                    Bad("scalar " + name);
                }
                if (rs.Class == TTypeShape::Ptr && instr.Op != "+"_op && instr.Op != "-"_op) {
                    Bad("pointer " + name);
                }
                if (instr.Op != "+"_op && instr.Op != "-"_op && instr.Op != "*"_op
                    && instr.Op != "/"_op && rs.Class != TTypeShape::Int)
                {
                    Bad(name + " on non-integer");
                }
                break;
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
            case "i2f"_op:
                if (!vecRes && Shape(opType(0)).Bits > 64) {
                    Bad("i128 i2f");
                }
                break;
            case "f2i"_op:
                if (!vecRes && rs.Bits > 64) {
                    Bad("i128 f2i");
                }
                break;
            case "mov"_op:
            case "bitcast"_op:
                if ((aggRes || vecRes || IsAggregate(opType(0)) || IsVector(opType(0)))
                    && !(vecRes && IsVector(opType(0)) && instr.Op == "mov"_op)
                    && opType(0) != res)
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
                if (Info.Module.SymIdToFuncIdx.count(sym)) {
                    const auto& callee = Info.Module.Functions[Info.Module.SymIdToFuncIdx.at(sym)];
                    if (callee.IsCoroutine) {
                        Bad("call of coroutine");
                    }
                } else if (auto it = Info.Module.SymIdToExtFuncIdx.find(sym); it != Info.Module.SymIdToExtFuncIdx.end()) {
                    const auto& ext = Info.Module.ExternalFunctions[it->second];
                    for (int t : ext.ArgTypes) {
                        if (!Scalar(t)) {
                            Bad("external call with " + TypeKind(t) + " argument");
                        }
                    }
                    if (Layout(ext.ReturnTypeId).Kind != TLayout::Void && !Scalar(ext.ReturnTypeId)) {
                        Bad("external call returning " + TypeKind(ext.ReturnTypeId));
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

// ---------------------------------------------------------------------------
// The compiler.

struct TConfig : tpde::a64::PlatformConfig {
    static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

struct TCompilerA64
    : tpde::a64::CompilerA64<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>
    , tpde_encodegen::EncodeCompiler<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>
{
    using Base = tpde::a64::CompilerA64<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>;
    using EncodeCompiler = tpde_encodegen::EncodeCompiler<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>;
    using ValuePart = typename Base::ValuePart;
    using ValuePartRef = typename Base::ValuePartRef;
    using ValueRef = typename Base::ValueRef;
    using ScratchReg = typename Base::ScratchReg;
    using GenericValuePart = typename Base::GenericValuePart;
    using Expr = typename GenericValuePart::Expr;
    using InstRange = typename Base::InstRange;
    using AsmReg = typename Base::AsmReg;
    using Jump = typename Base::Jump;
    using CallBuilder = typename Base::CallBuilder;
    using RetBuilder = typename Base::RetBuilder;
    using SymRef = tpde::SymRef;

    struct ValRefSpecial {
        uint8_t mode = 4;
        u32 Imm;
    };

    struct TParts {
        const TLayout* L;
        u32 count() const {
            return L->Parts.size();
        }
        u32 size_bytes(u32 i) const {
            return L->Parts[i].Size;
        }
        tpde::RegBank reg_bank(u32 i) const {
            return L->Parts[i].Fp ? TConfig::FP_BANK : TConfig::GP_BANK;
        }
    };

    explicit TCompilerA64(TAdaptor* adaptor)
        : Base(adaptor)
        , A(adaptor)
        , Info(adaptor->Info)
        , M(adaptor->Info.Module)
    {
        static_assert(tpde::Compiler<TCompilerA64, TConfig>);
    }

    void reset() {
        Base::reset();
        EncodeCompiler::reset();
    }

    TAdaptor* A;
    TModuleInfo& Info;
    TModule& M;
    std::vector<IRValueRef> PendingArgs;
    std::vector<SymRef> StrSyms, SlotSyms;
    SymRef MemsetSym, MemcpySym;
    TTpdeUnsupported* Failures = nullptr; // op kinds codegen gave up on

    // --- framework hooks ---------------------------------------------------

    bool cur_func_may_emit_calls() const {
        return A->MayCall();
    }
    SymRef cur_personality_func() const {
        return {};
    }
    bool try_force_fixed_assignment(IRValueRef) const {
        return false;
    }
    void define_func_idx(IRFuncRef, u32) {}
    void setup_var_ref_assignments() {}

    const TLayout& LayoutOf(int type) const {
        return Info.Layout(type);
    }

    TParts val_parts(IRValueRef v) const {
        return {&LayoutOf(A->ValueType(v))};
    }

    std::optional<ValRefSpecial> val_ref_special(IRValueRef v) const {
        if (A->IsImm(v)) {
            return ValRefSpecial{.mode = 4, .Imm = u32(v)};
        }
        return std::nullopt;
    }

    ValuePart VarRefPart(int varRef) {
        auto local = tpde::ValLocalIdx(A->VarRefLocalIdx(varRef));
        auto* assignment = this->val_assignment(local);
        if (!assignment) {
            this->init_variable_ref(local, varRef);
            assignment = this->val_assignment(local);
        }
        return ValuePart{local, assignment, 0, /*owned=*/false};
    }

    ValuePart val_part_ref_special(ValRefSpecial& vrs, u32 part) {
        const auto& imm = A->Imm(IRValueRef(vrs.Imm));
        if (imm.VarRef >= 0) {
            return VarRefPart(imm.VarRef);
        }
        const auto& layout = LayoutOf(imm.TypeId);
        const auto& desc = layout.Parts[part];
        auto bank = desc.Fp ? TConfig::FP_BANK : TConfig::GP_BANK;
        if (layout.Kind != TLayout::Scalar) {
            return ValuePart(u64(0), desc.Size, bank); // zero struct / vector
        }
        const auto& s = Info.Shape(imm.TypeId);
        u64 data = u64(imm.Value);
        if (s.Class == TTypeShape::Int && s.Bits == 1) {
            data = imm.Value != 0;
        }
        if (part == 1) {
            data = s.Signed && imm.Value < 0 ? ~u64(0) : 0;
        }
        if (desc.Size < 8) {
            data &= (u64(1) << (desc.Size * 8)) - 1;
        }
        return ValuePart(data, desc.Size, bank);
    }

    void load_address_of_var_reference(AsmReg dst, tpde::AssignmentPartRef ap) {
        const auto& ref = A->VarRef(ap.variable_ref_data());
        SymRef sym = ref.IsString ? StrSyms[ref.Index] : SlotSyms[ref.Index];
        this->text_writer.ensure_space(8);
        this->reloc_text(sym, tpde::elf::R_AARCH64_ADR_PREL_PG_HI21, this->text_writer.offset());
        ASMNC(ADRP, dst, 0, 0);
        this->reloc_text(sym, tpde::elf::R_AARCH64_ADD_ABS_LO12_NC, this->text_writer.offset());
        ASMNC(ADDxi, dst, dst, 0);
    }

    void prologue_assign_arg(tpde::CCAssigner* cc, u32 idx, IRValueRef arg) {
        bool wide = Info.Shape(A->ValueType(arg)).Bits > 64;
        Base::prologue_assign_arg(cc, idx, arg, wide ? 16 : 1, !wide);
    }

    bool hook_post_func_sym_init() {
        auto rodata = this->assembler.get_default_section(tpde::SectionKind::ReadOnly);
        auto bss = this->assembler.get_default_section(tpde::SectionKind::BSS);
        StrSyms.resize(M.StringLiterals.size());
        for (size_t i = 1; i < M.StringLiterals.size(); ++i) {
            const auto& s = M.StringLiterals[i];
            std::span<const u8> data(reinterpret_cast<const u8*>(s.c_str()), s.size() + 1);
            StrSyms[i] = this->assembler.sym_def_data(rodata, "", data, 1, tpde::Assembler::SymBinding::LOCAL);
        }
        SlotSyms.resize(M.GlobalTypes.size());
        for (size_t i = 0; i < M.GlobalTypes.size(); ++i) {
            u32 size = std::max(1, M.Types.SizeInBytes(M.GlobalTypes[i]));
            u32 align = std::clamp(M.Types.AlignInBytes(M.GlobalTypes[i]), 1, 16);
            SlotSyms[i] = this->assembler.sym_predef_data("", tpde::Assembler::SymBinding::LOCAL);
            this->assembler.sym_def_predef_zero(bss, SlotSyms[i], size, align);
        }
        MemsetSym = this->assembler.sym_add_undef("memset", tpde::Assembler::SymBinding::GLOBAL);
        MemcpySym = this->assembler.sym_add_undef("memcpy", tpde::Assembler::SymBinding::GLOBAL);
        return true;
    }

    // --- helpers -------------------------------------------------------------

    const TTypeShape& Shape(int type) const {
        return Info.Shape(type);
    }

    int TypeOf(IRValueRef v) const {
        return A->ValueType(v);
    }

    bool IsVec(int type) const {
        return LayoutOf(type).Kind == TLayout::Vector;
    }

    bool IsAgg(int type) const {
        return LayoutOf(type).Kind == TLayout::Aggregate;
    }

    // Lane type of a vector, the type itself for scalars.
    int LaneType(int type) const {
        return IsVec(type) ? LayoutOf(type).Elem : type;
    }

    ValuePartRef Const(u64 value, u32 size, tpde::RegBank bank) {
        return ValuePartRef(this, value, size, bank);
    }

    static u64 SignExtend(u64 v, u32 bits, bool sign) {
        if (bits >= 64) {
            return v;
        }
        u64 mask = (u64(1) << bits) - 1;
        v &= mask;
        if (sign && (v >> (bits - 1)) & 1) {
            v |= ~mask;
        }
        return v;
    }

    // Converts a single-part value of type `from` into type `to` like the
    // LLVM backend's implicit casts. Returns false when not implemented.
    bool Cast(ValuePartRef& p, int from, int to, bool srcSigned) {
        const auto& f = Shape(from);
        const auto& t = Shape(to);
        if (f.Class == t.Class && f.Bits == t.Bits) {
            return true;
        }
        bool fInt = f.Class == TTypeShape::Int || f.Class == TTypeShape::Ptr;
        bool tInt = t.Class == TTypeShape::Int || t.Class == TTypeShape::Ptr;
        if (fInt && tInt) {
            if (t.Bits > 64) {
                return false; // multi-part, handled by the callers that support i128
            }
            if (f.Bits > 64 || t.Bits <= f.Bits) {
                return true; // truncation: upper register bits are don't-care
            }
            bool sign = f.Class == TTypeShape::Ptr ? false : srcSigned;
            if (p.is_const()) {
                p = Const(SignExtend(p.const_data()[0], f.Bits, sign), std::max(1, t.Bits / 8), TConfig::GP_BANK);
                return true;
            }
            p = std::move(p).into_extended(sign, f.Bits, t.Bits <= 32 ? 32 : 64);
            return true;
        }
        if (fInt && t.Class == TTypeShape::Float) {
            if (f.Bits > 64) {
                return false;
            }
            if (f.Bits < 32) {
                p = std::move(p).into_extended(srcSigned, f.Bits, 32);
            }
            ValuePartRef res(this, TConfig::FP_BANK);
            bool wide = f.Bits > 32;
            bool ok;
            if (t.Bits == 64) {
                ok = srcSigned
                    ? (wide ? this->encode_i64tof64(std::move(p), res) : this->encode_i32tof64(std::move(p), res))
                    : (wide ? this->encode_u64tof64(std::move(p), res) : this->encode_u32tof64(std::move(p), res));
            } else {
                ok = srcSigned
                    ? (wide ? this->encode_i64tof32(std::move(p), res) : this->encode_i32tof32(std::move(p), res))
                    : (wide ? this->encode_u64tof32(std::move(p), res) : this->encode_u32tof32(std::move(p), res));
            }
            p = std::move(res);
            return ok;
        }
        if (f.Class == TTypeShape::Float && t.Class == TTypeShape::Float) {
            ValuePartRef res(this, TConfig::FP_BANK);
            bool ok = t.Bits == 64 ? this->encode_f32tof64(std::move(p), res) : this->encode_f64tof32(std::move(p), res);
            p = std::move(res);
            return ok;
        }
        return false;
    }

    // part(0) of v converted to `to`.
    bool Operand(ValueRef& vr, IRValueRef v, int to, bool srcSigned, ValuePartRef& out) {
        out = vr.part(0);
        return Cast(out, TypeOf(v), to, srcSigned);
    }

    // Lane `lane` of operand v converted to lane type `to`; scalars broadcast.
    bool Lane(ValueRef& vr, IRValueRef v, u32 lane, int to, bool srcSigned, ValuePartRef& out) {
        int type = TypeOf(v);
        // A broadcast scalar is read once per lane: never let a lane salvage it.
        out = IsVec(type) ? vr.part(lane) : vr.part_unowned(0);
        return Cast(out, LaneType(type), to, srcSigned);
    }

    // --- memory ------------------------------------------------------------

    bool LoadScalar(GenericValuePart&& addr, int type, ValuePart& out) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            return s.Bits == 64 ? this->encode_loadf64(std::move(addr), out) : this->encode_loadf32(std::move(addr), out);
        }
        switch (s.Class == TTypeShape::Ptr ? 64 : s.Bits) {
            case 1:
            case 8: return this->encode_loadi8_zext(std::move(addr), out);
            case 16: return this->encode_loadi16_zext(std::move(addr), out);
            case 32: return this->encode_loadi32_zext(std::move(addr), out);
            case 64:
            case 128: return this->encode_loadi64(std::move(addr), out);
            default: return false;
        }
    }

    bool StoreScalar(GenericValuePart&& addr, int type, ValuePartRef&& v) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            return s.Bits == 64 ? this->encode_storef64(std::move(addr), std::move(v)) : this->encode_storef32(std::move(addr), std::move(v));
        }
        switch (s.Class == TTypeShape::Ptr ? 64 : s.Bits) {
            case 1: return this->encode_storei8(std::move(addr), MaskBool(std::move(v)));
            case 8: return this->encode_storei8(std::move(addr), std::move(v));
            case 16: return this->encode_storei16(std::move(addr), std::move(v));
            case 32: return this->encode_storei32(std::move(addr), std::move(v));
            case 64:
            case 128: return this->encode_storei64(std::move(addr), std::move(v));
            default: return false;
        }
    }

    // Loads all parts of a value of `type` from [base].
    bool LoadValue(ValuePartRef&& base, int type, ValueRef& res) {
        const auto& layout = LayoutOf(type);
        if (layout.Kind == TLayout::Scalar && layout.Parts.size() == 1) {
            auto out = res.part(0);
            return LoadScalar(std::move(base), type, out);
        }
        AsmReg b = base.cur_reg_or_load();
        for (u32 i = 0; i < layout.Parts.size(); ++i) {
            const auto& d = layout.Parts[i];
            auto out = res.part(i);
            if (!LoadScalar(Expr(b, d.Offset), d.Type, out)) {
                return false;
            }
        }
        return true;
    }

    // Stores all parts of v (converted to `type` for scalars) to [base].
    bool StoreValue(ValuePartRef&& base, IRValueRef v, int type, bool srcSigned) {
        const auto& layout = LayoutOf(type);
        ValueRef vr = this->val_ref(v);
        if (layout.Kind == TLayout::Scalar && layout.Parts.size() == 1) {
            ValuePartRef p(this);
            if (!Operand(vr, v, type, srcSigned, p)) {
                return false;
            }
            return StoreScalar(std::move(base), type, std::move(p));
        }
        if (layout.Kind != TLayout::Scalar && TypeOf(v) != type) {
            return false;
        }
        if (layout.Kind == TLayout::Scalar && Shape(TypeOf(v)).Bits != 128) {
            return false; // i128 from a narrower value
        }
        AsmReg b = base.cur_reg_or_load();
        for (u32 i = 0; i < layout.Parts.size(); ++i) {
            const auto& d = layout.Parts[i];
            if (!StoreScalar(Expr(b, d.Offset), d.Type, vr.part(i))) {
                return false;
            }
        }
        return true;
    }

    // Copies `size` bytes from [src] to [dst].
    void CopyMemory(ValuePartRef&& dst, ValuePartRef&& src, u32 size) {
        if (size > 256) {
            tpde::a64::CCAssignerAAPCS assigner;
            CallBuilder cb(*this, assigner);
            cb.add_arg(std::move(dst), tpde::CCAssignment{});
            cb.add_arg(std::move(src), tpde::CCAssignment{});
            cb.add_arg(ValuePart(u64(size), 8, TConfig::GP_BANK), tpde::CCAssignment{});
            cb.call(MemcpySym);
            return;
        }
        AsmReg d = dst.cur_reg_or_load();
        AsmReg s = src.cur_reg_or_load();
        ScratchReg tmp(this);
        AsmReg t = tmp.alloc_gp();
        u32 pos = 0;
        for (u32 chunk : {8u, 4u, 2u, 1u}) {
            while (size - pos >= chunk) {
                this->load_off(t, s, pos, chunk);
                this->store_off(d, pos, t, chunk);
                pos += chunk;
            }
        }
    }

    // i1 registers only define bit 0; memory and the C ABI want 0/1.
    ValuePartRef MaskBool(ValuePartRef&& p) {
        if (p.is_const()) {
            return Const(p.const_data()[0] & 1, 1, TConfig::GP_BANK);
        }
        ValuePartRef res(this, TConfig::GP_BANK);
        this->encode_landi32(std::move(p), Const(1, 4, TConfig::GP_BANK), res);
        return res;
    }

    // Address of a local or global operand.
    ValuePartRef Address(const TOperand& op) {
        if (op.Type == TOperand::EType::Local) {
            auto local = tpde::ValLocalIdx(u32(A->LocalValue(op.Local.Idx)));
            return ValuePartRef(this, ValuePart{local, this->val_assignment(local), 0, /*owned=*/false});
        }
        return ValuePartRef(this, VarRefPart(A->SlotVarRef(op.Slot.Idx)));
    }

    int StoredType(const TOperand& op) const {
        return op.Type == TOperand::EType::Local ? A->Fun->LocalTypes[op.Local.Idx] : M.GlobalTypes[op.Slot.Idx];
    }

    // A register part with a known size: ValuePart(bank) temporaries report
    // size 0, which CallBuilder/RetBuilder would copy with a 32-bit move.
    ValuePartRef SizedTemp(u32 size, tpde::RegBank bank, AsmReg& reg) {
        ValuePartRef p(this, u64(0), size, bank);
        reg = p.alloc_reg();
        return p;
    }

    ValuePartRef WithSize(ValuePartRef&& p, u32 size) {
        if (p.has_assignment() || p.is_const()) {
            return std::move(p);
        }
        AsmReg src = p.cur_reg_or_load();
        AsmReg dst;
        auto q = SizedTemp(size, p.bank(), dst);
        this->mov(dst, src, size);
        p.reset();
        return q;
    }

    // Calls fn with each part of the value of `type` read from v: v itself
    // or, for address-backed structs, the struct v points to. Parts are
    // produced one at a time, so fn may pin them to argument registers.
    template <typename TFn>
    bool ForEachPart(IRValueRef v, int type, ValueRef& vr, TFn&& fn) {
        const auto& layout = LayoutOf(type);
        int from = TypeOf(v);
        if (layout.Kind == TLayout::Aggregate && Shape(from).Class == TTypeShape::Ptr) {
            // x17 is reserved by TPDE, so argument registers never clobber it.
            AsmReg base{AsmReg::R17};
            {
                auto ptr = vr.part(0);
                ASM(MOVx, base, ptr.cur_reg_or_load());
            }
            for (const auto& d : layout.Parts) {
                AsmReg reg;
                auto p = SizedTemp(d.Size, d.Fp ? TConfig::FP_BANK : TConfig::GP_BANK, reg);
                this->load_off(reg, base, d.Offset, d.Size);
                fn(std::move(p));
            }
            return true;
        }
        if (layout.Kind != TLayout::Scalar || layout.Parts.size() > 1) {
            if (from != type) {
                return false;
            }
            for (u32 i = 0; i < layout.Parts.size(); ++i) {
                fn(vr.part(i));
            }
            return true;
        }
        ValuePartRef p = vr.part(0);
        if (!Cast(p, from, type, from < 0 || Shape(from).Signed)) {
            return false;
        }
        fn(WithSize(std::move(p), layout.Parts[0].Size));
        return true;
    }

    // --- instruction selection ---------------------------------------------

    bool compile_inst(IRInstRef ref, InstRange remaining) {
        const auto& rec = A->Inst(ref);
        if (!rec.Instr) {
            return CompileEntry();
        }
        bool ok = CompileInstr(rec, remaining);
        if (!ok && Failures) {
            ++(*Failures)["codegen " + rec.Instr->Op.ToString()];
        }
        return ok;
    }

    bool CompileInstr(const TInstRec& rec, InstRange remaining) {
        const auto& instr = *rec.Instr;
        switch (instr.Op) {
            case "+"_op:
            case "-"_op:
            case "*"_op:
            case "/"_op:
            case "%"_op:
            case "&"_op:
            case "|"_op:
            case "^"_op:
            case "<<"_op:
            case ">>"_op:
            case "&&"_op:
            case "||"_op:
                return CompileBinary(rec);
            case "<"_op:
            case "<="_op:
            case ">"_op:
            case ">="_op:
            case "=="_op:
            case "!="_op:
                return CompileCompare(rec, remaining);
            case "!"_op:
            case "i2b"_op:
            case "f2b"_op:
                return CompileTruth(rec, instr.Op == "!"_op);
            case "~"_op:
            case "neg"_op:
                return CompileUnary(rec);
            case "mov"_op:
            case "i2f"_op:
            case "bitcast"_op:
                return CompileConvert(rec);
            case "f2i"_op:
                return CompileF2I(rec);
            case "lde"_op: {
                ValueRef ptr = this->val_ref(rec.Pos[0]);
                ValueRef res = this->result_ref(rec.Result);
                return LoadValue(ptr.part(0), TypeOf(rec.Result), res);
            }
            case "ste"_op:
                return CompileSte(rec);
            case "load"_op:
                return CompileLoad(rec);
            case "stre"_op:
                return CompileStre(rec);
            case "lea"_op: {
                auto addr = Address(instr.Operands[0]);
                ValueRef res = this->result_ref(rec.Result);
                res.part(0).set_value(std::move(addr));
                return true;
            }
            case "salloc"_op: {
                auto v = IRValueRef(instr.Dest.Idx);
                ZeroFrame(FrameOff(v), A->val_alloca_size(v));
                return true;
            }
            case "copy"_op:
                return CompileCopy(rec);
            case "arg"_op:
                PendingArgs.push_back(rec.Pos[0]);
                return true;
            case "call"_op:
                return CompileCall(rec);
            case "ret"_op:
                return CompileRet(rec);
            case "jmp"_op:
                this->generate_uncond_branch(A->BlockOfLabel(instr.Operands[0].Label));
                return true;
            case "cmp"_op:
                return CompileCondBr(rec);
            case "prefetch"_op: {
                ValueRef vr = this->val_ref(rec.Pos[0]);
                return true;
            }
            case "select"_op:
                return CompileSelect(rec);
            case "any"_op:
            case "all"_op:
            case "movemask"_op:
                return CompileReduce(rec);
            case "shuffle"_op:
            case "index"_op:
                return CompileShuffle(rec);
            default:
                return false;
        }
    }

    bool CompileLoad(const TInstRec& rec) {
        const auto& op = rec.Instr->Operands[0];
        int stored = StoredType(op);
        int type = TypeOf(rec.Result);
        ValueRef res = this->result_ref(rec.Result);
        auto addr = Address(op);
        if (stored == type || (LayoutOf(stored).Kind == TLayout::Scalar && LayoutOf(type).Kind == TLayout::Scalar
            && Shape(stored).Class == Shape(type).Class && Shape(stored).Bits == Shape(type).Bits))
        {
            return LoadValue(std::move(addr), stored, res);
        }
        // A local read as another scalar type: load, then cast like the LLVM backend.
        if (LayoutOf(stored).Parts.size() != 1 || LayoutOf(type).Parts.size() != 1) {
            return false;
        }
        ValuePartRef tmp(this, LayoutOf(stored).Parts[0].Fp ? TConfig::FP_BANK : TConfig::GP_BANK);
        if (!LoadScalar(std::move(addr), stored, tmp)) {
            return false;
        }
        if (!Cast(tmp, stored, type, Shape(stored).Signed)) {
            return false;
        }
        res.part(0).set_value(std::move(tmp));
        return true;
    }

    bool CompileStre(const TInstRec& rec) {
        const auto& op = rec.Instr->Operands[0];
        int stored = StoredType(op);
        IRValueRef v = rec.Pos[1];
        if (IsAgg(stored) && Shape(TypeOf(v)).Class == TTypeShape::Ptr) {
            // Address-backed struct: copy the bytes (memcpy in the LLVM backend).
            ValueRef vr = this->val_ref(v);
            CopyMemory(Address(op), vr.part(0), std::max(1, M.Types.SizeInBytes(stored)));
            return true;
        }
        return StoreValue(Address(op), v, stored, true);
    }

    bool CompileSte(const TInstRec& rec) {
        int ptrType = TypeOf(rec.Pos[0]);
        int valType = TypeOf(rec.Pos[1]);
        int storeType = M.Types.IsPointer(ptrType) ? M.Types.UnderlyingType(ptrType) : valType;
        if (LayoutOf(storeType).Kind != TLayout::Scalar) {
            storeType = valType;
        }
        ValueRef ptr = this->val_ref(rec.Pos[0]);
        return StoreValue(ptr.part(0), rec.Pos[1], storeType, valType < 0 || Shape(valType).Signed);
    }

    bool CompileCopy(const TInstRec& rec) {
        IRValueRef src = rec.Pos[1];
        int srcType = TypeOf(src);
        if (IsAgg(srcType)) {
            ValueRef dst = this->val_ref(rec.Pos[0]);
            ValueRef size = this->val_ref(rec.Pos[2]);
            return StoreValue(dst.part(0), src, srcType, true);
        }
        // memcpy(dst, src, size); size immediates are untyped -> i64
        std::array<typename Base::CallArg, 3> args{
            typename Base::CallArg(rec.Pos[0]),
            typename Base::CallArg(rec.Pos[1]),
            typename Base::CallArg(rec.Pos[2]),
        };
        this->generate_call(MemcpySym, args, nullptr);
        return true;
    }

    // Zero-fills [FP + off, FP + off + size).
    void ZeroFrame(u32 off, u32 size) {
        if (size > 256) {
            ValuePartRef addr(this, TConfig::GP_BANK);
            AsmReg r = addr.alloc_reg();
            if (!ASMIF(ADDxi, r, AsmReg{AsmReg::FP}, off)) {
                this->materialize_constant(u64(off), TConfig::GP_BANK, 8, r);
                ASM(ADDx, r, r, AsmReg{AsmReg::FP});
            }
            tpde::a64::CCAssignerAAPCS assigner;
            CallBuilder cb(*this, assigner);
            cb.add_arg(std::move(addr), tpde::CCAssignment{});
            cb.add_arg(ValuePart(u64(0), 4, TConfig::GP_BANK), tpde::CCAssignment{});
            cb.add_arg(ValuePart(u64(size), 8, TConfig::GP_BANK), tpde::CCAssignment{});
            cb.call(MemsetSym);
            return;
        }
        ScratchReg zero(this);
        AsmReg z = zero.alloc_gp();
        this->materialize_constant(u64(0), TConfig::GP_BANK, 4, z);
        u32 pos = 0;
        for (u32 chunk : {8u, 4u, 2u, 1u}) {
            while (size - pos >= chunk && (off + pos) % chunk == 0) {
                this->store_off(AsmReg{AsmReg::FP}, off + pos, z, chunk);
                pos += chunk;
            }
        }
        while (pos < size) {
            this->store_off(AsmReg{AsmReg::FP}, off + pos, z, 1);
            ++pos;
        }
    }

    u32 FrameOff(IRValueRef alloca) {
        return this->val_assignment(tpde::ValLocalIdx(u32(alloca)))->frame_off;
    }

    // Function entry: zero the locals (as the LLVM backend does) and spill the
    // arguments into their locals.
    bool CompileEntry() {
        const auto& fun = *A->Fun;
        for (u32 l = 0; l < fun.LocalTypes.size(); ++l) {
            if (A->LocalUsed(l) && !A->IsArgLocal(l)) {
                auto v = A->LocalValue(l);
                ZeroFrame(FrameOff(v), A->val_alloca_size(v));
            }
        }
        for (int i = 0; i < A->ArgCount(); ++i) {
            int local = fun.ArgLocals[i].Idx;
            u32 off = FrameOff(A->LocalValue(local));
            ValueRef arg = this->val_ref(A->cur_args()[i]);
            const auto& layout = LayoutOf(fun.LocalTypes[local]);
            for (u32 p = 0; p < layout.Parts.size(); ++p) {
                auto part = arg.part(p);
                AsmReg reg = part.cur_reg_or_load();
                this->store_off(AsmReg{AsmReg::FP}, off + layout.Parts[p].Offset, reg, layout.Parts[p].Size);
            }
        }
        return true;
    }

    // Scalar binary op on parts already converted to `type` (<= 64 bit).
    bool ScalarBinary(TOp op, int type, ValuePartRef&& l, ValuePartRef&& r, ValuePart& out) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            bool d = s.Bits == 64;
            switch (op) {
                case "+"_op: return d ? this->encode_addf64(std::move(l), std::move(r), out) : this->encode_addf32(std::move(l), std::move(r), out);
                case "-"_op: return d ? this->encode_subf64(std::move(l), std::move(r), out) : this->encode_subf32(std::move(l), std::move(r), out);
                case "*"_op: return d ? this->encode_mulf64(std::move(l), std::move(r), out) : this->encode_mulf32(std::move(l), std::move(r), out);
                case "/"_op: return d ? this->encode_divf64(std::move(l), std::move(r), out) : this->encode_divf32(std::move(l), std::move(r), out);
                default: return false;
            }
        }
        bool w64 = s.Class == TTypeShape::Ptr || s.Bits == 64;
        if (!w64 && s.Bits < 32) {
            switch (op) {
                case "/"_op:
                case "%"_op:
                    l = std::move(l).into_extended(s.Signed, s.Bits, 32);
                    r = std::move(r).into_extended(s.Signed, s.Bits, 32);
                    break;
                case ">>"_op:
                    l = std::move(l).into_extended(s.Signed, s.Bits, 32);
                    [[fallthrough]];
                case "<<"_op: {
                    ValuePartRef masked(this, TConfig::GP_BANK);
                    if (!this->encode_landi32(std::move(r), Const(std::max<int>(1, s.Bits) - 1, 4, TConfig::GP_BANK), masked)) {
                        return false;
                    }
                    r = std::move(masked);
                    break;
                }
                default:
                    break;
            }
        }
        // x * 2^k as a shift: index arithmetic would otherwise load the
        // constant and queue on the single integer multiply pipeline.
        if (op == "*"_op && l.is_const() && !r.is_const()) {
            std::swap(l, r);
        }
        if (op == "*"_op && r.is_const()) {
            u64 c = r.const_data()[0] & (w64 ? ~u64(0) : 0xffff'ffffu);
            if (c != 0 && (c & (c - 1)) == 0) {
                auto amount = Const(std::countr_zero(c), 8, TConfig::GP_BANK);
                return w64 ? this->encode_shli64(std::move(l), std::move(amount), out) : this->encode_shli32(std::move(l), std::move(amount), out);
            }
        }
        switch (op) {
            case "+"_op: return w64 ? this->encode_addi64(std::move(l), std::move(r), out) : this->encode_addi32(std::move(l), std::move(r), out);
            case "-"_op: return w64 ? this->encode_subi64(std::move(l), std::move(r), out) : this->encode_subi32(std::move(l), std::move(r), out);
            case "*"_op: return w64 ? this->encode_muli64(std::move(l), std::move(r), out) : this->encode_muli32(std::move(l), std::move(r), out);
            case "&"_op:
            case "&&"_op: return w64 ? this->encode_landi64(std::move(l), std::move(r), out) : this->encode_landi32(std::move(l), std::move(r), out);
            case "|"_op:
            case "||"_op: return w64 ? this->encode_lori64(std::move(l), std::move(r), out) : this->encode_lori32(std::move(l), std::move(r), out);
            case "^"_op: return w64 ? this->encode_lxori64(std::move(l), std::move(r), out) : this->encode_lxori32(std::move(l), std::move(r), out);
            case "<<"_op: return w64 ? this->encode_shli64(std::move(l), std::move(r), out) : this->encode_shli32(std::move(l), std::move(r), out);
            case ">>"_op:
                if (s.Signed && s.Class == TTypeShape::Int) {
                    return w64 ? this->encode_ashri64(std::move(l), std::move(r), out) : this->encode_ashri32(std::move(l), std::move(r), out);
                }
                return w64 ? this->encode_shri64(std::move(l), std::move(r), out) : this->encode_shri32(std::move(l), std::move(r), out);
            case "/"_op:
                if (s.Signed) {
                    return w64 ? this->encode_sdivi64(std::move(l), std::move(r), out) : this->encode_sdivi32(std::move(l), std::move(r), out);
                }
                return w64 ? this->encode_udivi64(std::move(l), std::move(r), out) : this->encode_udivi32(std::move(l), std::move(r), out);
            case "%"_op:
                if (s.Signed) {
                    return w64 ? this->encode_sremi64(std::move(l), std::move(r), out) : this->encode_sremi32(std::move(l), std::move(r), out);
                }
                return w64 ? this->encode_uremi64(std::move(l), std::move(r), out) : this->encode_uremi32(std::move(l), std::move(r), out);
            default:
                return false;
        }
    }

    bool CompileBinary(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        int type = TypeOf(rec.Result);
        const auto& s = Shape(type);
        IRValueRef a = rec.Pos[0], b = rec.Pos[1];
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        ValueRef res = this->result_ref(rec.Result);

        if (IsVec(type)) {
            int lane = LayoutOf(type).Elem;
            bool sign = Shape(lane).Signed;
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this), r(this);
                if (!Lane(ar, a, i, lane, sign, l) || !Lane(br, b, i, lane, sign, r)) {
                    return false;
                }
                auto out = res.part(i);
                if (!ScalarBinary(op, lane, std::move(l), std::move(r), out)) {
                    return false;
                }
            }
            return true;
        }

        if (s.Class == TTypeShape::Ptr) {
            // ptr +/- int: byte offset, narrower indices are sign-extended (GEP)
            ValuePartRef l(this), r(this);
            if (!Operand(ar, a, type, true, l) || !Operand(br, b, Info.I64, true, r)) {
                return false;
            }
            auto out = res.part(0);
            return ScalarBinary(op, Info.I64, std::move(l), std::move(r), out);
        }

        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            if (Shape(TypeOf(a)).Bits != 128) {
                return false;
            }
            if (op == "<<"_op || op == ">>"_op) {
                ValuePartRef amount(this);
                if (!Operand(br, b, Info.I64, true, amount)) {
                    return false;
                }
                if (op == "<<"_op) {
                    return this->encode_shli128(ar.part(0), ar.part(1), std::move(amount), res.part(0), res.part(1));
                }
                return s.Signed
                    ? this->encode_ashri128(ar.part(0), ar.part(1), std::move(amount), res.part(0), res.part(1))
                    : this->encode_shri128(ar.part(0), ar.part(1), std::move(amount), res.part(0), res.part(1));
            }
            if (Shape(TypeOf(b)).Bits != 128) {
                return false;
            }
            switch (op) {
                case "+"_op: return this->encode_addi128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                case "-"_op: return this->encode_subi128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                case "*"_op: return this->encode_muli128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                case "&"_op: return this->encode_landi128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                case "|"_op: return this->encode_lori128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                case "^"_op: return this->encode_lxori128(ar.part(0), ar.part(1), br.part(0), br.part(1), res.part(0), res.part(1));
                default: return false;
            }
        }

        ValuePartRef l(this), r(this);
        if (!Operand(ar, a, type, true, l) || !Operand(br, b, type, true, r)) {
            return false;
        }
        auto out = res.part(0);
        return ScalarBinary(op, type, std::move(l), std::move(r), out);
    }

    static typename Jump::Kind CondFor(TOp op, bool isFloat, bool isSigned) {
        switch (op) {
            case "=="_op: return Jump::Jeq;
            case "!="_op: return Jump::Jne;
            case "<"_op: return isFloat ? Jump::Jmi : isSigned ? Jump::Jlt : Jump::Jlo;
            case "<="_op: return isFloat ? Jump::Jls : isSigned ? Jump::Jle : Jump::Jls;
            case ">"_op: return isFloat ? Jump::Jgt : isSigned ? Jump::Jgt : Jump::Jhi;
            case ">="_op: return isFloat ? Jump::Jge : isSigned ? Jump::Jge : Jump::Jhs;
            default: return Jump::Jeq;
        }
    }

    // Flag-setting compare of parts l and r of scalar `type` (<= 64 bit);
    // returns the condition that holds when the comparison is true.
    Jump ScalarCompare(TOp op, int type, ValuePartRef&& l, ValuePartRef&& r) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            AsmReg lr = l.cur_reg_or_load();
            AsmReg rr = r.cur_reg_or_load();
            if (s.Bits == 64) {
                ASM(FCMP_d, lr, rr);
            } else {
                ASM(FCMP_s, lr, rr);
            }
            return Jump(CondFor(op, true, false));
        }
        bool sign = s.Class == TTypeShape::Int && s.Signed;
        Jump j(CondFor(op, false, sign));
        if (l.is_const() && !r.is_const()) {
            std::swap(l, r);
            j = this->swap_jump(j);
        }
        u32 bits = s.Class == TTypeShape::Ptr ? 64 : s.Bits;
        if (bits != 32 && bits != 64) {
            l = std::move(l).into_extended(sign, bits, 32);
            if (r.is_const()) {
                r = Const(SignExtend(r.const_data()[0], bits, sign) & 0xffff'ffffu, 4, TConfig::GP_BANK);
            } else {
                r = std::move(r).into_extended(sign, bits, 32);
            }
            bits = 32;
        }
        AsmReg lr = l.cur_reg_or_load();
        if (r.is_const()) {
            u64 imm = r.const_data()[0];
            if (bits == 32) {
                imm &= 0xffff'ffffu;
            }
            bool done = bits == 32 ? ASMIF(CMPwi, lr, imm) : ASMIF(CMPxi, lr, imm);
            if (!done) {
                AsmReg tmp = this->permanent_scratch_reg;
                this->materialize_constant(imm, TConfig::GP_BANK, bits / 8, tmp);
                if (bits == 32) {
                    ASM(CMPw, lr, tmp);
                } else {
                    ASM(CMPx, lr, tmp);
                }
            }
        } else {
            AsmReg rr = r.cur_reg_or_load();
            if (bits == 32) {
                ASM(CMPw, lr, rr);
            } else {
                ASM(CMPx, lr, rr);
            }
        }
        return j;
    }

    // Compare of scalar values a and b (b converted to a's type).
    std::optional<Jump> EmitCompare(TOp op, IRValueRef a, IRValueRef b) {
        int type = TypeOf(a);
        const auto& s = Shape(type);
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            if (Shape(TypeOf(b)).Bits != 128) {
                return std::nullopt;
            }
            auto lloP = ar.part(0);
            AsmReg llo = lloP.cur_reg_or_load();
            auto lhiP = ar.part(1);
            AsmReg lhi = lhiP.cur_reg_or_load();
            auto rloP = br.part(0);
            AsmReg rlo = rloP.cur_reg_or_load();
            auto rhiP = br.part(1);
            AsmReg rhi = rhiP.cur_reg_or_load();
            Jump j(CondFor(op, false, s.Signed));
            if (j.kind == Jump::Jeq || j.kind == Jump::Jne) {
                ASM(CMPx, llo, rlo);
                ASM(CCMPx, lhi, rhi, 0, DA_EQ);
            } else if (j.kind == Jump::Jhi || j.kind == Jump::Jls || j.kind == Jump::Jle || j.kind == Jump::Jgt) {
                j = this->swap_jump(j);
                ASM(CMPx, rlo, llo);
                ASM(SBCSx, DA_ZR, rhi, lhi);
            } else {
                ASM(CMPx, llo, rlo);
                ASM(SBCSx, DA_ZR, lhi, rhi);
            }
            return j;
        }
        if (s.Class != TTypeShape::Int && s.Class != TTypeShape::Ptr && s.Class != TTypeShape::Float) {
            return std::nullopt;
        }
        ValuePartRef l = ar.part(0), r(this);
        if (!Operand(br, b, type, true, r)) {
            return std::nullopt;
        }
        return ScalarCompare(op, type, std::move(l), std::move(r));
    }

    bool CompileCompare(const TInstRec& rec, InstRange remaining) {
        const auto op = rec.Instr->Op;
        int type = TypeOf(rec.Result);
        if (IsVec(type)) {
            IRValueRef a = rec.Pos[0], b = rec.Pos[1];
            int lane = LaneType(IsVec(TypeOf(a)) ? TypeOf(a) : TypeOf(b));
            bool sign = Shape(lane).Signed;
            ValueRef ar = this->val_ref(a);
            ValueRef br = this->val_ref(b);
            ValueRef res = this->result_ref(rec.Result);
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this), r(this);
                if (!Lane(ar, a, i, lane, sign, l) || !Lane(br, b, i, lane, sign, r)) {
                    return false;
                }
                Jump j = ScalarCompare(op, lane, std::move(l), std::move(r));
                auto out = res.part(i);
                this->generate_raw_set(j, out.alloc_reg());
                out.set_modified();
            }
            return true;
        }
        // Fuse with the next instruction if it is the only user: cmp tmp L1 L2.
        const TInstRec* br = nullptr;
        if (remaining.from != remaining.to) {
            IRInstRef next = *remaining.from;
            const auto& n = A->Inst(next);
            if (n.Instr && n.Instr->Op == "cmp"_op && n.Pos[0] == rec.Result
                && this->analyzer.liveness_info(tpde::ValLocalIdx(u32(rec.Result))).ref_count <= 2)
            {
                br = &n;
                A->inst_set_fused(next);
            }
        }
        auto j = EmitCompare(op, rec.Pos[0], rec.Pos[1]);
        if (!j) {
            return false;
        }
        if (br) {
            auto t = A->BlockOfLabel(br->Instr->Operands[1].Label);
            auto f = A->BlockOfLabel(br->Instr->Operands[2].Label);
            if (t == f) {
                this->generate_uncond_branch(t);
            } else {
                this->generate_cond_branch(*j, t, f);
            }
            return true;
        }
        ValueRef res = this->result_ref(rec.Result);
        auto p = res.part(0);
        this->generate_raw_set(*j, p.alloc_reg());
        p.set_modified();
        return true;
    }

    // Sets flags so that Jne means "part p of `type` is true" (non-zero;
    // float: unordered or != 0).
    void ScalarTest(int type, ValuePartRef&& p) {
        const auto& s = Shape(type);
        AsmReg r = p.cur_reg_or_load();
        if (s.Class == TTypeShape::Float) {
            if (s.Bits == 64) {
                ASM(FCMP_0d, r);
            } else {
                ASM(FCMP_0s, r);
            }
            return;
        }
        switch (s.Class == TTypeShape::Ptr ? 64 : s.Bits) {
            case 1: ASM(TSTwi, r, 1); break;
            case 8: ASM(TSTwi, r, 0xff); break;
            case 16: ASM(TSTwi, r, 0xffff); break;
            case 32: ASM(CMPwi, r, 0); break;
            default: ASM(CMPxi, r, 0); break;
        }
    }

    bool EmitTest(IRValueRef v) {
        int type = TypeOf(v);
        const auto& s = Shape(type);
        ValueRef vr = this->val_ref(v);
        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            auto lo = vr.part(0);
            AsmReg l = lo.cur_reg_or_load();
            auto hi = vr.part(1);
            AsmReg h = hi.cur_reg_or_load();
            ASM(ORRx, this->permanent_scratch_reg, l, h);
            ASM(CMPxi, this->permanent_scratch_reg, 0);
            return true;
        }
        if (LayoutOf(type).Kind != TLayout::Scalar) {
            return false;
        }
        ScalarTest(type, vr.part(0));
        return true;
    }

    bool CompileCondBr(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        auto t = A->BlockOfLabel(instr.Operands[1].Label);
        auto f = A->BlockOfLabel(instr.Operands[2].Label);
        if (t == f) {
            {
                ValueRef vr = this->val_ref(rec.Pos[0]);
            }
            this->generate_uncond_branch(t);
            return true;
        }
        if (A->IsImm(rec.Pos[0])) {
            this->generate_uncond_branch(A->Imm(rec.Pos[0]).Value != 0 ? t : f);
            return true;
        }
        if (!EmitTest(rec.Pos[0])) {
            return false;
        }
        this->generate_cond_branch(Jump::Jne, t, f);
        return true;
    }

    bool CompileTruth(const TInstRec& rec, bool negate) {
        int type = TypeOf(rec.Result);
        Jump j(negate ? Jump::Jeq : Jump::Jne);
        if (IsVec(type)) {
            IRValueRef a = rec.Pos[0];
            ValueRef ar = this->val_ref(a);
            ValueRef res = this->result_ref(rec.Result);
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this);
                int lane = LaneType(TypeOf(a));
                if (!Lane(ar, a, i, lane, true, l)) {
                    return false;
                }
                ScalarTest(lane, std::move(l));
                auto out = res.part(i);
                this->generate_raw_set(j, out.alloc_reg());
                out.set_modified();
            }
            return true;
        }
        if (!EmitTest(rec.Pos[0])) {
            return false;
        }
        ValueRef res = this->result_ref(rec.Result);
        auto p = res.part(0);
        this->generate_raw_set(j, p.alloc_reg());
        p.set_modified();
        return true;
    }

    bool UnaryPart(bool neg, int type, ValuePartRef&& v, ValuePart& out) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            if (!neg) {
                return false;
            }
            return s.Bits == 64 ? this->encode_fnegf64(std::move(v), out) : this->encode_fnegf32(std::move(v), out);
        }
        if (s.Bits == 64) {
            return neg ? this->encode_negi64(std::move(v), out) : this->encode_noti64(std::move(v), out);
        }
        return neg ? this->encode_negi32(std::move(v), out) : this->encode_noti32(std::move(v), out);
    }

    bool CompileUnary(const TInstRec& rec) {
        int type = TypeOf(rec.Result);
        const auto& s = Shape(type);
        IRValueRef a = rec.Pos[0];
        ValueRef ar = this->val_ref(a);
        ValueRef res = this->result_ref(rec.Result);
        bool neg = rec.Instr->Op == "neg"_op;
        if (IsVec(type)) {
            int lane = LayoutOf(type).Elem;
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef v(this);
                if (!Lane(ar, a, i, lane, Shape(lane).Signed, v)) {
                    return false;
                }
                auto out = res.part(i);
                if (!UnaryPart(neg, lane, std::move(v), out)) {
                    return false;
                }
            }
            return true;
        }
        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            if (Shape(TypeOf(a)).Bits != 128) {
                return false;
            }
            return neg ? this->encode_negi128(ar.part(0), ar.part(1), res.part(0), res.part(1))
                       : this->encode_noti128(ar.part(0), ar.part(1), res.part(0), res.part(1));
        }
        ValuePartRef v(this);
        if (!Operand(ar, a, type, true, v)) {
            return false;
        }
        auto out = res.part(0);
        return UnaryPart(neg, type, std::move(v), out);
    }

    // Widens part(0) of an integer value to i128.
    bool WidenTo128(ValueRef& ar, const TTypeShape& f, bool sign, ValueRef& res) {
        ValuePartRef lo = ar.part(0);
        if (f.Bits < 64) {
            if (lo.is_const()) {
                lo = Const(SignExtend(lo.const_data()[0], f.Bits, sign), 8, TConfig::GP_BANK);
            } else {
                lo = std::move(lo).into_extended(sign, f.Bits, 64);
            }
        }
        if (lo.is_const()) {
            u64 v = lo.const_data()[0];
            res.part(1).set_value(Const(sign && i64(v) < 0 ? ~u64(0) : 0, 8, TConfig::GP_BANK));
            res.part(0).set_value(std::move(lo));
            return true;
        }
        if (sign) {
            ValuePartRef hi(this, TConfig::GP_BANK);
            lo.cur_reg_or_load();
            if (!this->encode_fill_with_sign64(lo.get_unowned_ref(), hi)) {
                return false;
            }
            res.part(1).set_value(std::move(hi));
        } else {
            res.part(1).set_value(Const(0, 8, TConfig::GP_BANK));
        }
        res.part(0).set_value(std::move(lo));
        return true;
    }

    // mov / i2f / bitcast
    bool CompileConvert(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        IRValueRef a = rec.Pos[0];
        int from = TypeOf(a);
        int to = TypeOf(rec.Result);
        const auto& f = Shape(from);
        const auto& t = Shape(to);
        ValueRef ar = this->val_ref(a);
        ValueRef res = this->result_ref(rec.Result);
        bool fInt = f.Class == TTypeShape::Int || f.Class == TTypeShape::Ptr;
        bool tInt = t.Class == TTypeShape::Int || t.Class == TTypeShape::Ptr;

        if (from == to && LayoutOf(to).Kind != TLayout::Scalar) {
            for (u32 i = 0; i < LayoutOf(to).Parts.size(); ++i) {
                res.part(i).set_value(ar.part(i));
            }
            return true;
        }
        if (IsVec(to) && IsVec(from)) {
            // lane-wise cast with the element signedness
            int fl = LayoutOf(from).Elem, tl = LayoutOf(to).Elem;
            if (LayoutOf(from).Parts.size() != LayoutOf(to).Parts.size()) {
                return false;
            }
            for (u32 i = 0; i < LayoutOf(to).Parts.size(); ++i) {
                ValuePartRef p = ar.part(i);
                if (!Cast(p, fl, tl, Shape(fl).Signed)) {
                    return false;
                }
                res.part(i).set_value(std::move(p));
            }
            return true;
        }
        if (LayoutOf(to).Kind != TLayout::Scalar || LayoutOf(from).Kind != TLayout::Scalar) {
            return false;
        }

        if (f.Bits == 128 || t.Bits == 128) {
            if (!(fInt && tInt)) {
                return false;
            }
            if (f.Bits == 128 && t.Bits == 128) {
                res.part(0).set_value(ar.part(0));
                res.part(1).set_value(ar.part(1));
                return true;
            }
            if (f.Bits == 128) {
                res.part(0).set_value(ar.part(0)); // truncation
                return true;
            }
            return WidenTo128(ar, f, f.Class == TTypeShape::Int && f.Signed, res);
        }

        if (op == "bitcast"_op) {
            ValuePartRef p = ar.part(0);
            if (fInt && tInt) {
                res.part(0).set_value(std::move(p));
                return true;
            }
            if (f.Bits != t.Bits) {
                return false;
            }
            if (f.Class == t.Class) {
                res.part(0).set_value(std::move(p));
                return true;
            }
            if (f.Class == TTypeShape::Float) {
                return f.Bits == 64 ? this->encode_bitcastf64toi64(std::move(p), res.part(0)) : this->encode_bitcastf32toi32(std::move(p), res.part(0));
            }
            return t.Bits == 64 ? this->encode_bitcasti64tof64(std::move(p), res.part(0)) : this->encode_bitcasti32tof32(std::move(p), res.part(0));
        }

        // mov: int casts use the source signedness; i2f: also unsigned for U*.
        bool srcSigned = from < 0 || f.Signed;
        if (op == "mov"_op && !(fInt && tInt) && !(f.Class == TTypeShape::Float && t.Class == TTypeShape::Float)) {
            return false;
        }
        if (op == "mov"_op && f.Class == TTypeShape::Int && t.Class == TTypeShape::Ptr && f.Bits < 64) {
            srcSigned = false; // inttoptr zero-extends
        }
        ValuePartRef p = ar.part(0);
        if (!Cast(p, from, to, srcSigned)) {
            return false;
        }
        res.part(0).set_value(std::move(p));
        return true;
    }

    bool F2IPart(int from, int to, ValuePartRef&& in, ValuePart& out) {
        const auto& f = Shape(from);
        const auto& t = Shape(to);
        if (f.Class != TTypeShape::Float || t.Class != TTypeShape::Int || t.Bits > 64) {
            return false;
        }
        bool d = f.Bits == 64;
        bool w = t.Bits == 64;
        if (t.Signed) {
            if (d) {
                return w ? this->encode_f64toi64(std::move(in), out) : this->encode_f64toi32(std::move(in), out);
            }
            return w ? this->encode_f32toi64(std::move(in), out) : this->encode_f32toi32(std::move(in), out);
        }
        if (d) {
            return w ? this->encode_f64tou64(std::move(in), out) : this->encode_f64tou32(std::move(in), out);
        }
        return w ? this->encode_f32tou64(std::move(in), out) : this->encode_f32tou32(std::move(in), out);
    }

    bool CompileF2I(const TInstRec& rec) {
        IRValueRef a = rec.Pos[0];
        int from = TypeOf(a);
        int to = TypeOf(rec.Result);
        ValueRef ar = this->val_ref(a);
        ValueRef res = this->result_ref(rec.Result);
        if (IsVec(to)) {
            for (u32 i = 0; i < LayoutOf(to).Parts.size(); ++i) {
                ValuePartRef in(this);
                if (!Lane(ar, a, i, LaneType(from), true, in)) {
                    return false;
                }
                auto out = res.part(i);
                if (!F2IPart(LaneType(from), LayoutOf(to).Elem, std::move(in), out)) {
                    return false;
                }
            }
            return true;
        }
        auto out = res.part(0);
        return F2IPart(from, to, ar.part(0), out);
    }

    bool CompileSelect(const TInstRec& rec) {
        int type = TypeOf(rec.Result);
        if (!IsVec(type)) {
            return false;
        }
        IRValueRef m = rec.Pos[0], a = rec.Pos[1], b = rec.Pos[2];
        int lane = LayoutOf(type).Elem;
        bool fp = Shape(lane).Class == TTypeShape::Float;
        bool w64 = Shape(lane).Bits == 64 || Shape(lane).Class == TTypeShape::Ptr;
        ValueRef mr = this->val_ref(m);
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        ValueRef res = this->result_ref(rec.Result);
        for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
            ValuePartRef t(this), f(this), c(this);
            if (!Lane(ar, a, i, lane, true, t) || !Lane(br, b, i, lane, true, f)) {
                return false;
            }
            int mlane = LaneType(TypeOf(m));
            c = IsVec(TypeOf(m)) ? mr.part(i) : mr.part_unowned(0);
            AsmReg tr = t.cur_reg_or_load();
            AsmReg fr = f.cur_reg_or_load();
            ScalarTest(mlane, std::move(c));
            auto out = res.part(i);
            AsmReg dst = out.alloc_reg();
            if (fp) {
                if (Shape(lane).Bits == 64) {
                    ASM(FCSELd, dst, tr, fr, DA_NE);
                } else {
                    ASM(FCSELs, dst, tr, fr, DA_NE);
                }
            } else {
                this->generate_raw_select(Jump(Jump::Jne), dst, tr, fr, w64);
            }
            out.set_modified();
        }
        return true;
    }

    // any / all / movemask over the truth values of the lanes.
    bool CompileReduce(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        IRValueRef v = rec.Pos[0];
        int type = TypeOf(v);
        if (!IsVec(type)) {
            return false;
        }
        int lane = LayoutOf(type).Elem;
        ValueRef vr = this->val_ref(v);
        ValueRef res = this->result_ref(rec.Result);
        ScratchReg acc(this), bit(this);
        AsmReg ar = acc.alloc_gp();
        AsmReg br = bit.alloc_gp();
        u32 n = LayoutOf(type).Parts.size();
        for (u32 i = 0; i < n; ++i) {
            ScalarTest(lane, vr.part(i));
            this->generate_raw_set(Jump(Jump::Jne), i == 0 ? ar : br);
            if (i == 0) {
                continue;
            }
            if (op == "any"_op) {
                ASM(ORRw, ar, ar, br);
            } else if (op == "all"_op) {
                ASM(ANDw, ar, ar, br);
            } else {
                ASM(ORRw_lsl, ar, ar, br, i);
            }
        }
        auto out = res.part(0);
        out.set_value(std::move(acc));
        return true;
    }

    // shuffle with a constant mask, index with a constant lane.
    bool CompileShuffle(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        IRValueRef v = rec.Pos[0];
        int type = TypeOf(v);
        if (!IsVec(type) || instr.Operands[1].Type != TOperand::EType::Imm) {
            return false;
        }
        u32 n = LayoutOf(type).Parts.size();
        ValueRef vr = this->val_ref(v);
        ValueRef res = this->result_ref(rec.Result);
        if (instr.Op == "index"_op) {
            i64 lane = instr.Operands[1].Imm.Value;
            if (lane < 0 || lane >= (i64)n) {
                return false; // out of bounds: the LLVM backend reports it at run time
            }
            res.part(0).set_value(vr.part_unowned(lane));
            return true;
        }
        const auto& mask = M.VectorShuffleMasks.at(instr.Operands[1].Imm.Value);
        if (mask.size() != LayoutOf(TypeOf(rec.Result)).Parts.size()) {
            return false;
        }
        for (u32 i = 0; i < mask.size(); ++i) {
            if (mask[i] < 0 || mask[i] >= (int)n) {
                return false;
            }
            res.part(i).set_value(vr.part_unowned(mask[i]));
        }
        return true;
    }

    bool CompileCall(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        int sym = instr.Operands[0].Imm.Value;
        std::vector<int> params;
        int retType = -1;
        u32 funcIdx;
        bool external = false;
        const TExternalFunction* ext = nullptr;
        if (auto it = M.SymIdToFuncIdx.find(sym); it != M.SymIdToFuncIdx.end()) {
            const auto& callee = M.Functions[it->second];
            for (const auto& a : callee.ArgLocals) {
                params.push_back(callee.LocalTypes[a.Idx]);
            }
            retType = callee.ReturnTypeId;
            funcIdx = it->second;
        } else if (auto jt = M.SymIdToExtFuncIdx.find(sym); jt != M.SymIdToExtFuncIdx.end()) {
            ext = &M.ExternalFunctions[jt->second];
            params = ext->ArgTypes;
            retType = ext->ReturnTypeId;
            funcIdx = A->NFunc + jt->second;
            external = true;
        } else {
            return false;
        }
        auto args = std::move(PendingArgs);
        PendingArgs.clear();
        if (args.size() != params.size()) {
            return false;
        }

        bool hasResult = rec.Result != TAdaptor::INVALID_VALUE_REF && LayoutOf(retType).Kind != TLayout::Void;
        if (ext && (ext->Name == "builtin::cttz" || ext->Name == "builtin::ctlz" || ext->Name == "builtin::ctpop")) {
            ValueRef vr = this->val_ref(args[0]);
            ValuePartRef p(this);
            if (!Operand(vr, args[0], Info.I64, true, p)) {
                return false;
            }
            if (!hasResult) {
                return true;
            }
            ValueRef res = this->result_ref(rec.Result);
            if (ext->Name == "builtin::cttz") {
                return this->encode_cttzi64(std::move(p), res.part(0));
            }
            if (ext->Name == "builtin::ctlz") {
                return this->encode_ctlzi64(std::move(p), res.part(0));
            }
            return this->encode_ctpopi64(std::move(p), res.part(0));
        }
        if (ext && ext->Name == "builtin::byte_match_backend") {
            if (hasResult) {
                // Same answer as the LLVM backend on AArch64 (NEON), so kernels
                // take the same path; byte_match8/16 below are SWAR snippets.
                ValueRef res = this->result_ref(rec.Result);
                res.part(0).set_value(Const(1, 8, TConfig::GP_BANK));
            }
            return true;
        }
        if (ext && (ext->Name == "builtin::byte_match8" || ext->Name == "builtin::byte_match16")) {
            ValueRef ptr = this->val_ref(args[0]);
            ValueRef byte = this->val_ref(args[1]);
            if (!hasResult) {
                return true;
            }
            ValueRef res = this->result_ref(rec.Result);
            auto ones = Const(0x0101010101010101ull, 8, TConfig::GP_BANK);
            auto low7 = Const(0x7f7f7f7f7f7f7f7full, 8, TConfig::GP_BANK);
            if (ext->Name == "builtin::byte_match8") {
                return this->encode_byte_match8(ptr.part(0), byte.part(0), std::move(ones), std::move(low7), res.part(0));
            }
            return this->encode_byte_match16(ptr.part(0), byte.part(0), std::move(ones), std::move(low7),
                Const(0x0102040810204080ull, 8, TConfig::GP_BANK), res.part(0));
        }

        tpde::a64::CCAssignerAAPCS assigner;
        CallBuilder cb(*this, assigner);
        for (size_t i = 0; i < args.size(); ++i) {
            int to = params[i];
            ValueRef vr = this->val_ref(args[i]);
            if (Shape(to).Bits == 128 && LayoutOf(to).Kind == TLayout::Scalar) {
                if (Shape(TypeOf(args[i])).Bits != 128) {
                    return false;
                }
                cb.add_arg(vr.part(0), tpde::CCAssignment{.consecutive = 1, .align = 16});
                cb.add_arg(vr.part(1), tpde::CCAssignment{});
                continue;
            }
            // Runtime functions read a boolean as a whole register.
            bool zextBool = external && Shape(to).Class == TTypeShape::Int && Shape(to).Bits == 1;
            bool ok = ForEachPart(args[i], to, vr, [&](ValuePartRef&& p) {
                if (zextBool) {
                    p = WithSize(MaskBool(std::move(p)), 4);
                }
                cb.add_arg(std::move(p), tpde::CCAssignment{});
            });
            if (!ok) {
                return false;
            }
        }
        cb.call(this->func_syms[funcIdx]);
        if (hasResult) {
            if (LayoutOf(retType).Parts.size() != LayoutOf(TypeOf(rec.Result)).Parts.size()) {
                return false;
            }
            ValueRef res = this->result_ref(rec.Result);
            cb.add_ret(res);
        }
        return true;
    }

    bool CompileRet(const TInstRec& rec) {
        RetBuilder rb(*this, *this->cur_cc_assigner());
        if (rec.Pos[0] != TAdaptor::INVALID_VALUE_REF) {
            IRValueRef v = rec.Pos[0];
            int to = A->Fun->ReturnTypeId;
            ValueRef vr = this->val_ref(v);
            if (Shape(to).Bits == 128 && LayoutOf(to).Kind == TLayout::Scalar) {
                if (Shape(TypeOf(v)).Bits != 128) {
                    return false;
                }
                rb.add(vr.part(0), tpde::CCAssignment{});
                rb.add(vr.part(1), tpde::CCAssignment{});
            } else {
                bool isBool = Shape(to).Class == TTypeShape::Int && Shape(to).Bits == 1;
                bool ok = ForEachPart(v, to, vr, [&](ValuePartRef&& p) {
                    rb.add(isBool ? WithSize(MaskBool(std::move(p)), 4) : std::move(p), tpde::CCAssignment{});
                });
                if (!ok) {
                    return false;
                }
            }
        }
        rb.ret();
        return true;
    }
};

class TMappedModule : public TTpdeModule {
public:
    void* Lookup(const std::string& name) const override {
        auto it = Symbols.find(name);
        return it == Symbols.end() ? nullptr : Mapper.get_sym_addr(it->second);
    }

    tpde::elf::ElfMapper Mapper;
    std::unordered_map<std::string, tpde::SymRef> Symbols;
};

bool CompileModule(TCompilerA64& compiler, TTpdeStats* stats, std::string* error) {
    TTpdeUnsupported failures;
    compiler.Failures = &failures;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = compiler.compile();
    if (stats) {
        stats->CompileMs = MsSince(t0);
        auto& text = compiler.assembler.get_section(compiler.assembler.get_default_section(tpde::SectionKind::Text));
        stats->TextBytes = text.size();
    }
    if (!ok && error) {
        *error = "TPDE compilation failed:";
        for (const auto& [what, count] : failures) {
            *error += " " + what + " x" + std::to_string(count);
        }
    }
    return ok;
}

} // namespace NTpde

bool TpdeCheckModule(TModule& module, TTpdeUnsupported* unsupported) {
    TTpdeUnsupported local;
    auto& out = unsupported ? *unsupported : local;
    out.clear();
    NTpde::TModuleInfo info(module);
    NTpde::TChecker checker{info, out};
    for (const auto& fun : module.Functions) {
        checker.Function(fun);
    }
    return out.empty();
}

std::optional<std::vector<uint8_t>> TpdeCompileObject(TModule& module, TTpdeStats* stats, std::string* error) {
    NTpde::TModuleInfo info(module);
    NTpde::TAdaptor adaptor(info);
    NTpde::TCompilerA64 compiler(&adaptor);
    if (!NTpde::CompileModule(compiler, stats, error)) {
        return std::nullopt;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto obj = compiler.assembler.build_object_file();
    if (stats) {
        stats->ObjectMs = NTpde::MsSince(t0);
        stats->ObjectBytes = obj.size();
    }
    if (const char* path = std::getenv("QUMIR_TPDE_DUMP_OBJECT")) {
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(obj.data(), 1, obj.size(), f);
            std::fclose(f);
        }
    }
    return obj;
}

std::unique_ptr<TTpdeModule> TpdeCompileAndMap(TModule& module, TTpdeStats* stats, std::string* error) {
    NTpde::TModuleInfo info(module);
    NTpde::TAdaptor adaptor(info);
    NTpde::TCompilerA64 compiler(&adaptor);
    if (!NTpde::CompileModule(compiler, stats, error)) {
        return nullptr;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto mapped = std::make_unique<NTpde::TMappedModule>();
    std::string missing;
    bool ok = mapped->Mapper.map(compiler.assembler, [&](std::string_view name) -> void* {
        std::string s(name);
        void* addr = dlsym(RTLD_DEFAULT, s.c_str());
        if (!addr && missing.empty()) {
            missing = s;
        }
        return addr;
    });
    if (!ok) {
        if (error) {
            *error = "TPDE mapping failed" + (missing.empty() ? std::string() : ": unresolved symbol " + missing);
        }
        return nullptr;
    }
    for (size_t i = 0; i < module.Functions.size(); ++i) {
        if (!module.Functions[i].Blocks.empty()) {
            mapped->Symbols[module.Functions[i].Name] = compiler.func_syms[i];
        }
    }
    if (stats) {
        stats->MapMs = NTpde::MsSince(t0);
    }
    return mapped;
}

namespace {

template <typename TResult, typename TFunction>
[[gnu::noinline]] TResult SafeCall(TFunction function) {
    jmp_buf jb;
    __set_jmp_target(&jb);
    if (setjmp(jb) != 0) {
        __clear_jmp_target();
        throw std::runtime_error(__get_runtime_error());
    }
    if constexpr (std::is_void_v<TResult>) {
        function();
        __clear_jmp_target();
    } else {
        auto result = function();
        __clear_jmp_target();
        return result;
    }
}

} // namespace

std::optional<std::string> TpdeRunEntry(
    const TTpdeModule& compiled,
    const TModule& module,
    const TFunction& entry,
    double* entryMs,
    std::string* error)
{
    auto runVoid = [&](const std::string& name) {
        if (auto* fn = compiled.Lookup(name)) {
            SafeCall<void>(reinterpret_cast<void (*)()>(fn));
        }
    };
    void* fn = compiled.Lookup(entry.Name);
    if (!fn) {
        if (error) {
            *error = "entry point not found: " + entry.Name;
        }
        return std::nullopt;
    }
    runVoid("$$module_constructor");
    std::optional<std::string> result;
    std::ostringstream oss;
    auto t0 = std::chrono::steady_clock::now();
    int ret = entry.ReturnTypeId;
    auto kind = ret < 0 ? EKind::I64 : module.Types.GetKind(ret); // untyped is i64
    switch (kind) {
        case EKind::Void:
            SafeCall<void>(reinterpret_cast<void (*)()>(fn));
            break;
        case EKind::I1:
            oss << (SafeCall<bool>(reinterpret_cast<bool (*)()>(fn)) ? "true" : "false");
            result = oss.str();
            break;
        case EKind::I8:
        case EKind::U8:
            oss << int64_t(SafeCall<int8_t>(reinterpret_cast<int8_t (*)()>(fn)));
            result = oss.str();
            break;
        case EKind::I16:
        case EKind::U16:
            oss << int64_t(SafeCall<int16_t>(reinterpret_cast<int16_t (*)()>(fn)));
            result = oss.str();
            break;
        case EKind::I32:
        case EKind::U32:
            oss << int64_t(SafeCall<int32_t>(reinterpret_cast<int32_t (*)()>(fn)));
            result = oss.str();
            break;
        case EKind::I64:
        case EKind::U64:
        case EKind::Func:
            oss << SafeCall<int64_t>(reinterpret_cast<int64_t (*)()>(fn));
            result = oss.str();
            break;
        case EKind::F32:
            oss << std::fixed << std::setprecision(15) << SafeCall<float>(reinterpret_cast<float (*)()>(fn));
            result = oss.str();
            break;
        case EKind::F64:
            oss << std::fixed << std::setprecision(15) << SafeCall<double>(reinterpret_cast<double (*)()>(fn));
            result = oss.str();
            break;
        case EKind::Ptr: {
            auto* ptr = static_cast<char*>(SafeCall<void*>(reinterpret_cast<void* (*)()>(fn)));
            if (ptr) {
                oss << ptr;
                if (entry.ReturnTypeIsString) {
                    NRuntime::str_release(ptr);
                }
            } else {
                oss << "(null)";
            }
            result = oss.str();
            break;
        }
        default:
            if (error) {
                *error = "unsupported function return type";
            }
            return std::nullopt;
    }
    if (entryMs) {
        *entryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    runVoid("$$module_destructor");
    return result;
}

} // namespace NQumir::NCodeGen
