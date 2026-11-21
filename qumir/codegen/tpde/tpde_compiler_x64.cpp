// TPDE compiler for x86-64 (System V, ELF). Most instruction selection comes
// from the snippets (snippets.c, snippets_div.ll) through tpde_encodegen and
// from TCompilerCommon; compares, memory blocks and vector select are here.

// Int128Compare and load_address_of_var_reference are adapted from
// tpde-llvm/src/x64/LLVMCompilerX64.cpp of TPDE (license text: LICENSE.TPDE):
//
// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <bit>
#include "tpde_backend.h"
#include "tpde_check.h"
#include "tpde_compiler_common.h"

#include <tpde/x64/CompilerX64.hpp>

#include "snippets_x64.hpp"

#include <cassert>
#include <exception>

namespace NQumir {
namespace NCodeGen {

using namespace NIR;
using namespace NIR::NLiterals;

namespace NTpde {
namespace {

struct TConfig : tpde::x64::PlatformConfig {
    static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

// System V: a struct that does not fit in the registers left goes to the
// stack whole, while the registers stay free for the arguments after it.
// CCAssignerSysV takes the pieces one by one, so BeginCStruct tells it
// how many registers of each file the next struct needs.
class TAssignerX64 : public tpde::x64::CCAssignerSysV {
public:
    void reset() override {
        CCAssignerSysV::reset();
        GpUsed = FpUsed = StackPieces = 0;
    }

    void BeginCStruct(u32 gp, u32 fp) {
        if (GpUsed + gp > GpArgRegs || FpUsed + fp > FpArgRegs) {
            StackPieces = gp + fp;
        }
    }

    void assign_arg(tpde::CCAssignment& arg) override {
        auto bank = arg.bank;
        if (StackPieces) {
            // A bank without registers: the base class puts the piece on the stack.
            --StackPieces;
            arg.bank = tpde::RegBank{2};
        }
        CCAssignerSysV::assign_arg(arg);
        arg.bank = bank;
        if (arg.reg.valid()) {
            ++(bank == TConfig::GP_BANK ? GpUsed : FpUsed);
        }
    }

private:
    // The argument registers of the base assigner: the general ones are the
    // low 16 bits of the mask, the XMM ones start at bit 32.
    static constexpr u32 GpArgRegs = std::popcount(Info.arg_regs & 0xFFFF);
    static constexpr u32 FpArgRegs = std::popcount(Info.arg_regs >> 32);
    u32 GpUsed = 0, FpUsed = 0, StackPieces = 0;
};

struct TCompilerX64;
using TBaseX64 = tpde::x64::CompilerX64<TAdaptor, TCompilerX64, tpde::CompilerBase, TConfig>;
using TEncodeX64 = tpde_encodegen::EncodeCompiler<TAdaptor, TCompilerX64, tpde::CompilerBase, TConfig>;

struct TCompilerX64
    : TCompilerCommon<TCompilerX64, TBaseX64, TEncodeX64, TConfig, TAssignerX64>
{
    using TCommon = TCompilerCommon<TCompilerX64, TBaseX64, TEncodeX64, TConfig, TAssignerX64>;

    explicit TCompilerX64(TAdaptor* adaptor)
        : TCommon(adaptor)
    {
    }

    // System V returns values in up to 2 general and 2 FP registers.
    static constexpr TReturnLimits ReturnLimits{.Gp = 2, .Fp = 2};
    static constexpr u64 ByteMatchBackend = 2; // SSE2
    static constexpr bool CStructs = true;

    static Jump CondEq() {
        return Jump::je;
    }
    static Jump CondNe() {
        return Jump::jne;
    }

    void load_address_of_var_reference(AsmReg dst, tpde::AssignmentPartRef ap) {
        const auto& ref = Ir->VarRef(ap.variable_ref_data());
        SymRef sym = ref.IsString
            ? StrSyms[ref.Index]
            : SlotSyms[ref.Index];
        ASM(LEA64rm, dst, FE_MEM(FE_IP, 0, FE_NOREG, -1));
        this->reloc_text(sym, tpde::elf::R_X86_64_PC32, this->text_writer.offset() - 4, -4);
    }

    // Memory at base + off; GP registers move whole or partial integers, XMM
    // registers 4 or 8 bytes.

    static bool IsXmm(AsmReg reg) {
        return reg.id() >= AsmReg::XMM0;
    }

    void LoadOff(AsmReg dst, AsmReg base, i32 off, u32 size) {
        assert(size == 1 || size == 2 || size == 4 || size == 8);
        auto mem = FE_MEM(base, 0, FE_NOREG, off);
        if (IsXmm(dst)) {
            if (size == 4) {
                ASM(SSE_MOVD_G2Xrm, dst, mem);
            } else {
                ASM(SSE_MOVQ_G2Xrm, dst, mem);
            }
            return;
        }
        switch (size) {
            case 1:
                ASM(MOVZXr32m8, dst, mem);
                break;
            case 2:
                ASM(MOVZXr32m16, dst, mem);
                break;
            case 4:
                ASM(MOV32rm, dst, mem);
                break;
            default:
                ASM(MOV64rm, dst, mem);
                break;
        }
    }

    void StoreOff(AsmReg base, i32 off, AsmReg src, u32 size) {
        assert(size == 1 || size == 2 || size == 4 || size == 8);
        auto mem = FE_MEM(base, 0, FE_NOREG, off);
        if (IsXmm(src)) {
            if (size == 4) {
                ASM(SSE_MOVD_X2Gmr, mem, src);
            } else {
                ASM(SSE_MOVQ_X2Gmr, mem, src);
            }
            return;
        }
        switch (size) {
            case 1:
                ASM(MOV8mr, mem, src);
                break;
            case 2:
                ASM(MOV16mr, mem, src);
                break;
            case 4:
                ASM(MOV32mr, mem, src);
                break;
            default:
                ASM(MOV64mr, mem, src);
                break;
        }
    }

    // Copies `size` bytes from [src] to [dst].
    void CopyMemory(ValuePartRef&& dst, ValuePartRef&& src, u32 size) {
        if (size > InlineMemoryBytes) {
            tpde::x64::CCAssignerSysV assigner;
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
                this->LoadOff(t, s, pos, chunk);
                this->StoreOff(d, pos, t, chunk);
                pos += chunk;
            }
        }
    }

