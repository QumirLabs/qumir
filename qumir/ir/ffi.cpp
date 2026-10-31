#include "ffi.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#error "NFFI supports only x86-64 and AArch64"
#endif

#if defined(__APPLE__)
#define QFFI_ASM_NAME "_QumirFfiInvoke"
#else
#define QFFI_ASM_NAME "QumirFfiInvoke"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
asm(
    ".text\n"
    ".globl " QFFI_ASM_NAME "\n"
    ".p2align 2\n"
    QFFI_ASM_NAME ":\n"
    // CFI lets an exception thrown by the callee unwind through this frame.
    "    .cfi_startproc\n"
    "    stp x29, x30, [sp, #-48]!\n"
    "    .cfi_def_cfa_offset 48\n"
    "    .cfi_offset 29, -48\n"
    "    .cfi_offset 30, -40\n"
    "    mov x29, sp\n"
    "    .cfi_def_cfa_register 29\n"
    "    stp x19, x20, [sp, #16]\n"
    "    .cfi_offset 19, -32\n"
    "    .cfi_offset 20, -24\n"
    "    str x21, [sp, #32]\n"
    "    .cfi_offset 21, -16\n"
    "    mov x19, x0\n"
    "    mov x20, x1\n"
    "    mov x21, x2\n"
    "    ldr x9, [x20, #144]\n"
    "    cbz x9, 2f\n"
    "    lsl x10, x9, #3\n"
    "    add x10, x10, #15\n"
    "    and x10, x10, #0xfffffffffffffff0\n"
    "    sub sp, sp, x10\n"
    "    ldr x11, [x20, #136]\n"
    "    mov x12, sp\n"
    "1:\n"
    "    ldr x13, [x11], #8\n"
    "    str x13, [x12], #8\n"
    "    subs x9, x9, #1\n"
    "    bne 1b\n"
    "2:\n"
    "    ldp d0, d1, [x20, #0]\n"
    "    ldp d2, d3, [x20, #16]\n"
    "    ldp d4, d5, [x20, #32]\n"
    "    ldp d6, d7, [x20, #48]\n"
    "    ldp x0, x1, [x20, #64]\n"
    "    ldp x2, x3, [x20, #80]\n"
    "    ldp x4, x5, [x20, #96]\n"
    "    ldp x6, x7, [x20, #112]\n"
    "    ldr x8, [x20, #128]\n"
    "    blr x19\n"
    "    str x0, [x21, #0]\n"
    "    str x1, [x21, #8]\n"
    "    str d0, [x21, #16]\n"
    "    str d1, [x21, #24]\n"
    "    mov sp, x29\n"
    "    ldp x19, x20, [sp, #16]\n"
    "    ldr x21, [sp, #32]\n"
    "    ldp x29, x30, [sp], #48\n"
    "    ret\n"
    "    .cfi_endproc\n"
);
#else
asm(
    ".text\n"
    ".globl " QFFI_ASM_NAME "\n"
    ".p2align 4\n"
    QFFI_ASM_NAME ":\n"
    // CFI lets an exception thrown by the callee unwind through this frame.
    "    .cfi_startproc\n"
    "    pushq %rbp\n"
    "    .cfi_def_cfa_offset 16\n"
    "    .cfi_offset 6, -16\n"
    "    movq %rsp, %rbp\n"
    "    .cfi_def_cfa_register 6\n"
    "    pushq %r12\n"
    "    .cfi_offset 12, -24\n"
    "    pushq %r13\n"
    "    .cfi_offset 13, -32\n"
    "    pushq %r14\n"
    "    .cfi_offset 14, -40\n"
    "    pushq %rbx\n"
    "    .cfi_offset 3, -48\n"
    "    movq %rdi, %r12\n"
    "    movq %rsi, %r14\n"
    "    movq %rdx, %r13\n"
    "    movq 144(%r14), %rcx\n"
    "    testq %rcx, %rcx\n"
    "    je 2f\n"
    "    movq %rcx, %rax\n"
    "    shlq $3, %rax\n"
    "    addq $15, %rax\n"
    "    andq $-16, %rax\n"
    "    subq %rax, %rsp\n"
    "    movq 136(%r14), %r8\n"
    "    movq %rsp, %r9\n"
    "1:\n"
    "    movq (%r8), %rax\n"
    "    movq %rax, (%r9)\n"
    "    addq $8, %r8\n"
    "    addq $8, %r9\n"
    "    decq %rcx\n"
    "    jne 1b\n"
    "2:\n"
    "    movsd 0(%r14), %xmm0\n"
    "    movsd 8(%r14), %xmm1\n"
    "    movsd 16(%r14), %xmm2\n"
    "    movsd 24(%r14), %xmm3\n"
    "    movsd 32(%r14), %xmm4\n"
    "    movsd 40(%r14), %xmm5\n"
    "    movsd 48(%r14), %xmm6\n"
    "    movsd 56(%r14), %xmm7\n"
    "    movq 64(%r14), %rdi\n"
    "    movq 72(%r14), %rsi\n"
    "    movq 80(%r14), %rdx\n"
    "    movq 88(%r14), %rcx\n"
    "    movq 96(%r14), %r8\n"
    "    movq 104(%r14), %r9\n"
    "    movb $8, %al\n"
    "    call *%r12\n"
    "    movq %rax, 0(%r13)\n"
    "    movq %rdx, 8(%r13)\n"
    "    movsd %xmm0, 16(%r13)\n"
    "    movsd %xmm1, 24(%r13)\n"
    "    leaq -32(%rbp), %rsp\n"
    "    popq %rbx\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbp\n"
    "    ret\n"
    "    .cfi_endproc\n"
);
#endif

