#pragma once

// The part of the TPDE compiler that does not depend on the target: type
// rules, memory, arithmetic, conversions, calls. The platform compiler derives
// from TCompilerCommon and adds what differs between the targets: compares and
// flags, blocks of memory, the aggregate argument parts, vector select and
// reductions (see the "Platform hooks" list below).

#include "tpde_adaptor.h"

#include <tpde/CompilerBase.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

namespace NQumir {
namespace NCodeGen {

using namespace NIR;
using namespace NIR::NLiterals;

namespace NTpde {

// Platform hooks, members of TDerived:
//   static Jump CondEq(), CondNe();    conditions of a flag test
//   static constexpr u64 ByteMatchBackend;  value of builtin::byte_match_backend
//   Jump ScalarCompare(TOp, int type, ValuePartRef&& l, ValuePartRef&& r)
//   Jump Int128Compare(TOp, bool isSigned, ValueRef& a, ValueRef& b)
//   void ScalarTest(int type, ValuePartRef&& p)    CondNe holds for "true"
//   bool EmitTest(EValueRef v)
//   void CopyMemory(ValuePartRef&& dst, ValuePartRef&& src, u32 size)
//   void ZeroFrame(u32 frameOff, u32 size)
//   void StoreFrame(u32 frameOff, AsmReg reg, u32 size)
//   template <class TFn> void ForEachAggPart(ValueRef&, const TLayout&, TFn&&)
//   bool CompileSelect(const TInstRec&), CompileReduce(const TInstRec&)
template <typename TDerived, typename TBase, typename TEncode, typename TConfig, typename TAssigner>
struct TCompilerCommon
    : TBase
    , TEncode
{
    using Base = TBase;
    using EncodeCompiler = TEncode;
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

    TDerived& D() {
        return *static_cast<TDerived*>(this);
    }

    struct ValRefSpecial {
        uint8_t mode = 4;
        u32 Imm;
    };

    struct TParts {
        const TLayout* Layout;

        u32 count() const {
            return Layout->Parts.size();
        }
        u32 size_bytes(u32 i) const {
            return Layout->Parts[i].Size;
        }
        tpde::RegBank reg_bank(u32 i) const {
            return Layout->Parts[i].Fp
                ? TConfig::FP_BANK
                : TConfig::GP_BANK;
        }
    };

    explicit TCompilerCommon(TAdaptor* adaptor)
        : Base(adaptor)
        , Ir(adaptor)
        , Info(adaptor->Info)
        , Module(adaptor->Info.Module)
    {
        static_assert(tpde::Compiler<TDerived, TConfig>);
    }

    void reset() {
        Base::reset();
        EncodeCompiler::reset();
    }

    TAdaptor* Ir;
    TModuleInfo& Info;
    TModule& Module;
    std::vector<EValueRef> PendingArgs;
    std::vector<SymRef> StrSyms;
    std::vector<SymRef> SlotSyms;
    SymRef MemsetSym;
    SymRef MemcpySym;
    // Op of the first instruction that codegen gave up on.
    std::string FailedOp;

    // TPDE hooks.

    bool cur_func_may_emit_calls() const {
        return Ir->MayCall();
    }
    SymRef cur_personality_func() const {
        return {};
    }
    // A phi is assigned in the preheader, where TPDE's heuristic counts the
    // loop's own definitions and gives up: loop counters would stay on the stack.
    bool try_force_fixed_assignment(EValueRef v) const {
        return Ir->val_is_phi(v);
    }
    void define_func_idx(EFuncRef, u32) {}
    void setup_var_ref_assignments() {}

    const TLayout& LayoutOf(int type) const {
        return Info.Layout(type);
    }

    TParts val_parts(EValueRef v) const {
        return {&LayoutOf(Ir->ValueType(v))};
    }

    std::optional<ValRefSpecial> val_ref_special(EValueRef v) const {
        if (Ir->IsImm(v)) {
            return ValRefSpecial{.mode = 4, .Imm = u32(v)};
        }
        return std::nullopt;
    }

    ValuePart VarRefPart(int varRef) {
        auto local = tpde::ValLocalIdx(Ir->VarRefLocalIdx(varRef));
        auto* assignment = this->val_assignment(local);
        if (!assignment) {
            this->init_variable_ref(local, varRef);
            assignment = this->val_assignment(local);
        }
        return ValuePart{local, assignment, 0, /*owned=*/false};
    }

    ValuePart val_part_ref_special(ValRefSpecial& vrs, u32 part) {
        const auto& imm = Ir->Imm(EValueRef(vrs.Imm));
        if (imm.VarRef >= 0) {
            return VarRefPart(imm.VarRef);
        }
        const auto& layout = LayoutOf(imm.TypeId);
        const auto& desc = layout.Parts[part];
        auto bank = desc.Fp
            ? TConfig::FP_BANK
            : TConfig::GP_BANK;
        if (layout.Kind != TLayout::Scalar) {
            return ValuePart(u64(0), desc.Size, bank); // zero struct or vector
        }
        const auto& s = Info.Shape(imm.TypeId);
        u64 data = u64(imm.Value);
        if (s.Class == TTypeShape::Int && s.Bits == 1) {
            data = imm.Value != 0;
        }
        if (part == 1) {
            data = s.Signed && imm.Value < 0
                ? ~u64(0)
                : 0;
        }
        if (desc.Size < 8) {
            data &= (u64(1) << (desc.Size * 8)) - 1;
        }
        return ValuePart(data, desc.Size, bank);
    }

    void prologue_assign_arg(tpde::CCAssigner* cc, u32 idx, EValueRef arg) {
        bool wide = Info.Shape(Ir->ValueType(arg)).Bits > 64;
        Base::prologue_assign_arg(cc, idx, arg, wide ? 16 : 1, !wide);
    }

    bool hook_post_func_sym_init() {
        auto rodata = this->assembler.get_default_section(tpde::SectionKind::ReadOnly);
        auto bss = this->assembler.get_default_section(tpde::SectionKind::BSS);
        StrSyms.resize(Module.StringLiterals.size());
        for (size_t i = 1; i < Module.StringLiterals.size(); ++i) {
            const auto& s = Module.StringLiterals[i];
            std::span<const u8> data(reinterpret_cast<const u8*>(s.c_str()), s.size() + 1);
            StrSyms[i] = this->assembler.sym_def_data(rodata, "", data, 1, tpde::Assembler::SymBinding::LOCAL);
        }
        SlotSyms.resize(Module.GlobalTypes.size());
        for (size_t i = 0; i < Module.GlobalTypes.size(); ++i) {
            u32 size = std::max(1, Module.Types.SizeInBytes(Module.GlobalTypes[i]));
            u32 align = std::clamp(Module.Types.AlignInBytes(Module.GlobalTypes[i]), 1, 16);
            SlotSyms[i] = this->assembler.sym_predef_data("", tpde::Assembler::SymBinding::LOCAL);
            this->assembler.sym_def_predef_zero(bss, SlotSyms[i], size, align);
        }
        MemsetSym = this->assembler.sym_add_undef("memset", tpde::Assembler::SymBinding::GLOBAL);
        MemcpySym = this->assembler.sym_add_undef("memcpy", tpde::Assembler::SymBinding::GLOBAL);
        if (Ir->Link) {
            AddStructor(Module.ModuleConstructorFunctionId, /*init=*/true);
            AddStructor(Module.ModuleDestructorFunctionId, /*init=*/false);
        }
        return true;
    }

    // The C runtime calls the hook from this entry, as for llvm.global_ctors/dtors in the LLVM backend.
    void AddStructor(int funcIdx, bool init) {
        if (funcIdx < 0) {
            return;
        }
        auto sec = this->assembler.create_structor_section(init);
        auto& data = this->assembler.get_section(sec).data;
        data.resize(data.size() + 8);
        this->assembler.reloc_abs(sec, this->func_syms[funcIdx], data.size() - 8, 0);
    }

    // Helpers.

    const TTypeShape& Shape(int type) const {
        return Info.Shape(type);
    }

    int TypeOf(EValueRef v) const {
        return Ir->ValueType(v);
    }

    bool IsVec(int type) const {
        return LayoutOf(type).Kind == TLayout::Vector;
    }

    bool IsAgg(int type) const {
        return LayoutOf(type).Kind == TLayout::Aggregate;
    }

    // Lane type of a vector, the type itself for scalars.
    int LaneType(int type) const {
        return IsVec(type)
            ? LayoutOf(type).Elem
            : type;
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
    // implicit casts of the LLVM backend.
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
                return false; // two parts: the callers that support i128 widen
            }
            if (f.Bits > 64 || t.Bits <= f.Bits) {
                return true; // truncation: the upper register bits are don't-care
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
            bool ok = IntToFloat(std::move(p), f.Bits > 32, srcSigned, t.Bits == 64, res);
            p = std::move(res);
            return ok;
        }
        if (f.Class == TTypeShape::Float && t.Class == TTypeShape::Float) {
            ValuePartRef res(this, TConfig::FP_BANK);
            bool ok = t.Bits == 64
                ? this->encode_f32tof64(std::move(p), res)
                : this->encode_f64tof32(std::move(p), res);
            p = std::move(res);
            return ok;
        }
        return false;
    }