    // Zero-fills [BP + off, BP + off + size).
    void ZeroFrame(u32 off, u32 size) {
        if (size > InlineMemoryBytes) {
            AsmReg r;
            auto addr = SizedTemp(8, TConfig::GP_BANK, r);
            ASM(LEA64rm, r, FE_MEM(FE_BP, 0, FE_NOREG, i32(off)));
            tpde::x64::CCAssignerSysV assigner;
            CallBuilder cb(*this, assigner);
            cb.add_arg(std::move(addr), tpde::CCAssignment{});
            cb.add_arg(ValuePart(u64(0), 4, TConfig::GP_BANK), tpde::CCAssignment{});
            cb.add_arg(ValuePart(u64(size), 8, TConfig::GP_BANK), tpde::CCAssignment{});
            cb.call(MemsetSym);
            return;
        }
        u32 pos = 0;
        for (u32 chunk : {8u, 4u, 2u, 1u}) {
            while (size - pos >= chunk) {
                auto mem = FE_MEM(FE_BP, 0, FE_NOREG, i32(off + pos));
                switch (chunk) {
                    case 8:
                        ASM(MOV64mi, mem, 0);
                        break;
                    case 4:
                        ASM(MOV32mi, mem, 0);
                        break;
                    case 2:
                        ASM(MOV16mi, mem, 0);
                        break;
                    default:
                        ASM(MOV8mi, mem, 0);
                        break;
                }
                pos += chunk;
            }
        }
    }

    void StoreFrame(u32 frameOff, AsmReg reg, u32 size) {
        this->StoreOff(AsmReg{AsmReg::BP}, i32(frameOff), reg, size);
    }

    ValuePartRef FrameAddress(u32 frameOff) {
        AsmReg r;
        auto addr = SizedTemp(8, TConfig::GP_BANK, r);
        ASM(LEA64rm, r, FE_MEM(FE_BP, 0, FE_NOREG, i32(frameOff)));
        return addr;
    }

    // Copies the frame slot to the next place in the stack argument area. The
    // area is addressed from SP, so it is written before the call.
    void PassByval(TAssignerX64& assigner, i32 frameOff, u32 size, u32 align) {
        tpde::CCAssignment cca{.byval = true, .align = u8(std::max(align, 8u)), .size = size};
        assigner.assign_arg(cca);
        ScratchReg tmp(this);
        AsmReg t = tmp.alloc_gp();
        for (u32 pos = 0; pos < size; pos += 8) {
            this->LoadOff(t, AsmReg{AsmReg::BP}, frameOff + i32(pos), 8);
            this->StoreOff(AsmReg{AsmReg::SP}, i32(cca.stack_off + pos), t, 8);
        }
    }

