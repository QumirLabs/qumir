#pragma once

// TPDE IR adaptor over Qumir IR (NIR::TModule). TPDE walks the existing IR
// through this view; switch_func only numbers the values of one function so
// that TPDE gets dense local indices.

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

namespace NQumir::NCodeGen::NTpde {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

enum class IRValueRef : u32 {};
enum class IRInstRef : u32 {};
enum class IRBlockRef : u32 {};
enum class IRFuncRef : u32 {};

// Machine-level shape of a Qumir type.
struct TTypeShape {
    enum EClass : u8 { Void, Int, Float, Ptr, Other } Class = Other;
    u8 Bits = 0;        // 1..128 for Int, 32/64 for Float, 64 for Ptr
    bool Signed = true; // IsSignedIntegerType of the LLVM backend: all but U*
};

// One register part of a value. Structs are flattened into their scalar
// fields and vectors into their lanes (scalarized), i128 takes two parts.
struct TPartDesc {
    u32 Offset; // byte offset of the part in memory
    u8 Size;    // bytes held by the register
    bool Fp;
    int Type;   // scalar type of the field or lane (i128 for both halves)
};

struct TLayout {
    enum EKind : u8 { Scalar, Aggregate, Vector, Void, Unsupported } Kind = Unsupported;
    u32 Size = 0;
    int Elem = -1; // vector lane type
    std::vector<TPartDesc> Parts;
    u32 GpParts = 0, FpParts = 0;
};

// Module-wide data shared by the adaptor and the compiler.
struct TModuleInfo {
    explicit TModuleInfo(NIR::TModule& module);

    const TTypeShape& Shape(int typeId) const {
        return typeId < 0 || typeId >= (int)Shapes.size() ? I64Shape : Shapes[typeId];
    }
    const TLayout& Layout(int typeId) const {
        return typeId < 0 || typeId >= (int)Layouts.size() ? Layouts[I64] : Layouts[typeId];
    }

    NIR::TModule& Module;
    std::vector<TTypeShape> Shapes;
    std::vector<TLayout> Layouts;
    TTypeShape I64Shape{TTypeShape::Int, 64, true};
    int I1 = -1, I64 = -1, F64 = -1, Str = -1;
    // Symbol names of the external functions, by external function index.
    std::vector<std::string> ExternalLinkNames;
};

// Value ids of the current function double as TPDE local indices:
//   [0, NTmp)               temporaries (TTmp)
//   [LocalBase, ArgBase)    locals, as static stack slots
//   [ArgBase, VarRefBase)   function arguments
//   [VarRefBase, ...)       addresses of globals and string literals
// Immediates have no local index: ImmBit | index into Imms.
constexpr u32 ImmBit = 0x8000'0000u;

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
    const NIR::TInstr* Instr = nullptr; // nullptr: synthetic function entry
    u32 OpBegin = 0, OpCount = 0;       // value operands in Operands
    std::array<IRValueRef, 4> Pos;      // operand position -> value or INVALID
    IRValueRef Result;
};

struct TPhiRec {
    IRValueRef Dest;
    u32 IncBegin, IncCount;
};

class TAdaptor {
public:
    using IRValueRef = NTpde::IRValueRef;
    using IRInstRef = NTpde::IRInstRef;
    using IRBlockRef = NTpde::IRBlockRef;
    using IRFuncRef = NTpde::IRFuncRef;

    static constexpr IRValueRef INVALID_VALUE_REF = IRValueRef(~0u);
    static constexpr IRBlockRef INVALID_BLOCK_REF = IRBlockRef(~0u);
    static constexpr IRFuncRef INVALID_FUNC_REF = IRFuncRef(~0u);
    static constexpr bool TPDE_PROVIDES_HIGHEST_VAL_IDX = true;
    static constexpr bool TPDE_LIVENESS_VISIT_ARGS = true;

    explicit TAdaptor(TModuleInfo& info);