    bool IntToFloat(ValuePartRef&& p, bool wide, bool srcSigned, bool toDouble, ValuePart& res) {
        if (toDouble) {
            if (srcSigned) {
                return wide
                    ? this->encode_i64tof64(std::move(p), res)
                    : this->encode_i32tof64(std::move(p), res);
            }
            return wide
                ? this->encode_u64tof64(std::move(p), res)
                : this->encode_u32tof64(std::move(p), res);
        }
        if (srcSigned) {
            return wide
                ? this->encode_i64tof32(std::move(p), res)
                : this->encode_i32tof32(std::move(p), res);
        }
        return wide
            ? this->encode_u64tof32(std::move(p), res)
            : this->encode_u32tof32(std::move(p), res);
    }

    // Part 0 of v converted to `to`.
    bool Operand(ValueRef& vr, EValueRef v, int to, bool srcSigned, ValuePartRef& out) {
        out = vr.part(0);
        return Cast(out, TypeOf(v), to, srcSigned);
    }

    // Lane `lane` of operand v converted to lane type `to`; scalars broadcast.
    bool Lane(ValueRef& vr, EValueRef v, u32 lane, int to, bool srcSigned, ValuePartRef& out) {
        int type = TypeOf(v);
        // A broadcast scalar is read once per lane: no lane may take over its register.
        out = IsVec(type)
            ? vr.part(lane)
            : vr.part_unowned(0);
        return Cast(out, LaneType(type), to, srcSigned);
    }