namespace NQumir {
namespace NIR {
namespace NFFI {

namespace {

static_assert(sizeof(void*) == sizeof(int64_t));

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr bool IsArm = true;
constexpr int NGpr = 8;
constexpr int NFp = 8;
#else
constexpr bool IsArm = false;
constexpr int NGpr = 6;
constexpr int NFp = 8;
#endif

constexpr size_t MaxStack = 64;
static_assert(MaxStack <= 255);

struct TFrame {
    double Fp[8];
    uint64_t Gpr[8];
    uint64_t Indirect;
    const uint64_t* Stack;
    uint64_t StackCount;
};

static_assert(offsetof(TFrame, Fp) == 0);
static_assert(offsetof(TFrame, Gpr) == 64);
static_assert(offsetof(TFrame, Indirect) == 128);
static_assert(offsetof(TFrame, Stack) == 136);
static_assert(offsetof(TFrame, StackCount) == 144);

struct TResult {
    uint64_t Rax;
    uint64_t Rdx;
    double Xmm0;
    double Xmm1;
};

static_assert(offsetof(TResult, Rax) == 0);
static_assert(offsetof(TResult, Rdx) == 8);
static_assert(offsetof(TResult, Xmm0) == 16);
static_assert(offsetof(TResult, Xmm1) == 24);

extern "C" void QumirFfiInvoke(void* fn, const TFrame* frame, TResult* out);

enum class EFile : uint8_t { Gpr, Fp, Stack };
enum class ERet : uint8_t { Void, Gpr, Fp, StructRegs, StructMem };
enum class EResSrc : uint8_t { Rax, Rdx, Xmm0, Xmm1 };

struct TMove {
    uint8_t Src;
    bool Deref;
    uint8_t Offset;
    EFile File;
    uint8_t Index;
};

uint64_t ReadResSrc(const TResult& r, EResSrc s) {
    switch (s) {
        case EResSrc::Rax: return r.Rax;
        case EResSrc::Rdx: return r.Rdx;
        case EResSrc::Xmm0: return std::bit_cast<uint64_t>(r.Xmm0);
        case EResSrc::Xmm1: return std::bit_cast<uint64_t>(r.Xmm1);
    }
    return 0;
}

bool IsGprScalar(EKind k) {
    switch (k) {
        case EKind::I1:
        case EKind::I8:
        case EKind::I16:
        case EKind::I32:
        case EKind::I64:
        case EKind::U8:
        case EKind::U16:
        case EKind::U32:
        case EKind::U64:
        case EKind::Ptr:
            return true;
        default:
            return false;
    }
}

struct TDynamicFunction : public IFunction {
    void* Symbol = nullptr;
    std::vector<TMove> Moves;
    uint64_t StackCount = 0;
    bool HasSret = false;
    bool SretInGpr = false;
    ERet RetMode = ERet::Void;
    uint8_t RetEbCount = 0;
    EResSrc RetSrc[2] = {};
    size_t RetCopyBytes = 0;

