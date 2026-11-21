// TPDE compiler for AArch64. Most instruction selection comes from the
// snippets (snippets.c, snippets_div.ll) through tpde_encodegen and from
// TCompilerCommon; compares, memory blocks and vector select are here.

// Int128Compare and load_address_of_var_reference are adapted from
// tpde-llvm/src/arm64/LLVMCompilerArm64.cpp of TPDE (license text: LICENSE.TPDE):
//
// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "tpde_backend.h"
#include "tpde_check.h"
#include "tpde_compiler_common.h"

#include <tpde/arm64/CompilerA64.hpp>

#include "snippets_a64.hpp"

#include <exception>

namespace NQumir {
namespace NCodeGen {

using namespace NIR;
using namespace NIR::NLiterals;

namespace NTpde {
namespace {

struct TConfig : tpde::a64::PlatformConfig {
    static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

struct TCompilerA64;
using TBaseA64 = tpde::a64::CompilerA64<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>;
using TEncodeA64 = tpde_encodegen::EncodeCompiler<TAdaptor, TCompilerA64, tpde::CompilerBase, TConfig>;

struct TCompilerA64
    : TCompilerCommon<TCompilerA64, TBaseA64, TEncodeA64, TConfig, tpde::a64::CCAssignerAAPCS>
{
    using TCommon = TCompilerCommon<TCompilerA64, TBaseA64, TEncodeA64, TConfig, tpde::a64::CCAssignerAAPCS>;

    explicit TCompilerA64(TAdaptor* adaptor)
        : TCommon(adaptor)
    {
    }

    // AArch64 returns values in up to 8 general and 8 FP registers.
    static constexpr TReturnLimits ReturnLimits{.Gp = 8, .Fp = 8};
    static constexpr u64 ByteMatchBackend = 1; // NEON

    static Jump CondEq() {
        return Jump::Jeq;
    }
    static Jump CondNe() {
        return Jump::Jne;
    }

    void load_address_of_var_reference(AsmReg dst, tpde::AssignmentPartRef ap) {
        const auto& ref = Ir->VarRef(ap.variable_ref_data());
        SymRef sym = ref.IsString
            ? StrSyms[ref.Index]
            : SlotSyms[ref.Index];
        this->text_writer.ensure_space(8);
        this->reloc_text(sym, tpde::elf::R_AARCH64_ADR_PREL_PG_HI21, this->text_writer.offset());
        ASMNC(ADRP, dst, 0, 0);
        this->reloc_text(sym, tpde::elf::R_AARCH64_ADD_ABS_LO12_NC, this->text_writer.offset());
        ASMNC(ADDxi, dst, dst, 0);
    }

    // Copies `size` bytes from [src] to [dst].
    void CopyMemory(ValuePartRef&& dst, ValuePartRef&& src, u32 size) {
        if (size > InlineMemoryBytes) {
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

    // Zero-fills [FP + off, FP + off + size).
    void ZeroFrame(u32 off, u32 size) {
        if (size > InlineMemoryBytes) {
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

    void StoreFrame(u32 frameOff, AsmReg reg, u32 size) {
        this->store_off(AsmReg{AsmReg::FP}, frameOff, reg, size);
    }

    // Calls fn with each part of the struct `vr` points to.
    template <typename TFn>
    void ForEachAggPart(ValueRef& vr, const TLayout& layout, TFn&& fn) {
        // TPDE reserves x17, so argument registers never clobber it.
        AsmReg base{AsmReg::R17};
        {
            auto ptr = vr.part(0);
            ASM(MOVx, base, ptr.cur_reg_or_load());
        }
        for (const auto& d : layout.Parts) {
            AsmReg reg;
            auto bank = d.Fp
                ? TConfig::FP_BANK
                : TConfig::GP_BANK;
            auto p = SizedTemp(d.Size, bank, reg);
            this->load_off(reg, base, d.Offset, d.Size);
            fn(std::move(p));
        }
    }

    // Float compares are ordered except !=, as in the LLVM backend.
    static typename Jump::Kind CondFor(TOp op, bool isFloat, bool isSigned) {
        switch (op) {
            case "=="_op:
                return Jump::Jeq;
            case "!="_op:
                return Jump::Jne;
            case "<"_op:
                if (isFloat) {
                    return Jump::Jmi;
                }
                return isSigned
                    ? Jump::Jlt
                    : Jump::Jlo;
            case "<="_op:
                if (isFloat || !isSigned) {
                    return Jump::Jls;
                }
                return Jump::Jle;
            case ">"_op:
                if (isFloat || isSigned) {
                    return Jump::Jgt;
                }
                return Jump::Jhi;
            case ">="_op:
                if (isFloat || isSigned) {
                    return Jump::Jge;
                }
                return Jump::Jhs;
            default:
                return Jump::Jeq;
        }
    }

    // Flag-setting compare of parts l and r of scalar `type` (up to 64 bits);
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
        // false < true: bools compare unsigned.
        bool sign = s.Class == TTypeShape::Int && s.Signed && s.Bits != 1;
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
            bool done = bits == 32
                ? ASMIF(CMPwi, lr, imm)
                : ASMIF(CMPxi, lr, imm);
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

    Jump Int128Compare(TOp op, bool isSigned, ValueRef& ar, ValueRef& br) {
        auto lloP = ar.part(0);
        AsmReg llo = lloP.cur_reg_or_load();
        auto lhiP = ar.part(1);
        AsmReg lhi = lhiP.cur_reg_or_load();
        auto rloP = br.part(0);
        AsmReg rlo = rloP.cur_reg_or_load();
        auto rhiP = br.part(1);
        AsmReg rhi = rhiP.cur_reg_or_load();
        Jump j(CondFor(op, false, isSigned));
        if (j.kind == Jump::Jeq || j.kind == Jump::Jne) {
            ASM(CMPx, llo, rlo);
            ASM(CCMPx, lhi, rhi, 0, DA_EQ);
        } else if (j.kind == Jump::Jhi || j.kind == Jump::Jls || j.kind == Jump::Jle || j.kind == Jump::Jgt) {
            // SBCS leaves Z of the high half only, so the conditions that read Z swap the operands.
            j = this->swap_jump(j);
            ASM(CMPx, rlo, llo);
            ASM(SBCSx, DA_ZR, rhi, lhi);
        } else {
            ASM(CMPx, llo, rlo);
            ASM(SBCSx, DA_ZR, lhi, rhi);
        }
        return j;
    }

    // Sets the flags so that Jne means "part p of `type` is true": non-zero,
    // for floats also unordered.
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
            case 1:
                ASM(TSTwi, r, 1);
                break;
            case 8:
                ASM(TSTwi, r, 0xff);
                break;
            case 16:
                ASM(TSTwi, r, 0xffff);
                break;
            case 32:
                ASM(CMPwi, r, 0);
                break;
            default:
                ASM(CMPxi, r, 0);
                break;
        }
    }

    bool EmitTest(EValueRef v) {
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

    bool CompileSelect(const TInstRec& rec) {
        int type = TypeOf(rec.Result);
        if (!IsVec(type)) {
            return false;
        }
        EValueRef m = rec.Pos[0];
        EValueRef a = rec.Pos[1];
        EValueRef b = rec.Pos[2];
        int lane = LayoutOf(type).Elem;
        int mlane = LaneType(TypeOf(m));
        bool fp = Shape(lane).Class == TTypeShape::Float;
        bool w64 = Shape(lane).Bits == 64 || Shape(lane).Class == TTypeShape::Ptr;
        ValueRef mr = this->val_ref(m);
        ValueRef ar = this->val_ref(a);
        ValueRef br = this->val_ref(b);
        ValueRef res = this->result_ref(rec.Result);
        for (u32 i = 0; i < LayoutOf(type).Parts.size(); ++i) {
            ValuePartRef t(this);
            ValuePartRef f(this);
            if (!Lane(ar, a, i, lane, true, t) || !Lane(br, b, i, lane, true, f)) {
                return false;
            }
            ValuePartRef c = IsVec(TypeOf(m))
                ? mr.part(i)
                : mr.part_unowned(0);
            AsmReg tr = t.cur_reg_or_load();
            AsmReg fr = f.cur_reg_or_load();
            ScalarTest(mlane, std::move(c));
            auto out = res.part(i);
            AsmReg dst = out.alloc_reg();
            if (fp && Shape(lane).Bits == 64) {
                ASM(FCSELd, dst, tr, fr, DA_NE);
            } else if (fp) {
                ASM(FCSELs, dst, tr, fr, DA_NE);
            } else {
                this->generate_raw_select(Jump(Jump::Jne), dst, tr, fr, w64);
            }
            out.set_modified();
        }
        return true;
    }

    // any, all and movemask over the truth values of the lanes.
    bool CompileReduce(const TInstRec& rec) {
        const auto op = rec.Instr->Op;
        EValueRef v = rec.Pos[0];
        int type = TypeOf(v);
        if (!IsVec(type)) {
            return false;
        }
        int lane = LayoutOf(type).Elem;
        ValueRef vr = this->val_ref(v);
        ValueRef res = this->result_ref(rec.Result);
        ScratchReg acc(this);
        ScratchReg bit(this);
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

};

} // namespace
} // namespace NTpde

std::expected<std::string, TError> TTpdeBackend::EmitObject(NIR::TModule& module, EObjectUse use) {
    try {
        NTpde::TModuleInfo info(module);
        if (auto unsupported = NTpde::FindUnsupported(info, NTpde::TCompilerA64::ReturnLimits)) {
            return std::unexpected(TError("tpde: unsupported " + *unsupported));
        }
        NTpde::TAdaptor adaptor(info, use == EObjectUse::Link);
        NTpde::TCompilerA64 compiler(&adaptor);
        if (!compiler.compile()) {
            return std::unexpected(TError(compiler.FailedOp.empty()
                ? std::string("tpde: compilation failed")
                : "tpde: cannot compile " + compiler.FailedOp));
        }
        auto object = compiler.assembler.build_object_file();
        return std::string(object.begin(), object.end());
    } catch (const std::exception& e) {
        return std::unexpected(TError(std::string("tpde codegen error: ") + e.what()));
    }
}

std::expected<std::string, TError> TTpdeBackend::EmitAsm(NIR::TModule&) {
    return std::unexpected(TError("tpde: no assembly output, use the LLVM backend"));
}

} // namespace NCodeGen
} // namespace NQumir