    // module
    u32 func_count() const {
        return NFunc + NExt;
    }
    auto funcs() const {
        return std::views::iota(0u, NFunc + NExt)
            | std::views::transform([](u32 f) { return IRFuncRef(f); });
    }
    auto funcs_to_compile() const {
        return funcs() | std::views::filter([this](IRFuncRef f) { return !func_extern(f); });
    }
    std::string_view func_link_name(IRFuncRef f) const;
    bool func_extern(IRFuncRef f) const {
        return u32(f) >= NFunc || Info.Module.Functions[u32(f)].Blocks.empty();
    }
    bool func_only_local(IRFuncRef) const {
        return false;
    }
    bool func_has_weak_linkage(IRFuncRef) const {
        return false;
    }

    // current function
    bool cur_needs_unwind_info() const {
        return true;
    }
    bool cur_is_vararg() const {
        return false;
    }
    u32 cur_highest_val_idx() const {
        return HighestValIdx;
    }
    std::span<const IRValueRef> cur_args() const {
        return Args;
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
    std::span<const IRValueRef> cur_static_allocas() const {
        return StaticAllocas;
    }
    bool cur_has_dynamic_alloca() const {
        return false;
    }
    IRBlockRef cur_entry_block() const {
        return IRBlockRef(0);
    }
    auto cur_blocks() const {
        return std::views::iota(0u, NBlocks)
            | std::views::transform([](u32 b) { return IRBlockRef(b); });
    }

    // blocks
    std::span<const IRBlockRef> block_succs(IRBlockRef b) const {
        return std::span(Succs).subspan(SuccBegin[u32(b)], SuccBegin[u32(b) + 1] - SuccBegin[u32(b)]);
    }
    auto block_insts(IRBlockRef b) const {
        return std::views::iota(InstBegin[u32(b)], InstBegin[u32(b) + 1])
            | std::views::transform([](u32 i) { return IRInstRef(i); });
    }
    auto block_phis(IRBlockRef b) const {
        return std::views::iota(PhiBegin[u32(b)], PhiBegin[u32(b) + 1])
            | std::views::transform([this](u32 p) { return Phis[p].Dest; });
    }
    u32 block_info(IRBlockRef b) const {
        return BlockInfo[u32(b)];
    }
    void block_set_info(IRBlockRef b, u32 v) {
        BlockInfo[u32(b)] = v;
    }
    u32 block_info2(IRBlockRef b) const {
        return BlockInfo2[u32(b)];
    }
    void block_set_info2(IRBlockRef b, u32 v) {
        BlockInfo2[u32(b)] = v;
    }
    u32 block_fmt_ref(IRBlockRef b) const {
        return u32(b);
    }

    // values
    tpde::ValLocalIdx val_local_idx(IRValueRef v) const {
        return tpde::ValLocalIdx(u32(v));
    }
    bool val_ignore_in_liveness_analysis(IRValueRef v) const {
        u32 id = u32(v);
        if (id & ImmBit) {
            return true;
        }
        if (id < NTmp) {
            return IsAlloca[id];
        }
        return id < ArgBase || id >= VarRefBase;
    }
    bool val_is_phi(IRValueRef v) const {
        u32 id = u32(v);
        return id < NTmp && TmpPhi[id] >= 0;
    }

    struct PHIRef {
        const TAdaptor* A;
        const TPhiRec* P;
        u32 incoming_count() const {
            return P->IncCount;
        }
        IRValueRef incoming_val_for_slot(u32 s) const {
            return A->PhiVals[P->IncBegin + s];
        }
        IRBlockRef incoming_block_for_slot(u32 s) const {
            return A->PhiBlocks[P->IncBegin + s];
        }
        IRValueRef incoming_val_for_block(IRBlockRef b) const {
            for (u32 s = 0; s < P->IncCount; ++s) {
                if (A->PhiBlocks[P->IncBegin + s] == b) {
                    return A->PhiVals[P->IncBegin + s];
                }
            }
            return INVALID_VALUE_REF;
        }
    };
    PHIRef val_as_phi(IRValueRef v) const {
        return PHIRef{this, &Phis[TmpPhi[u32(v)]]};
    }
    u32 val_alloca_size(IRValueRef v) const;
    u32 val_alloca_align(IRValueRef v) const;
    u32 value_fmt_ref(IRValueRef v) const {
        return u32(v);
    }

    // instructions
    std::span<const IRValueRef> inst_operands(IRInstRef i) const {
        const auto& rec = Insts[u32(i)];
        return std::span(Operands).subspan(rec.OpBegin, rec.OpCount);
    }
    std::span<const IRValueRef> inst_results(IRInstRef i) const {
        const auto& rec = Insts[u32(i)];
        return std::span(&rec.Result, rec.Result == INVALID_VALUE_REF ? 0 : 1);
    }
    bool inst_fused(IRInstRef i) const {
        return Fused[u32(i)];
    }
    void inst_set_fused(IRInstRef i) {
        Fused[u32(i)] = 1;
    }
    std::string inst_fmt_ref(IRInstRef i) const {
        const auto* instr = Insts[u32(i)].Instr;
        return instr ? instr->Op.ToString() : std::string("entry");
    }

    void start_compile() const {}
    void end_compile() const {}
    bool switch_func(IRFuncRef f);
    void reset() {}

    // Qumir-specific accessors for the compiler.
    const TInstRec& Inst(IRInstRef i) const {
        return Insts[u32(i)];
    }
    int ValueType(IRValueRef v) const;
    bool IsImm(IRValueRef v) const {
        return u32(v) & ImmBit;
    }
    const TImmRec& Imm(IRValueRef v) const {
        return Imms[u32(v) & ~ImmBit];
    }
    IRValueRef LocalValue(int local) const {
        return IRValueRef(LocalBase + local);
    }
    int ArgCount() const {
        return (int)Args.size();
    }
    int SlotVarRef(int slot) const {
        return SlotRefs[slot];
    }
    u32 VarRefLocalIdx(int varRef) const {
        return VarRefBase + varRef;
    }
    const TVarRef& VarRef(int varRef) const {
        return VarRefs[varRef];
    }
    IRBlockRef BlockOfLabel(NIR::TLabel l) const {
        return IRBlockRef(Fun->GetBlockIdx(l));
    }
    // Locals referenced by load/stre/lea; others got promoted and need no slot.
    bool LocalUsed(int local) const {
        return UsedLocals[local];
    }
    bool IsArgLocal(int local) const {
        return ArgLocal[local];
    }
    // Whether code for the current function may contain calls (leaf
    // functions get a cheaper frame and caller-saved fixed registers).
    bool MayCall() const {
        return HasCalls;
    }

    TModuleInfo& Info;
    const NIR::TFunction* Fun = nullptr;
    u32 NFunc, NExt;

private:
    IRValueRef AddImm(const NIR::TImm& imm, int contextType);
    int VarRefFor(bool isString, int index);

    u32 NTmp = 0, LocalBase = 0, ArgBase = 0, VarRefBase = 0, NBlocks = 0;
    u32 HighestValIdx = 0;
    bool HasCalls = false;
    std::vector<IRValueRef> Args, StaticAllocas, Operands, PhiVals;
    std::vector<IRBlockRef> PhiBlocks, Succs;
    std::vector<u32> InstBegin, PhiBegin, SuccBegin, BlockInfo, BlockInfo2;
    std::vector<TInstRec> Insts;
    std::vector<TPhiRec> Phis;
    std::vector<int> TmpPhi;
    std::vector<u8> IsAlloca, Fused, UsedLocals, ArgLocal;
    std::vector<u32> AllocaSize;
    std::vector<TImmRec> Imms;
    std::vector<TVarRef> VarRefs;
    std::vector<int> SlotRefs, StrRefs; // module-wide, reset per function
};

static_assert(tpde::IRAdaptor<TAdaptor>);

} // namespace NQumir::NCodeGen::NTpde