    uint64_t operator() (const uint64_t* args, size_t) override {
        TFrame frame = {};
        uint64_t stackBuf[MaxStack];

        if (HasSret) {
            if (SretInGpr) {
                frame.Gpr[0] = args[0];
            } else {
                frame.Indirect = args[0];
            }
        }

        for (const TMove& m : Moves) {
            uint64_t v;
            if (m.Deref) {
                std::memcpy(&v,
                    reinterpret_cast<const char*>(static_cast<uintptr_t>(args[m.Src])) + m.Offset,
                    sizeof(uint64_t));
            } else {
                v = args[m.Src];
            }
            switch (m.File) {
                case EFile::Gpr:
                    frame.Gpr[m.Index] = v;
                    break;
                case EFile::Fp:
                    std::memcpy(&frame.Fp[m.Index], &v, sizeof(double));
                    break;
                case EFile::Stack:
                    stackBuf[m.Index] = v;
                    break;
            }
        }

        frame.Stack = stackBuf;
        frame.StackCount = StackCount;

        TResult result = {};
        QumirFfiInvoke(Symbol, &frame, &result);

        switch (RetMode) {
            case ERet::Void:
                return 0;
            case ERet::Gpr:
                return result.Rax;
            case ERet::Fp:
                return std::bit_cast<uint64_t>(result.Xmm0);
            case ERet::StructRegs: {
                uint64_t tmp[2] = {0, 0};
                for (uint8_t e = 0; e < RetEbCount; ++e) {
                    tmp[e] = ReadResSrc(result, RetSrc[e]);
                }
                std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(args[0])), tmp, RetCopyBytes);
                return args[0];
            }
            case ERet::StructMem:
                return args[0];
        }
        return 0;
    }
};

bool StructEightbytes(EStructKind sk, EFile (&files)[2], uint8_t& count) {
    switch (sk) {
        case EStructKind::Int:
            count = 1;
            files[0] = EFile::Gpr;
            return true;
        case EStructKind::Sse:
            count = 1;
            files[0] = EFile::Fp;
            return true;
        case EStructKind::IntInt:
            count = 2;
            files[0] = EFile::Gpr;
            files[1] = EFile::Gpr;
            return true;
        case EStructKind::SseSse:
            count = 2;
            files[0] = EFile::Fp;
            files[1] = EFile::Fp;
            return true;
        case EStructKind::IntSse:
            count = 2;
            files[0] = EFile::Gpr;
            files[1] = IsArm ? EFile::Gpr : EFile::Fp;
            return true;
        case EStructKind::SseInt:
            count = 2;
            files[0] = IsArm ? EFile::Gpr : EFile::Fp;
            files[1] = EFile::Gpr;
            return true;
        default:
            return false;
    }
}

} // namespace

IFunction* BuildFFI(
    void* symbol,
    EKind retKind,
    EStructKind retStruct,
    size_t retSize,
    const std::vector<EKind>& argKinds,
    const std::vector<EStructKind>& argStructs,
    const std::vector<size_t>& argSizes) noexcept
{
    if (argKinds.size() != argStructs.size()) {
        return nullptr;
    }

    const bool hasStructRet = (retKind == EKind::Struct);
    const bool isMemRet = hasStructRet && retStruct == EStructKind::Memory;

    int ngrp = 0;
    int nfp = 0;
    size_t nstack = 0;
    std::vector<TMove> moves;

    if (isMemRet && !IsArm) {
        ngrp = 1;
    }

    auto placeGpr = [&](uint8_t src, bool deref, uint8_t off) {
        if (ngrp < NGpr) {
            moves.push_back({src, deref, off, EFile::Gpr, static_cast<uint8_t>(ngrp++)});
        } else {
            moves.push_back({src, deref, off, EFile::Stack, static_cast<uint8_t>(nstack++)});
            if (IsArm) {
                ngrp = NGpr;
            }
        }
    };

    auto placeFp = [&](uint8_t src, bool deref, uint8_t off) {
        if (nfp < NFp) {
            moves.push_back({src, deref, off, EFile::Fp, static_cast<uint8_t>(nfp++)});
        } else {
            moves.push_back({src, deref, off, EFile::Stack, static_cast<uint8_t>(nstack++)});
            if (IsArm) {
                nfp = NFp;
            }
        }
    };

    auto placeStruct = [&](uint8_t src, EStructKind sk) -> bool {
        EFile files[2] = {};
        uint8_t neb = 0;
        if (!StructEightbytes(sk, files, neb)) {
            return false;
        }
        int gNeed = 0;
        int fNeed = 0;
        for (uint8_t e = 0; e < neb; ++e) {
            (files[e] == EFile::Gpr ? gNeed : fNeed)++;
        }
        const bool fits = (ngrp + gNeed <= NGpr) && (nfp + fNeed <= NFp);
        if (fits) {
            for (uint8_t e = 0; e < neb; ++e) {
                if (files[e] == EFile::Gpr) {
                    moves.push_back({src, true, static_cast<uint8_t>(e * 8), EFile::Gpr, static_cast<uint8_t>(ngrp++)});
                } else {
                    moves.push_back({src, true, static_cast<uint8_t>(e * 8), EFile::Fp, static_cast<uint8_t>(nfp++)});
                }
            }
        } else {
            for (uint8_t e = 0; e < neb; ++e) {
                moves.push_back({src, true, static_cast<uint8_t>(e * 8), EFile::Stack, static_cast<uint8_t>(nstack++)});
            }
            if (IsArm) {
                if (gNeed > 0) {
                    ngrp = NGpr;
                }
                if (fNeed > 0) {
                    nfp = NFp;
                }
            }
        }
        return true;
    };

    const uint8_t base = hasStructRet ? 1 : 0;
    for (size_t i = 0; i < argKinds.size(); ++i) {
        const auto src = static_cast<uint8_t>(base + i);
        const EKind k = argKinds[i];
        if (k == EKind::Struct) {
            if (argStructs[i] == EStructKind::Memory) {
                if (IsArm) {
                    // AAPCS: a pointer to a caller-owned copy in a GP register.
                    placeGpr(src, false, 0);
                } else {
                    // SysV: the struct is copied by value onto the stack.
                    const size_t sz = i < argSizes.size() ? argSizes[i] : 0;
                    if (sz == 0) {
                        return nullptr;
                    }
                    for (size_t off = 0; off < sz; off += 8) {
                        moves.push_back({src, true, static_cast<uint8_t>(off), EFile::Stack, static_cast<uint8_t>(nstack++)});
                    }
                }
            } else if (!placeStruct(src, argStructs[i])) {
                return nullptr;
            }
        } else if (k == EKind::F64) {
            placeFp(src, false, 0);
        } else if (IsGprScalar(k)) {
            placeGpr(src, false, 0);
        } else {
            return nullptr;
        }
    }

    if (nstack > MaxStack) {
        return nullptr;
    }

    auto fn = std::make_unique<TDynamicFunction>();
    fn->Symbol = symbol;
    fn->Moves = std::move(moves);
    fn->StackCount = static_cast<uint64_t>(nstack);
    fn->HasSret = isMemRet;
    fn->SretInGpr = !IsArm;

    if (retKind == EKind::Void) {
        fn->RetMode = ERet::Void;
    } else if (retKind == EKind::F64) {
        fn->RetMode = ERet::Fp;
    } else if (IsGprScalar(retKind)) {
        fn->RetMode = ERet::Gpr;
    } else if (hasStructRet) {
        if (isMemRet) {
            fn->RetMode = ERet::StructMem;
        } else {
            fn->RetMode = ERet::StructRegs;
            size_t classBytes = 16;
            switch (retStruct) {
                case EStructKind::Int:
                    fn->RetEbCount = 1;
                    fn->RetSrc[0] = EResSrc::Rax;
                    classBytes = 8;
                    break;
                case EStructKind::Sse:
                    fn->RetEbCount = 1;
                    fn->RetSrc[0] = EResSrc::Xmm0;
                    classBytes = 8;
                    break;
                case EStructKind::IntInt:
                    fn->RetEbCount = 2;
                    fn->RetSrc[0] = EResSrc::Rax;
                    fn->RetSrc[1] = EResSrc::Rdx;
                    break;
                case EStructKind::SseSse:
                    fn->RetEbCount = 2;
                    fn->RetSrc[0] = EResSrc::Xmm0;
                    fn->RetSrc[1] = EResSrc::Xmm1;
                    break;
                case EStructKind::IntSse:
                    fn->RetEbCount = 2;
                    fn->RetSrc[0] = EResSrc::Rax;
                    fn->RetSrc[1] = IsArm ? EResSrc::Rdx : EResSrc::Xmm0;
                    break;
                case EStructKind::SseInt:
                    fn->RetEbCount = 2;
                    fn->RetSrc[0] = IsArm ? EResSrc::Rax : EResSrc::Xmm0;
                    fn->RetSrc[1] = IsArm ? EResSrc::Rdx : EResSrc::Rax;
                    break;
                default:
                    return nullptr;
            }
            fn->RetCopyBytes = (retSize > 0) ? std::min(retSize, classBytes) : classBytes;
        }
    } else {
        return nullptr;
    }

    return fn.release();
}

} // namespace NFFI
} // namespace NIR
} // namespace NQumir