    // Memory.

    bool LoadScalar(GenericValuePart&& addr, int type, ValuePart& out) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            return s.Bits == 64
                ? this->encode_loadf64(std::move(addr), out)
                : this->encode_loadf32(std::move(addr), out);
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
            return s.Bits == 64
                ? this->encode_storef64(std::move(addr), std::move(v))
                : this->encode_storef32(std::move(addr), std::move(v));
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
    bool StoreValue(ValuePartRef&& base, EValueRef v, int type, bool srcSigned) {
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
            auto local = tpde::ValLocalIdx(u32(Ir->LocalValue(op.Local.Idx)));
            return ValuePartRef(this, ValuePart{local, this->val_assignment(local), 0, /*owned=*/false});
        }
        return ValuePartRef(this, VarRefPart(Ir->SlotVarRef(op.Slot.Idx)));
    }

    int StoredType(const TOperand& op) const {
        return op.Type == TOperand::EType::Local
            ? Ir->Fun->LocalTypes[op.Local.Idx]
            : Module.GlobalTypes[op.Slot.Idx];
    }

    // ValuePart(bank) temporaries report size 0, which CallBuilder and
    // RetBuilder would copy with a 32-bit move.
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
    // or, for address-backed structs, the struct v points to. Parts come one
    // at a time, so fn may pin them to argument registers.
    template <typename TFn>
    bool ForEachPart(EValueRef v, int type, ValueRef& vr, TFn&& fn) {
        const auto& layout = LayoutOf(type);
        int from = TypeOf(v);
        if (layout.Kind == TLayout::Aggregate && Shape(from).Class == TTypeShape::Ptr) {
            D().ForEachAggPart(vr, layout, fn);
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

    // Instruction selection.

    bool compile_inst(EInstRef ref, InstRange remaining) {
        const auto& rec = Ir->Inst(ref);
        if (!rec.Instr) {
            return CompileEntry();
        }
        bool ok = CompileInstr(rec, remaining);
        if (!ok && FailedOp.empty()) {
            FailedOp = rec.Instr->Op.ToString();
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
                auto v = EValueRef(instr.Dest.Idx);
                D().ZeroFrame(FrameOff(v), Ir->val_alloca_size(v));
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
                this->generate_uncond_branch(Ir->BlockOfLabel(instr.Operands[0].Label));
                return true;
            case "cmp"_op:
                return CompileCondBr(rec);
            case "prefetch"_op: {
                // Emits nothing, but the operand use must still be consumed.
                ValueRef vr = this->val_ref(rec.Pos[0]);
                return true;
            }
            case "select"_op:
                return D().CompileSelect(rec);
            case "any"_op:
            case "all"_op:
            case "movemask"_op:
                return D().CompileReduce(rec);
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
        bool sameScalar = LayoutOf(stored).Kind == TLayout::Scalar
            && LayoutOf(type).Kind == TLayout::Scalar
            && Shape(stored).Class == Shape(type).Class
            && Shape(stored).Bits == Shape(type).Bits;
        if (stored == type || sameScalar) {
            return LoadValue(std::move(addr), stored, res);
        }
        // A local read as another scalar type: load, then cast like the LLVM backend.
        if (LayoutOf(stored).Parts.size() != 1 || LayoutOf(type).Parts.size() != 1) {
            return false;
        }
        auto bank = LayoutOf(stored).Parts[0].Fp
            ? TConfig::FP_BANK
            : TConfig::GP_BANK;
        ValuePartRef tmp(this, bank);
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
        EValueRef v = rec.Pos[1];
        if (IsAgg(stored) && Shape(TypeOf(v)).Class == TTypeShape::Ptr) {
            // An address-backed struct: copy the bytes (memcpy in the LLVM backend).
            ValueRef vr = this->val_ref(v);
            D().CopyMemory(Address(op), vr.part(0), std::max(1, Module.Types.SizeInBytes(stored)));
            return true;
        }
        return StoreValue(Address(op), v, stored, true);
    }

    bool CompileSte(const TInstRec& rec) {
        int ptrType = TypeOf(rec.Pos[0]);
        int valType = TypeOf(rec.Pos[1]);
        int storeType = Module.Types.IsPointer(ptrType)
            ? Module.Types.UnderlyingType(ptrType)
            : valType;
        if (LayoutOf(storeType).Kind != TLayout::Scalar) {
            storeType = valType;
        }
        ValueRef ptr = this->val_ref(rec.Pos[0]);
        return StoreValue(ptr.part(0), rec.Pos[1], storeType, valType < 0 || Shape(valType).Signed);
    }

    bool CompileCopy(const TInstRec& rec) {
        EValueRef src = rec.Pos[1];
        int srcType = TypeOf(src);
        if (IsAgg(srcType)) {
            ValueRef dst = this->val_ref(rec.Pos[0]);
            ValueRef size = this->val_ref(rec.Pos[2]);
            return StoreValue(dst.part(0), src, srcType, true);
        }
        // memcpy(dst, src, size); untyped size immediates are i64.
        std::array<typename Base::CallArg, 3> args{
            typename Base::CallArg(rec.Pos[0]),
            typename Base::CallArg(rec.Pos[1]),
            typename Base::CallArg(rec.Pos[2]),
        };
        this->generate_call(MemcpySym, args, nullptr);
        return true;
    }

    u32 FrameOff(EValueRef alloca) {
        return this->val_assignment(tpde::ValLocalIdx(u32(alloca)))->frame_off;
    }

    // Function entry: zero the locals, as the LLVM backend does, and spill
    // the arguments into their locals.
    bool CompileEntry() {
        const auto& fun = *Ir->Fun;
        for (u32 l = 0; l < fun.LocalTypes.size(); ++l) {
            if (Ir->LocalUsed(l) && !Ir->IsArgLocal(l)) {
                auto v = Ir->LocalValue(l);
                D().ZeroFrame(FrameOff(v), Ir->val_alloca_size(v));
            }
        }
        for (int i = 0; i < Ir->ArgCount(); ++i) {
            int local = fun.ArgLocals[i].Idx;
            u32 off = FrameOff(Ir->LocalValue(local));
            ValueRef arg = this->val_ref(Ir->cur_args()[i]);
            const auto& layout = LayoutOf(fun.LocalTypes[local]);
            for (u32 p = 0; p < layout.Parts.size(); ++p) {
                auto part = arg.part(p);
                AsmReg reg = part.cur_reg_or_load();
                D().StoreFrame(off + layout.Parts[p].Offset, reg, layout.Parts[p].Size);
            }
        }
        return true;
    }

    bool FloatBinary(TOp op, bool d, ValuePartRef&& l, ValuePartRef&& r, ValuePart& out) {
        switch (op) {
            case "+"_op:
                return d
                    ? this->encode_addf64(std::move(l), std::move(r), out)
                    : this->encode_addf32(std::move(l), std::move(r), out);
            case "-"_op:
                return d
                    ? this->encode_subf64(std::move(l), std::move(r), out)
                    : this->encode_subf32(std::move(l), std::move(r), out);
            case "*"_op:
                return d
                    ? this->encode_mulf64(std::move(l), std::move(r), out)
                    : this->encode_mulf32(std::move(l), std::move(r), out);
            case "/"_op:
                return d
                    ? this->encode_divf64(std::move(l), std::move(r), out)
                    : this->encode_divf32(std::move(l), std::move(r), out);
            default:
                return false;
        }
    }

    // 32-bit ops compute i8 and i16 too; w64 selects the 64-bit snippet.
    bool IntBinary(TOp op, bool w64, bool isSigned, ValuePartRef&& l, ValuePartRef&& r, ValuePart& out) {
        switch (op) {
            case "+"_op:
                return w64
                    ? this->encode_addi64(std::move(l), std::move(r), out)
                    : this->encode_addi32(std::move(l), std::move(r), out);
            case "-"_op:
                return w64
                    ? this->encode_subi64(std::move(l), std::move(r), out)
                    : this->encode_subi32(std::move(l), std::move(r), out);
            case "*"_op:
                return w64
                    ? this->encode_muli64(std::move(l), std::move(r), out)
                    : this->encode_muli32(std::move(l), std::move(r), out);
            case "&"_op:
            case "&&"_op:
                return w64
                    ? this->encode_landi64(std::move(l), std::move(r), out)
                    : this->encode_landi32(std::move(l), std::move(r), out);
            case "|"_op:
            case "||"_op:
                return w64
                    ? this->encode_lori64(std::move(l), std::move(r), out)
                    : this->encode_lori32(std::move(l), std::move(r), out);
            case "^"_op:
                return w64
                    ? this->encode_lxori64(std::move(l), std::move(r), out)
                    : this->encode_lxori32(std::move(l), std::move(r), out);
            case "<<"_op:
                return w64
                    ? this->encode_shli64(std::move(l), std::move(r), out)
                    : this->encode_shli32(std::move(l), std::move(r), out);
            case ">>"_op:
                if (isSigned) {
                    return w64
                        ? this->encode_ashri64(std::move(l), std::move(r), out)
                        : this->encode_ashri32(std::move(l), std::move(r), out);
                }
                return w64
                    ? this->encode_shri64(std::move(l), std::move(r), out)
                    : this->encode_shri32(std::move(l), std::move(r), out);
            case "/"_op:
                if (isSigned) {
                    return w64
                        ? this->encode_sdivi64(std::move(l), std::move(r), out)
                        : this->encode_sdivi32(std::move(l), std::move(r), out);
                }
                return w64
                    ? this->encode_udivi64(std::move(l), std::move(r), out)
                    : this->encode_udivi32(std::move(l), std::move(r), out);
            case "%"_op:
                if (isSigned) {
                    return w64
                        ? this->encode_sremi64(std::move(l), std::move(r), out)
                        : this->encode_sremi32(std::move(l), std::move(r), out);
                }
                return w64
                    ? this->encode_uremi64(std::move(l), std::move(r), out)
                    : this->encode_uremi32(std::move(l), std::move(r), out);
            default:
                return false;
        }
    }

    // Scalar binary op on parts already converted to `type` (up to 64 bits).
    bool ScalarBinary(TOp op, int type, ValuePartRef&& l, ValuePartRef&& r, ValuePart& out) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            return FloatBinary(op, s.Bits == 64, std::move(l), std::move(r), out);
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
                    auto mask = Const(std::max<int>(1, s.Bits) - 1, 4, TConfig::GP_BANK);
                    if (!this->encode_landi32(std::move(r), std::move(mask), masked)) {
                        return false;
                    }
                    r = std::move(masked);
                    break;
                }
                default:
                    break;
            }
        }
        // x * 2^k as a shift: array indexing would load the constant and
        // queue on the single integer multiply pipeline.
        if (op == "*"_op && l.is_const() && !r.is_const()) {
            std::swap(l, r);
        }
        if (op == "*"_op && r.is_const()) {
            u64 c = r.const_data()[0] & (w64 ? ~u64(0) : 0xffff'ffffu);
            if (c != 0 && (c & (c - 1)) == 0) {
                auto amount = Const(std::countr_zero(c), 8, TConfig::GP_BANK);
                return w64
                    ? this->encode_shli64(std::move(l), std::move(amount), out)
                    : this->encode_shli32(std::move(l), std::move(amount), out);
            }
        }
        return IntBinary(op, w64, s.Signed && s.Class == TTypeShape::Int, std::move(l), std::move(r), out);
    }

