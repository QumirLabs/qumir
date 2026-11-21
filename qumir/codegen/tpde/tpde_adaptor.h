#pragma once

// TPDE walks the existing Qumir IR through this view; switch_func only
// numbers the values of one function, so TPDE gets dense local indices.

#include <qumir/ir/builder.h>

#include <tpde/IRAdaptor.hpp>
#include <tpde/ValLocalIdx.hpp>

#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace NQumir {
namespace NCodeGen {
namespace NTpde {

using u8 = uint8_t;
using i32 = int32_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

enum class EValueRef : u32 {};
enum class EInstRef : u32 {};
enum class EBlockRef : u32 {};
enum class EFuncRef : u32 {};

// Machine-level shape of a Qumir type.
struct TTypeShape {
    enum EClass : u8 { Void, Int, Float, Ptr, Other };
    EClass Class = Other;
    // 1..128 for Int, 32/64 for Float, 64 for Ptr.
    u8 Bits = 0;
    // As IsSignedIntegerType of the LLVM backend: all but U*.
    bool Signed = true;
};

// One register part of a value. Structs are flattened into their scalar
// fields and vectors into their lanes; i128 takes two parts.
struct TPartDesc {
    u32 Offset;
    u8 Size;
    bool Fp;
    // Scalar type of the field or lane (i128 for both halves).
    int Type;
};

struct TLayout {
    enum EKind : u8 { Scalar, Aggregate, Vector, Void, Unsupported };
    EKind Kind = Unsupported;
    int Elem = -1; // vector lane type
    std::vector<TPartDesc> Parts;
    u32 GpParts = 0;
    u32 FpParts = 0;
};

// Module-wide data shared by the adaptor and the compiler.
struct TModuleInfo {
    explicit TModuleInfo(NIR::TModule& module);

    const TTypeShape& Shape(int typeId) const;
    const TLayout& Layout(int typeId) const;

    NIR::TModule& Module;
    std::vector<TTypeShape> Shapes;
    std::vector<TLayout> Layouts;
    TTypeShape I64Shape{TTypeShape::Int, 64, true};
    int I64 = -1;
    int Str = -1;
    // By external function index.
    std::vector<std::string> ExternalLinkNames;
};

// Value ids of a function are TPDE local indices: temporaries, then locals
// (static stack slots), arguments, addresses of globals and string literals.
// Immediates have no local index: ImmBit | index into Imms.
constexpr u32 ImmBit = 0x8000'0000u;

// Larger copies and zero fills call memcpy and memset.
constexpr u32 InlineMemoryBytes = 256;

// Whether operand `pos` of `instr` is an immediate value (not a symbol or a size).
bool IsValueImm(const NIR::TInstr& instr, int pos);

struct TImmRec {
    i64 Value;
    int TypeId;
    int VarRef = -1; // string literal address
};

struct TVarRef {
    bool IsString;
    int Index; // string literal id or slot index
};

struct TInstRec {
    // Null for the synthetic function entry.
    const NIR::TInstr* Instr = nullptr;
    // Value operands in Operands.
    u32 OpBegin = 0;
    u32 OpCount = 0;
    // Operand position -> value or INVALID_VALUE_REF.
    std::array<EValueRef, 4> Pos;
    EValueRef Result;
};

struct TPhiRec {
    EValueRef Dest;
    u32 IncBegin;
    u32 IncCount;
};

class TAdaptor {
public:
    using IRValueRef = EValueRef;
    using IRInstRef = EInstRef;
    using IRBlockRef = EBlockRef;
    using IRFuncRef = EFuncRef;

    static constexpr EValueRef INVALID_VALUE_REF = EValueRef(~0u);
    static constexpr EBlockRef INVALID_BLOCK_REF = EBlockRef(~0u);
    static constexpr EFuncRef INVALID_FUNC_REF = EFuncRef(~0u);
    static constexpr bool TPDE_PROVIDES_HIGHEST_VAL_IDX = true;
    static constexpr bool TPDE_LIVENESS_VISIT_ARGS = true;

    TAdaptor(TModuleInfo& info, bool link);