    // System V: a struct of up to 16 bytes goes by eightbytes, in an FP
    // register if all fields in the eightbyte are floats or doubles and in a
    // general one otherwise, a larger struct in memory.
    TCStruct ClassifyCStruct(int type) const {
        const auto& parts = LayoutOf(type).Parts;
        int size = Module.Types.SizeInBytes(type);
        TCStruct abi{.Align = u32(Module.Types.AlignInBytes(type))};
        if (size > 16) {
            abi.Memory = abi.Byval = true;
            return abi;
        }
        for (int off = 0; off < size; off += 8) {
            const TPartDesc* first = nullptr;
            bool fp = true;
            for (const auto& p : parts) {
                if (p.Offset < off + 8 && p.Offset + p.Size > off) {
                    first = first ? first : &p;
                    fp = fp && p.Fp;
                }
            }
            if (first) {
                abi.Pieces.push_back({u32(off), u8(fp ? std::min(8, size - off) : 8), fp, fp ? first->Type : Info.I64});
            }
        }
        return abi;
    }

    // Calls fn with each part of the struct `vr` points to.
    template <typename TFn>
    void ForEachAggPart(ValueRef& vr, const TLayout& layout, TFn&& fn) {
        for (const auto& d : layout.Parts) {
            AsmReg reg;
            auto bank = d.Fp
                ? TConfig::FP_BANK
                : TConfig::GP_BANK;
            auto p = SizedTemp(d.Size, bank, reg);
            // Passing the previous part may have moved the pointer: read it again.
            {
                // Unlocked before fn: the part may go to the register that holds the pointer.
                auto ptr = vr.part_unowned(0);
                this->LoadOff(reg, ptr.cur_reg_or_load(), d.Offset, d.Size);
            }
            fn(std::move(p));
        }
    }

    // Float compares are ordered except !=, as in the LLVM backend.
    static Jump CondFor(TOp op, bool isSigned) {
        switch (op) {
            case "=="_op:
                return Jump::je;
            case "!="_op:
                return Jump::jne;
            case "<"_op:
                return isSigned
                    ? Jump::jl
                    : Jump::jb;
            case "<="_op:
                return isSigned
                    ? Jump::jle
                    : Jump::jbe;
            case ">"_op:
                return isSigned
                    ? Jump::jg
                    : Jump::ja;
            case ">="_op:
                return isSigned
                    ? Jump::jge
                    : Jump::jae;
            default:
                return Jump::je;
        }
    }

    // ucomis sets CF for "below" and for unordered, ZF for equal and for
    // unordered, PF for unordered. a < b is b > a, so only ja and jae are used:
    // both are false when unordered. == and != combine ZF with PF in a register.
    Jump FloatCompare(TOp op, bool d, ValuePartRef&& l, ValuePartRef&& r) {
        AsmReg lr = l.cur_reg_or_load();
        AsmReg rr = r.cur_reg_or_load();
        if (op == "<"_op || op == "<="_op) {
            std::swap(lr, rr);
        }
        if (d) {
            ASM(SSE_UCOMISDrr, lr, rr);
        } else {
            ASM(SSE_UCOMISSrr, lr, rr);
        }
        switch (op) {
            case "<"_op:
            case ">"_op:
                return Jump::ja;
            case "<="_op:
            case ">="_op:
                return Jump::jae;
            default:
                break;
        }
        ScratchReg a(this);
        ScratchReg b(this);
        AsmReg ar = a.alloc_gp();
        AsmReg br = b.alloc_gp();
        if (op == "=="_op) {
            ASM(SETNP8r, br);
            ASM(SETZ8r, ar);
            ASM(AND8rr, ar, br);
        } else {
            ASM(SETP8r, br);
            ASM(SETNZ8r, ar);
            ASM(OR8rr, ar, br);
        }
        return Jump::jne;
    }

    // Flag-setting compare of parts l and r of scalar `type` (up to 64 bits);
    // returns the condition that holds when the comparison is true.
    Jump ScalarCompare(TOp op, int type, ValuePartRef&& l, ValuePartRef&& r) {
        const auto& s = Shape(type);
        if (s.Class == TTypeShape::Float) {
            return FloatCompare(op, s.Bits == 64, std::move(l), std::move(r));
        }
        // false < true: bools compare unsigned.
        bool sign = s.Class == TTypeShape::Int && s.Signed && s.Bits != 1;
        Jump j = CondFor(op, sign);
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
        if (r.is_const() && (bits == 32 || i64(i32(r.const_data()[0])) == i64(r.const_data()[0]))) {
            i32 imm = i32(r.const_data()[0]);
            if (imm == 0) {
                if (bits == 32) {
                    ASM(TEST32rr, lr, lr);
                } else {
                    ASM(TEST64rr, lr, lr);
                }
            } else if (bits == 32) {
                ASM(CMP32ri, lr, imm);
            } else {
                ASM(CMP64ri, lr, imm);
            }
            return j;
        }
        AsmReg rr = r.cur_reg_or_load();
        if (bits == 32) {
            ASM(CMP32rr, lr, rr);
        } else {
            ASM(CMP64rr, lr, rr);
        }
        return j;
    }