    bool Int128Binary(TOp op, bool isSigned, ValueRef& ar, ValueRef& br, EValueRef b, ValueRef& res) {
        if (op == "<<"_op || op == ">>"_op) {
            ValuePartRef amount(this);
            if (!Operand(br, b, Info.I64, true, amount)) {
                return false;
            }
            if (op == "<<"_op) {
                return this->encode_shli128(ar.part(0), ar.part(1), std::move(amount), res.part(0), res.part(1));
            }
            return isSigned
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

    bool CompileBinary(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        int type = TypeOf(rec.Result);
        const auto& s = Shape(type);
        EValueRef a = rec.Pos[0];
        EValueRef b = rec.Pos[1];
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        ValueRef res = this->result_ref(rec.Result);

        if (IsVec(type)) {
            int lane = LayoutOf(type).Elem;
            bool sign = Shape(lane).Signed;
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this);
                ValuePartRef r(this);
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
            // ptr +/- int is a byte offset; narrower indices are sign-extended, as in a GEP.
            ValuePartRef l(this);
            ValuePartRef r(this);
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
            return Int128Binary(op, s.Signed, ar, br, b, res);
        }

        ValuePartRef l(this);
        ValuePartRef r(this);
        if (!Operand(ar, a, type, true, l) || !Operand(br, b, type, true, r)) {
            return false;
        }
        auto out = res.part(0);
        return ScalarBinary(op, type, std::move(l), std::move(r), out);
    }

    // Compare of scalar values a and b (b converted to the type of a).
    std::optional<Jump> EmitCompare(TOp op, EValueRef a, EValueRef b) {
        int type = TypeOf(a);
        const auto& s = Shape(type);
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            if (Shape(TypeOf(b)).Bits != 128) {
                return std::nullopt;
            }
            return D().Int128Compare(op, s.Signed, ar, br);
        }
        if (s.Class != TTypeShape::Int && s.Class != TTypeShape::Ptr && s.Class != TTypeShape::Float) {
            return std::nullopt;
        }
        ValuePartRef l = ar.part(0);
        ValuePartRef r(this);
        if (!Operand(br, b, type, true, r)) {
            return std::nullopt;
        }
        return D().ScalarCompare(op, type, std::move(l), std::move(r));
    }

