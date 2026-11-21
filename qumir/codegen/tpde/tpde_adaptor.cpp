#include "tpde_adaptor.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace NQumir {
namespace NCodeGen {
namespace NTpde {

using namespace NIR;
using namespace NIR::NLiterals;

namespace {

TTypeShape ShapeOfKind(EKind kind) {
    switch (kind) {
        case EKind::I1:
            return {TTypeShape::Int, 1, true};
        case EKind::I8:
            return {TTypeShape::Int, 8, true};
        case EKind::I16:
            return {TTypeShape::Int, 16, true};
        case EKind::I32:
            return {TTypeShape::Int, 32, true};
        case EKind::I64:
            return {TTypeShape::Int, 64, true};
        case EKind::I128:
            return {TTypeShape::Int, 128, true};
        case EKind::U8:
            return {TTypeShape::Int, 8, false};
        case EKind::U16:
            return {TTypeShape::Int, 16, false};
        case EKind::U32:
            return {TTypeShape::Int, 32, false};
        case EKind::U64:
            return {TTypeShape::Int, 64, false};
        case EKind::U128:
            return {TTypeShape::Int, 128, false};
        case EKind::F32:
            return {TTypeShape::Float, 32, true};
        case EKind::F64:
            return {TTypeShape::Float, 64, true};
        case EKind::Void:
            return {TTypeShape::Void, 0, true};
        case EKind::Ptr:
            return {TTypeShape::Ptr, 64, false};
        case EKind::Func:
            // The LLVM backend lowers function ids to i64.
            return {TTypeShape::Int, 64, true};
        default:
            return {TTypeShape::Other, 0, true};
    }
}

// Values with more parts (big structs and vectors) are not supported.
constexpr u32 MaxParts = 64;

} // namespace

bool IsValueImm(const TInstr& instr, int pos) {
    if (instr.Operands[pos].Type != TOperand::EType::Imm) {
        return false;
    }
    return !(instr.Op == "call"_op && pos == 0) && instr.Op != "salloc"_op;
}

TModuleInfo::TModuleInfo(TModule& module)
    : Module(module)
{
    auto& tt = module.Types;
    I64 = tt.I(EKind::I64);
    Str = tt.Ptr(tt.I(EKind::I8));
    // After Ptr() above, which may add a type; TTypeTable has no size accessor.
    for (int id = 0;; ++id) {
        EKind kind;
        try {
            kind = tt.GetKind(id);
        } catch (const std::runtime_error&) {
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
            if (p.Fp) {
                ++l.FpParts;
            } else {
                ++l.GpParts;
            }
        }
    }
    for (const auto& ext : module.ExternalFunctions) {
        ExternalLinkNames.push_back(ext.MangledName.empty() ? ext.Name : ext.MangledName);
    }
}

const TTypeShape& TModuleInfo::Shape(int typeId) const {
    // Untyped values are i64, as in the LLVM backend.
    return typeId < 0 || typeId >= (int)Shapes.size()
        ? I64Shape
        : Shapes[typeId];
}

const TLayout& TModuleInfo::Layout(int typeId) const {
    return typeId < 0 || typeId >= (int)Layouts.size()
        ? Layouts[I64]
        : Layouts[typeId];
}

TAdaptor::TAdaptor(TModuleInfo& info)
    : Info(info)
    , NFunc(info.Module.Functions.size())
    , NExt(info.Module.ExternalFunctions.size())
{
    SlotRefs_.assign(info.Module.GlobalTypes.size(), -1);
    StrRefs_.assign(info.Module.StringLiterals.size(), -1);
}

std::string_view TAdaptor::func_link_name(EFuncRef f) const {
    if (u32(f) < NFunc) {
        return Info.Module.Functions[u32(f)].Name;
    }
    return Info.ExternalLinkNames[u32(f) - NFunc];
}

bool TAdaptor::val_ignore_in_liveness_analysis(EValueRef v) const {
    u32 id = u32(v);
    if (id & ImmBit) {
        return true;
    }
    if (id < NTmp_) {
        return IsAlloca_[id];
    }
    return id < ArgBase_ || id >= VarRefBase_;
}

EValueRef TAdaptor::TPhiRef::incoming_val_for_block(EBlockRef b) const {
    for (u32 slot = 0; slot < Phi->IncCount; ++slot) {
        if (Adaptor->PhiBlocks_[Phi->IncBegin + slot] == b) {
            return Adaptor->PhiVals_[Phi->IncBegin + slot];
        }
    }
    return INVALID_VALUE_REF;
}

std::string TAdaptor::inst_fmt_ref(EInstRef i) const {
    const auto* instr = Insts_[u32(i)].Instr;
    return instr
        ? instr->Op.ToString()
        : std::string("entry");
}

int TAdaptor::ValueType(EValueRef v) const {
    u32 id = u32(v);
    if (id & ImmBit) {
        return Imms_[id & ~ImmBit].TypeId;
    }
    if (id < NTmp_) {
        return Fun->GetTmpType(id);
    }
    if (id < ArgBase_) {
        return Info.Str; // address of a local
    }
    if (id < VarRefBase_) {
        return Fun->LocalTypes[Fun->ArgLocals[id - ArgBase_].Idx];
    }
    return Info.Str;
}

u32 TAdaptor::val_alloca_size(EValueRef v) const {
    u32 id = u32(v);
    if (id < NTmp_) {
        return AllocaSize_[id];
    }
    return std::max(1, Info.Module.Types.SizeInBytes(Fun->LocalTypes[id - LocalBase_]));
}

u32 TAdaptor::val_alloca_align(EValueRef v) const {
    u32 id = u32(v);
    if (id < NTmp_) {
        return 16;
    }
    int align = Info.Module.Types.AlignInBytes(Fun->LocalTypes[id - LocalBase_]);
    return std::clamp(align, 1, 16);
}

int TAdaptor::VarRefFor(bool isString, int index) {
    auto& map = isString ? StrRefs_ : SlotRefs_;
    if (map[index] < 0) {
        map[index] = VarRefs_.size();
        VarRefs_.push_back({isString, index});
    }
    return map[index];
}

EValueRef TAdaptor::AddImm(const TImm& imm, int contextType) {
    int type = imm.TypeId;
    auto kind = Info.Layout(type).Kind;
    if (type < 0 || kind == TLayout::Unsupported || kind == TLayout::Void) {
        type = contextType >= 0 ? contextType : Info.I64;
    }
    TImmRec rec{imm.Value, type};
    if (type == Info.Str && imm.Value > 0) {
        rec.VarRef = VarRefFor(true, imm.Value);
    }
    Imms_.push_back(rec);
    return EValueRef(ImmBit | u32(Imms_.size() - 1));
}

bool TAdaptor::switch_func(EFuncRef f) {
    for (const auto& ref : VarRefs_) {
        (ref.IsString ? StrRefs_ : SlotRefs_)[ref.Index] = -1;
    }
    VarRefs_.clear();
    Imms_.clear();
    Insts_.clear();
    Operands_.clear();
    Phis_.clear();
    PhiVals_.clear();
    PhiBlocks_.clear();
    Succs_.clear();
    StaticAllocas_.clear();
    Args_.clear();

    Fun = &Info.Module.Functions[u32(f)];
    const auto& fun = *Fun;
    const auto& types = Info.Module.Types;
    HasCalls_ = false;
    NTmp_ = std::max<u32>(fun.NextTmpIdx, fun.TmpTypes.size());
    LocalBase_ = NTmp_;
    ArgBase_ = LocalBase_ + fun.LocalTypes.size();
    VarRefBase_ = ArgBase_ + fun.ArgLocals.size();
    NBlocks_ = fun.Blocks.size();

    TmpPhi_.assign(NTmp_, -1);
    IsAlloca_.assign(NTmp_, 0);
    AllocaSize_.assign(NTmp_, 0);
    UsedLocals_.assign(fun.LocalTypes.size(), 0);
    ArgLocal_.assign(fun.LocalTypes.size(), 0);
    for (const auto& a : fun.ArgLocals) {
        ArgLocal_[a.Idx] = 1;
    }
    InstBegin_.assign(NBlocks_ + 1, 0);
    PhiBegin_.assign(NBlocks_ + 1, 0);
    SuccBegin_.assign(NBlocks_ + 1, 0);
    BlockInfo_.assign(NBlocks_, 0);
    BlockInfo2_.assign(NBlocks_, 0);

    for (u32 i = 0; i < fun.ArgLocals.size(); ++i) {
        Args_.push_back(EValueRef(ArgBase_ + i));
    }

    for (u32 b = 0; b < NBlocks_; ++b) {
        const auto& block = fun.Blocks[b];
        InstBegin_[b] = Insts_.size();
        PhiBegin_[b] = Phis_.size();
        SuccBegin_[b] = Succs_.size();

        for (const auto& phi : block.Phis) {
            if (phi.Op != "phi"_op) {
                continue;
            }
            int phiType = fun.GetTmpType(phi.Dest.Idx);
            TPhiRec rec{EValueRef(phi.Dest.Idx), (u32)PhiVals_.size(), (u32)phi.Operands.size() / 2};
            for (size_t i = 0; i + 1 < phi.Operands.size(); i += 2) {
                const auto& op = phi.Operands[i];
                PhiVals_.push_back(op.Type == TOperand::EType::Imm
                    ? AddImm(op.Imm, phiType)
                    : EValueRef(op.Tmp.Idx));
                PhiBlocks_.push_back(BlockOfLabel(phi.Operands[i + 1].Label));
            }
            TmpPhi_[phi.Dest.Idx] = Phis_.size();
            Phis_.push_back(rec);
        }

        if (b == 0) {
            // Zeroes the locals and spills the arguments into them.
            TInstRec entry;
            entry.Pos.fill(INVALID_VALUE_REF);
            entry.Result = INVALID_VALUE_REF;
            entry.OpBegin = Operands_.size();
            for (auto a : Args_) {
                Operands_.push_back(a);
            }
            entry.OpCount = Args_.size();
            Insts_.push_back(entry);
        }

        for (const auto& instr : block.Instrs) {
            if (instr.Op == "nop"_op) {
                continue;
            }
            // memcpy/memset copy and zero large memory.
            if (
                instr.Op == "call"_op ||
                instr.Op == "copy"_op ||
                (instr.Op == "salloc"_op && instr.Operands[0].Imm.Value > InlineMemoryBytes))
            {
                HasCalls_ = true;
            }
            if (instr.Op == "stre"_op) {
                const auto& dst = instr.Operands[0];
                int type = dst.Type == TOperand::EType::Local
                    ? fun.LocalTypes[dst.Local.Idx]
                    : Info.Module.GlobalTypes[dst.Slot.Idx];
                HasCalls_ |= types.SizeInBytes(type) > (int)InlineMemoryBytes;
            }
            TInstRec rec;
            rec.Instr = &instr;
            rec.Pos.fill(INVALID_VALUE_REF);
            rec.OpBegin = Operands_.size();
            for (int i = 0; i < instr.OperandCount; ++i) {
                const auto& op = instr.Operands[i];
                if (op.Type == TOperand::EType::Tmp) {
                    rec.Pos[i] = EValueRef(op.Tmp.Idx);
                } else if (op.Type == TOperand::EType::Imm && IsValueImm(instr, i)) {
                    rec.Pos[i] = AddImm(op.Imm, -1);
                } else if (op.Type == TOperand::EType::Local) {
                    UsedLocals_[op.Local.Idx] = 1;
                    continue;
                } else if (op.Type == TOperand::EType::Slot) {
                    VarRefFor(false, op.Slot.Idx);
                    continue;
                } else {
                    continue;
                }
                Operands_.push_back(rec.Pos[i]);
            }
            rec.OpCount = Operands_.size() - rec.OpBegin;
            rec.Result = INVALID_VALUE_REF;
            if (instr.Dest.Idx >= 0) {
                int type = fun.GetTmpType(instr.Dest.Idx);
                if (instr.Op == "salloc"_op) {
                    IsAlloca_[instr.Dest.Idx] = 1;
                    AllocaSize_[instr.Dest.Idx] = std::max<i64>(1, instr.Operands[0].Imm.Value);
                    StaticAllocas_.push_back(EValueRef(instr.Dest.Idx));
                } else if (Info.Layout(type).Kind != TLayout::Void) {
                    rec.Result = EValueRef(instr.Dest.Idx);
                }
            }
            Insts_.push_back(rec);
        }

        if (!block.Instrs.empty()) {
            const auto& term = block.Instrs.back();
            if (term.Op == "jmp"_op) {
                Succs_.push_back(BlockOfLabel(term.Operands[0].Label));
            } else if (term.Op == "cmp"_op) {
                auto t = BlockOfLabel(term.Operands[1].Label);
                auto e = BlockOfLabel(term.Operands[2].Label);
                Succs_.push_back(t);
                if (e != t) {
                    Succs_.push_back(e);
                }
            }
        }
    }
    InstBegin_[NBlocks_] = Insts_.size();
    PhiBegin_[NBlocks_] = Phis_.size();
    SuccBegin_[NBlocks_] = Succs_.size();
    Fused_.assign(Insts_.size(), 0);

    for (u32 l = 0; l < fun.LocalTypes.size(); ++l) {
        if (UsedLocals_[l] || ArgLocal_[l]) {
            UsedLocals_[l] = 1;
            StaticAllocas_.push_back(EValueRef(LocalBase_ + l));
            if (types.SizeInBytes(fun.LocalTypes[l]) > (int)InlineMemoryBytes) {
                HasCalls_ = true; // zeroed with memset
            }
        }
    }
    u32 end = VarRefBase_ + VarRefs_.size();
    HighestValIdx_ = end > 0 ? end - 1 : 0;
    return true;
}

} // namespace NTpde
} // namespace NCodeGen
} // namespace NQumir