    // Module.
    u32 func_count() const {
        return NFunc + NExt;
    }
    auto funcs() const {
        return std::views::iota(0u, NFunc + NExt)
            | std::views::transform([](u32 f) {
                return EFuncRef(f);
            });
    }
    auto funcs_to_compile() const {
        return funcs()
            | std::views::filter([this](EFuncRef f) {
                return !func_extern(f);
            });
    }
    std::string_view func_link_name(EFuncRef f) const;
    bool func_extern(EFuncRef f) const {
        return u32(f) >= NFunc || Info.Module.Functions[u32(f)].Blocks.empty();
    }
    // Several linked objects each have their own hooks, as in the LLVM backend.
    bool func_only_local(EFuncRef f) const {
        int idx = int(u32(f));
        const auto& m = Info.Module;
        return Link && (idx == m.ModuleConstructorFunctionId || idx == m.ModuleDestructorFunctionId);
    }
    bool func_has_weak_linkage(EFuncRef) const {
        return false;
    }

    // Current function.
    bool cur_needs_unwind_info() const {
        return true;
    }
    bool cur_is_vararg() const {
        return false;
    }
    u32 cur_highest_val_idx() const {
        return HighestValIdx_;
    }
    std::span<const EValueRef> cur_args() const {
        return Args_;
    }
    bool cur_arg_is_byval(u32) const {
        return false;
    }
    u32 cur_arg_byval_size(u32) const {
        return 0;
    }
    u32 cur_arg_byval_align(u32) const {
        return 0;
    }
    bool cur_arg_is_sret(u32) const {
        return false;
    }
    std::span<const EValueRef> cur_static_allocas() const {
        return StaticAllocas_;
    }
    bool cur_has_dynamic_alloca() const {
        return false;
    }
    EBlockRef cur_entry_block() const {
        return EBlockRef(0);
    }
    auto cur_blocks() const {
        return std::views::iota(0u, NBlocks_)
            | std::views::transform([](u32 b) {
                return EBlockRef(b);
            });
    }

    // Blocks.
    std::span<const EBlockRef> block_succs(EBlockRef b) const {
        return std::span(Succs_).subspan(SuccBegin_[u32(b)], SuccBegin_[u32(b) + 1] - SuccBegin_[u32(b)]);
    }
    auto block_insts(EBlockRef b) const {
        return std::views::iota(InstBegin_[u32(b)], InstBegin_[u32(b) + 1])
            | std::views::transform([](u32 i) {
                return EInstRef(i);
            });
    }
    auto block_phis(EBlockRef b) const {
        return std::views::iota(PhiBegin_[u32(b)], PhiBegin_[u32(b) + 1])
            | std::views::transform([this](u32 p) {
                return Phis_[p].Dest;
            });
    }
    u32 block_info(EBlockRef b) const {
        return BlockInfo_[u32(b)];
    }
    void block_set_info(EBlockRef b, u32 v) {
        BlockInfo_[u32(b)] = v;
    }
    u32 block_info2(EBlockRef b) const {
        return BlockInfo2_[u32(b)];
    }
    void block_set_info2(EBlockRef b, u32 v) {
        BlockInfo2_[u32(b)] = v;
    }
    u32 block_fmt_ref(EBlockRef b) const {
        return u32(b);
    }

    // Values.
    tpde::ValLocalIdx val_local_idx(EValueRef v) const {
        return tpde::ValLocalIdx(u32(v));
    }
    bool val_ignore_in_liveness_analysis(EValueRef v) const;
    bool val_is_phi(EValueRef v) const {
        return u32(v) < NTmp_ && TmpPhi_[u32(v)] >= 0;
    }

    struct TPhiRef {
        const TAdaptor* Adaptor;
        const TPhiRec* Phi;

        u32 incoming_count() const {
            return Phi->IncCount;
        }
        EValueRef incoming_val_for_slot(u32 slot) const {
            return Adaptor->PhiVals_[Phi->IncBegin + slot];
        }
        EBlockRef incoming_block_for_slot(u32 slot) const {
            return Adaptor->PhiBlocks_[Phi->IncBegin + slot];
        }
        EValueRef incoming_val_for_block(EBlockRef b) const;
    };
    TPhiRef val_as_phi(EValueRef v) const {
        return TPhiRef{this, &Phis_[TmpPhi_[u32(v)]]};
    }
    u32 val_alloca_size(EValueRef v) const;
    u32 val_alloca_align(EValueRef v) const;
    u32 value_fmt_ref(EValueRef v) const {
        return u32(v);
    }