    bool CompileCompare(const TInstRec& rec, InstRange remaining) {
        const auto op = rec.Instr->Op;
        int type = TypeOf(rec.Result);
        if (IsVec(type)) {
            EValueRef a = rec.Pos[0];
            EValueRef b = rec.Pos[1];
            int lane = LaneType(IsVec(TypeOf(a)) ? TypeOf(a) : TypeOf(b));
            bool sign = Shape(lane).Signed;
            ValueRef ar = this->val_ref(a);
            ValueRef br = this->val_ref(b);
            ValueRef res = this->result_ref(rec.Result);
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this);
                ValuePartRef r(this);
                if (!Lane(ar, a, i, lane, sign, l) || !Lane(br, b, i, lane, sign, r)) {
                    return false;
                }
                Jump j = D().ScalarCompare(op, lane, std::move(l), std::move(r));
                auto out = res.part(i);
                this->generate_raw_set(j, out.alloc_reg());
                out.set_modified();
            }
            return true;
        }
        // Fused with the next instruction `cmp tmp L1 L2` when that is the only user.
        const TInstRec* br = nullptr;
        if (remaining.from != remaining.to) {
            EInstRef next = *remaining.from;
            const auto& n = Ir->Inst(next);
            if (
                n.Instr &&
                n.Instr->Op == "cmp"_op &&
                n.Pos[0] == rec.Result &&
                this->analyzer.liveness_info(tpde::ValLocalIdx(u32(rec.Result))).ref_count <= 2)
            {
                br = &n;
                Ir->inst_set_fused(next);
            }
        }
        auto j = EmitCompare(op, rec.Pos[0], rec.Pos[1]);
        if (!j) {
            return false;
        }
        if (br) {
            auto t = Ir->BlockOfLabel(br->Instr->Operands[1].Label);
            auto f = Ir->BlockOfLabel(br->Instr->Operands[2].Label);
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

    bool CompileCondBr(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        auto t = Ir->BlockOfLabel(instr.Operands[1].Label);
        auto f = Ir->BlockOfLabel(instr.Operands[2].Label);
        if (t == f) {
            {
                // Releases the condition.
                ValueRef vr = this->val_ref(rec.Pos[0]);
            }
            this->generate_uncond_branch(t);
            return true;
        }
        if (Ir->IsImm(rec.Pos[0])) {
            this->generate_uncond_branch(Ir->Imm(rec.Pos[0]).Value != 0 ? t : f);
            return true;
        }
        if (!D().EmitTest(rec.Pos[0])) {
            return false;
        }
        this->generate_cond_branch(TDerived::CondNe(), t, f);
        return true;
    }

    bool CompileTruth(const TInstRec& rec, bool negate) {
        int type = TypeOf(rec.Result);
        Jump j = negate ? TDerived::CondEq() : TDerived::CondNe();
        if (IsVec(type)) {
            EValueRef a = rec.Pos[0];
            ValueRef ar = this->val_ref(a);
            ValueRef res = this->result_ref(rec.Result);
            int lane = LaneType(TypeOf(a));
            for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
                ValuePartRef l(this);
                if (!Lane(ar, a, i, lane, true, l)) {
                    return false;
                }
                D().ScalarTest(lane, std::move(l));
                auto out = res.part(i);
                this->generate_raw_set(j, out.alloc_reg());
                out.set_modified();
            }
            return true;
        }
        if (!D().EmitTest(rec.Pos[0])) {
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
            return s.Bits == 64
                ? this->encode_fnegf64(std::move(v), out)
                : this->encode_fnegf32(std::move(v), out);
        }
        if (s.Bits == 64) {
            return neg
                ? this->encode_negi64(std::move(v), out)
                : this->encode_noti64(std::move(v), out);
        }
        return neg
            ? this->encode_negi32(std::move(v), out)
            : this->encode_noti32(std::move(v), out);
    }