    Jump Int128Compare(TOp op, bool isSigned, ValueRef& ar, ValueRef& br) {
        Jump j = CondFor(op, isSigned);
        ValueRef* l = &ar;
        ValueRef* r = &br;
        // The carried subtraction below leaves ZF of the high half only: the
        // conditions that read ZF swap the operands.
        if (j == Jump::ja || j == Jump::jbe || j == Jump::jle || j == Jump::jg) {
            std::swap(l, r);
            j = this->swap_jump(j);
        }
        auto rlo = r->part(0);
        auto rhi = r->part(1);
        auto lhi = l->part(1).into_temporary();
        AsmReg rloReg = rlo.load_to_reg();
        AsmReg rhiReg = rhi.cur_reg_or_load();
        if (j == Jump::je || j == Jump::jne) {
            auto llo = l->part(0).into_temporary();
            ASM(XOR64rr, llo.cur_reg(), rloReg);
            ASM(XOR64rr, lhi.cur_reg(), rhiReg);
            ASM(OR64rr, llo.cur_reg(), lhi.cur_reg());
        } else {
            auto llo = l->part(0);
            ASM(CMP64rr, llo.load_to_reg(), rloReg);
            ASM(SBB64rr, lhi.cur_reg(), rhiReg);
        }
        return j;
    }

    // Sets the flags so that CondNe means "part p of `type` is true": non-zero,
    // for floats also unordered.
    void ScalarTest(int type, ValuePartRef&& p) {
        const auto& s = Shape(type);
        AsmReg r = p.cur_reg_or_load();
        if (s.Class == TTypeShape::Float) {
            ScratchReg zero(this);
            ScratchReg a(this);
            ScratchReg b(this);
            AsmReg z = zero.alloc(TConfig::FP_BANK);
            AsmReg ar = a.alloc_gp();
            AsmReg br = b.alloc_gp();
            ASM(SSE_XORPSrr, z, z);
            if (s.Bits == 64) {
                ASM(SSE_UCOMISDrr, r, z);
            } else {
                ASM(SSE_UCOMISSrr, r, z);
            }
            ASM(SETP8r, br);
            ASM(SETNZ8r, ar);
            ASM(OR8rr, ar, br);
            return;
        }
        switch (s.Class == TTypeShape::Ptr ? 64 : s.Bits) {
            case 1:
                ASM(TEST32ri, r, 1);
                break;
            case 8:
                ASM(TEST32ri, r, 0xff);
                break;
            case 16:
                ASM(TEST32ri, r, 0xffff);
                break;
            case 32:
                ASM(TEST32rr, r, r);
                break;
            default:
                ASM(TEST64rr, r, r);
                break;
        }
    }

    bool EmitTest(EValueRef v) {
        int type = TypeOf(v);
        const auto& s = Shape(type);
        ValueRef vr = this->val_ref(v);
        if (s.Class == TTypeShape::Int && s.Bits == 128) {
            auto lo = vr.part(0).into_temporary();
            auto hi = vr.part(1);
            ASM(OR64rr, lo.cur_reg(), hi.cur_reg_or_load());
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
            // mov keeps the flags.
            this->mov(dst, fr, w64 ? 8 : 4);
            if (fp) {
                // There is no conditional move for XMM registers: skip the copy when false.
                auto skip = this->text_writer.label_create();
                this->generate_raw_jump(Jump::je, skip);
                this->mov(dst, tr, w64 ? 8 : 4);
                this->label_place(skip);
            } else {
                this->generate_raw_cmov(Jump::jne, dst, tr, w64);
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
            this->generate_raw_set(Jump::jne, i == 0 ? ar : br);
            if (i == 0) {
                continue;
            }
            if (op == "any"_op) {
                ASM(OR32rr, ar, br);
            } else if (op == "all"_op) {
                ASM(AND32rr, ar, br);
            } else {
                ASM(SHL32ri, br, i);
                ASM(OR32rr, ar, br);
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
        if (auto unsupported = NTpde::FindUnsupported(info, NTpde::TCompilerX64::ReturnLimits, NTpde::TCompilerX64::CStructs)) {
            return std::unexpected(TError("tpde: unsupported " + *unsupported));
        }
        NTpde::TAdaptor adaptor(info, use == EObjectUse::Link);
        NTpde::TCompilerX64 compiler(&adaptor);
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