    // Instructions.
    std::span<const EValueRef> inst_operands(EInstRef i) const {
        const auto& rec = Insts_[u32(i)];
        return std::span(Operands_).subspan(rec.OpBegin, rec.OpCount);
    }
    std::span<const EValueRef> inst_results(EInstRef i) const {
        const auto& rec = Insts_[u32(i)];
        return std::span(&rec.Result, rec.Result == INVALID_VALUE_REF ? 0 : 1);
    }
    bool inst_fused(EInstRef i) const {
        return Fused_[u32(i)];
    }
    void inst_set_fused(EInstRef i) {
        Fused_[u32(i)] = 1;
    }
    std::string inst_fmt_ref(EInstRef i) const;

    void start_compile() const {}
    void end_compile() const {}
    bool switch_func(EFuncRef f);
    void reset() {}

    // Qumir-specific accessors for the compiler.
    const TInstRec& Inst(EInstRef i) const {
        return Insts_[u32(i)];
    }
    int ValueType(EValueRef v) const;
    bool IsImm(EValueRef v) const {
        return u32(v) & ImmBit;
    }
    const TImmRec& Imm(EValueRef v) const {
        return Imms_[u32(v) & ~ImmBit];
    }
    EValueRef LocalValue(int local) const {
        return EValueRef(LocalBase_ + local);
    }
    int ArgCount() const {
        return (int)Args_.size();
    }
    int SlotVarRef(int slot) const {
        return SlotRefs_[slot];
    }
    u32 VarRefLocalIdx(int varRef) const {
        return VarRefBase_ + varRef;
    }
    const TVarRef& VarRef(int varRef) const {
        return VarRefs_[varRef];
    }
    EBlockRef BlockOfLabel(NIR::TLabel label) const {
        return EBlockRef(Fun->GetBlockIdx(label));
    }
    // Locals referenced by load/stre/lea and arguments; promoted locals need no slot.
    bool LocalUsed(int local) const {
        return UsedLocals_[local];
    }
    bool IsArgLocal(int local) const {
        return ArgLocal_[local];
    }
    // Leaf functions get a cheaper frame and may keep values in caller-saved registers.
    bool MayCall() const {
        return HasCalls_;
    }

    TModuleInfo& Info;
    const NIR::TFunction* Fun = nullptr;
    u32 NFunc;
    u32 NExt;
    // EObjectUse::Link: the module hooks run from .init_array and .fini_array.
    bool Link;

private:
    EValueRef AddImm(const NIR::TImm& imm, int contextType);
    int VarRefFor(bool isString, int index);

    u32 NTmp_ = 0;
    u32 LocalBase_ = 0;
    u32 ArgBase_ = 0;
    u32 VarRefBase_ = 0;
    u32 NBlocks_ = 0;
    u32 HighestValIdx_ = 0;
    bool HasCalls_ = false;
    std::vector<EValueRef> Args_;
    std::vector<EValueRef> StaticAllocas_;
    std::vector<EValueRef> Operands_;
    std::vector<EValueRef> PhiVals_;
    std::vector<EBlockRef> PhiBlocks_;
    std::vector<EBlockRef> Succs_;
    std::vector<u32> InstBegin_;
    std::vector<u32> PhiBegin_;
    std::vector<u32> SuccBegin_;
    std::vector<u32> BlockInfo_;
    std::vector<u32> BlockInfo2_;
    std::vector<TInstRec> Insts_;
    std::vector<TPhiRec> Phis_;
    std::vector<int> TmpPhi_;
    std::vector<u8> IsAlloca_;
    std::vector<u8> Fused_;
    std::vector<u8> UsedLocals_;
    std::vector<u8> ArgLocal_;
    std::vector<u32> AllocaSize_;
    std::vector<TImmRec> Imms_;
    std::vector<TVarRef> VarRefs_;
    // Module-wide, reset per function.
    std::vector<int> SlotRefs_;
    std::vector<int> StrRefs_;
};

static_assert(tpde::IRAdaptor<TAdaptor>);

} // namespace NTpde
} // namespace NCodeGen
} // namespace NQumir