    bool CompileUnary(const TInstRec& rec) {
        int type = TypeOf(rec.Result);
        const auto& s = Shape(type);
        EValueRef a = rec.Pos[0];
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
            return neg
                ? this->encode_negi128(ar.part(0), ar.part(1), res.part(0), res.part(1))
                : this->encode_noti128(ar.part(0), ar.part(1), res.part(0), res.part(1));
        }
        ValuePartRef v(this);
        if (!Operand(ar, a, type, true, v)) {
            return false;
        }
        auto out = res.part(0);
        return UnaryPart(neg, type, std::move(v), out);
    }

    // Widens part 0 of an integer value to i128.
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
            u64 hi = sign && i64(v) < 0
                ? ~u64(0)
                : 0;
            res.part(1).set_value(Const(hi, 8, TConfig::GP_BANK));
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

    bool Bitcast(const TTypeShape& f, const TTypeShape& t, ValuePartRef&& p, ValuePartRef&& out) {
        bool fInt = f.Class == TTypeShape::Int || f.Class == TTypeShape::Ptr;
        bool tInt = t.Class == TTypeShape::Int || t.Class == TTypeShape::Ptr;
        if (fInt && tInt) {
            out.set_value(std::move(p));
            return true;
        }
        if (f.Bits != t.Bits) {
            return false;
        }
        if (f.Class == t.Class) {
            out.set_value(std::move(p));
            return true;
        }
        if (f.Class == TTypeShape::Float) {
            return f.Bits == 64
                ? this->encode_bitcastf64toi64(std::move(p), out)
                : this->encode_bitcastf32toi32(std::move(p), out);
        }
        return t.Bits == 64
            ? this->encode_bitcasti64tof64(std::move(p), out)
            : this->encode_bitcasti32tof32(std::move(p), out);
    }

    // mov, i2f and bitcast.
    bool CompileConvert(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        EValueRef a = rec.Pos[0];
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
            // Lane-wise cast with the element signedness.
            int fl = LayoutOf(from).Elem;
            int tl = LayoutOf(to).Elem;
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
            return Bitcast(f, t, ar.part(0), res.part(0));
        }

        // Integer casts follow the source signedness, i2f too (unsigned for U*).
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
        if (t.Signed && d) {
            return w
                ? this->encode_f64toi64(std::move(in), out)
                : this->encode_f64toi32(std::move(in), out);
        }
        if (t.Signed) {
            return w
                ? this->encode_f32toi64(std::move(in), out)
                : this->encode_f32toi32(std::move(in), out);
        }
        if (d) {
            return w
                ? this->encode_f64tou64(std::move(in), out)
                : this->encode_f64tou32(std::move(in), out);
        }
        return w
            ? this->encode_f32tou64(std::move(in), out)
            : this->encode_f32tou32(std::move(in), out);
    }

    bool CompileF2I(const TInstRec& rec) {
        EValueRef a = rec.Pos[0];
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

    // shuffle with a constant mask, index with a constant lane.
    bool CompileShuffle(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        EValueRef v = rec.Pos[0];
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
                return false; // the LLVM backend reports it at run time
            }
            res.part(0).set_value(vr.part_unowned(lane));
            return true;
        }
        const auto& mask = Module.VectorShuffleMasks.at(instr.Operands[1].Imm.Value);
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

    // builtin:: externals with an inline implementation; nullopt for the others.
    std::optional<bool> CompileBuiltin(const TExternalFunction& ext, const std::vector<EValueRef>& args, EValueRef result) {
        if (ext.Name == "builtin::cttz" || ext.Name == "builtin::ctlz" || ext.Name == "builtin::ctpop") {
            ValueRef vr = this->val_ref(args[0]);
            ValuePartRef p(this);
            if (!Operand(vr, args[0], Info.I64, true, p)) {
                return false;
            }
            if (result == TAdaptor::INVALID_VALUE_REF) {
                return true;
            }
            ValueRef res = this->result_ref(result);
            if (ext.Name == "builtin::cttz") {
                return this->encode_cttzi64(std::move(p), res.part(0));
            }
            if (ext.Name == "builtin::ctlz") {
                return this->encode_ctlzi64(std::move(p), res.part(0));
            }
            return this->encode_ctpopi64(std::move(p), res.part(0));
        }
        if (ext.Name == "builtin::byte_match_backend") {
            // As the LLVM backend on AArch64, so kernels take the same path.
            if (result != TAdaptor::INVALID_VALUE_REF) {
                ValueRef res = this->result_ref(result);
                res.part(0).set_value(Const(TDerived::ByteMatchBackend, 8, TConfig::GP_BANK));
            }
            return true;
        }
        if (ext.Name == "builtin::byte_match8" || ext.Name == "builtin::byte_match16") {
            ValueRef ptr = this->val_ref(args[0]);
            ValueRef byte = this->val_ref(args[1]);
            if (result == TAdaptor::INVALID_VALUE_REF) {
                return true;
            }
            ValueRef res = this->result_ref(result);
            auto ones = Const(0x0101010101010101ull, 8, TConfig::GP_BANK);
            auto low7 = Const(0x7f7f7f7f7f7f7f7full, 8, TConfig::GP_BANK);
            if (ext.Name == "builtin::byte_match8") {
                return this->encode_byte_match8(ptr.part(0), byte.part(0), std::move(ones), std::move(low7), res.part(0));
            }
            auto gather = Const(0x0102040810204080ull, 8, TConfig::GP_BANK);
            return this->encode_byte_match16(
                ptr.part(0),
                byte.part(0),
                std::move(ones),
                std::move(low7),
                std::move(gather),
                res.part(0));
        }
        return std::nullopt;
    }

    bool CompileCall(const TInstRec& rec) {
        const auto& instr = *rec.Instr;
        int sym = instr.Operands[0].Imm.Value;
        std::vector<int> params;
        int retType = -1;
        u32 funcIdx;
        const TExternalFunction* ext = nullptr;
        if (auto it = Module.SymIdToFuncIdx.find(sym); it != Module.SymIdToFuncIdx.end()) {
            const auto& callee = Module.Functions[it->second];
            for (const auto& a : callee.ArgLocals) {
                params.push_back(callee.LocalTypes[a.Idx]);
            }
            retType = callee.ReturnTypeId;
            funcIdx = it->second;
        } else if (auto jt = Module.SymIdToExtFuncIdx.find(sym); jt != Module.SymIdToExtFuncIdx.end()) {
            ext = &Module.ExternalFunctions[jt->second];
            params = ext->ArgTypes;
            retType = ext->ReturnTypeId;
            funcIdx = Ir->NFunc + jt->second;
        } else {
            return false;
        }
        auto args = std::move(PendingArgs);
        PendingArgs.clear();
        if (args.size() != params.size()) {
            return false;
        }

        bool hasResult = rec.Result != TAdaptor::INVALID_VALUE_REF && LayoutOf(retType).Kind != TLayout::Void;
        if (ext) {
            auto result = hasResult
                ? rec.Result
                : TAdaptor::INVALID_VALUE_REF;
            if (auto done = CompileBuiltin(*ext, args, result)) {
                return *done;
            }
        }

        TAssigner assigner;
        CallBuilder cb(D(), assigner);
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
            // The runtime reads a boolean as a whole register.
            bool zextBool = ext && Shape(to).Class == TTypeShape::Int && Shape(to).Bits == 1;
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
        RetBuilder rb(D(), *this->cur_cc_assigner());
        if (rec.Pos[0] != TAdaptor::INVALID_VALUE_REF) {
            EValueRef v = rec.Pos[0];
            int to = Ir->Fun->ReturnTypeId;
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
                    if (isBool) {
                        p = WithSize(MaskBool(std::move(p)), 4);
                    }
                    rb.add(std::move(p), tpde::CCAssignment{});
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

} // namespace NTpde
} // namespace NCodeGen
} // namespace NQumir
