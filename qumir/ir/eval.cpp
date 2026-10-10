#include "eval.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <cassert>
#include <sstream>
#include <cstring>
#include <type_traits>

#include <qumir/runtime/string.h> // for str_release
#include <qumir/runtime/drawer.h>
#include <qumir/runtime/painter.h>
#include <qumir/runtime/robot.h>
#include <qumir/runtime/turtle.h>
#include <qumir/runtime/io.h>
#include <qumir/runtime/future.h>
#include <qumir/future.h>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

namespace {

template<typename Dest=int64_t>
inline Dest ReadOperand(const TRegisterFile& regs, const TVMOperand& op) {
    switch (op.Type) {
        case TVMOperand::EType::Tmp: {
            const auto& t = op.Tmp;
            if constexpr (sizeof(Dest) == sizeof(int64_t)) {
                return std::bit_cast<Dest>(regs.Get<int64_t>(t.Idx));
            } else {
                return regs.Get<Dest>(t.Idx);
            }
        }
        case TVMOperand::EType::Imm: {
            const auto& i = op.Imm;
            if constexpr (sizeof(Dest) == sizeof(int64_t)) {
                return std::bit_cast<Dest>(i.Value);
            } else {
                return static_cast<Dest>(i.Value);
            }
        }
        default: {
            assert(false && "Slot operand not supported in ALU operations");
            return 0;
        }
    }
}

template<typename Dest, typename T>
inline int64_t EvalAlu(const TRegisterFile& regs, const TVMInstr& instr, T lambda) {
    Dest lhs = ReadOperand<Dest>(regs, instr.Operands[1]);
    Dest rhs = ReadOperand<Dest>(regs, instr.Operands[2]);
    auto res = lambda(lhs, rhs);
    if constexpr (sizeof(res) == sizeof(int64_t)) {
        return std::bit_cast<int64_t>(res);
    } else {
        return static_cast<int64_t>(res);
    }
}

template<typename T>
inline auto Alu128(const TRegisterFile& regs, const TVMInstr& instr, T lambda) {
    return lambda(ReadOperand<__int128_t>(regs, instr.Operands[1]),
                  ReadOperand<__int128_t>(regs, instr.Operands[2]));
}

template<typename T>
inline auto AluU128(const TRegisterFile& regs, const TVMInstr& instr, T lambda) {
    return lambda(std::bit_cast<__uint128_t>(ReadOperand<__int128_t>(regs, instr.Operands[1])),
                  std::bit_cast<__uint128_t>(ReadOperand<__int128_t>(regs, instr.Operands[2])));
}

void CopyOperand(void* destination, size_t size, const TVMOperand& source, const TRegisterFile& regs) {
    size_t effectiveSize = size;
    const void* value;
    if (source.Type == TVMOperand::EType::Tmp) {
        assert(source.Tmp.Idx >= 0 && static_cast<size_t>(source.Tmp.Idx) + size <= regs.Size());
        value = regs.Data() + source.Tmp.Idx;
    } else {
        assert(source.Type == TVMOperand::EType::Imm);
        effectiveSize = std::min(size, sizeof(source.Imm.Value));
        value = &source.Imm.Value;
    }
    std::memcpy(destination, value, effectiveSize);
    if (effectiveSize < size) {
        // A wide immediate extends its sign; vector immediates are only zero.
        std::memset(static_cast<char*>(destination) + effectiveSize,
            source.Imm.Value < 0 ? 0xff : 0, size - effectiveSize);
    }
}

template<typename T>
T ReadVectorOperand(const TRegisterFile& regs, const TVMOperand& operand, int offset) {
    if (operand.Type == TVMOperand::EType::Tmp) {
        return regs.Get<T>(operand.Tmp.Idx + offset);
    }
    assert(operand.Type == TVMOperand::EType::Imm);
    if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(static_cast<uint32_t>(operand.Imm.Value));
    } else {
        return ReadOperand<T>(regs, operand);
    }
}

template<typename T, bool Boolean, bool Unary, typename TOp>
void EvalVectorLanes(TRegisterFile& regs, const TVMInstr& instr, TOp op) {
    // Unsigned arithmetic gives modular integer lanes without signed overflow
    // or the overflowing int promotions of small integer multiplication.
    using TCalc = std::conditional_t<std::is_floating_point_v<T>, T, uint64_t>;
    using TResult = std::conditional_t<Boolean, uint8_t, T>;
    const int leftStride = (!Unary && (instr.Format & TVMInstr::BroadcastLeft))
        ? 0
        : sizeof(T);
    const int rightStride = (instr.Format & TVMInstr::BroadcastRight)
        ? 0
        : sizeof(T);
    for (size_t i = 0; i < instr.LaneCount(); ++i) {
        const auto left = ReadVectorOperand<T>(regs, instr.Operands[1], static_cast<int>(i) * leftStride);
        T right = 0;
        if constexpr (!Unary) {
            right = ReadVectorOperand<T>(regs, instr.Operands[2], static_cast<int>(i) * rightStride);
        }
        regs.Get<TResult>(instr.Operands[0].Tmp.Idx + static_cast<int>(i * sizeof(TResult))) =
            static_cast<TResult>(op(static_cast<TCalc>(left), static_cast<TCalc>(right)));
    }
}

template<bool Floating, bool Signed = false, typename TVisitor>
void VisitVectorElement(size_t size, TVisitor visitor) {
    if constexpr (Floating) {
        switch (size) {
            case 4: visitor.template operator()<float>(); return;
            case 8: visitor.template operator()<double>(); return;
        }
    } else {
        switch (size) {
            case 1: visitor.template operator()<std::conditional_t<Signed, int8_t, uint8_t>>(); return;
            case 2: visitor.template operator()<std::conditional_t<Signed, int16_t, uint16_t>>(); return;
            case 4: visitor.template operator()<std::conditional_t<Signed, int32_t, uint32_t>>(); return;
            case 8: visitor.template operator()<std::conditional_t<Signed, int64_t, uint64_t>>(); return;
        }
    }
    throw std::runtime_error("Unsupported vector element width");
}

template<bool Floating, bool Boolean = false, bool Unary = false, typename TOp>
void EvalVectorAlu(TRegisterFile& regs, const TVMInstr& instr, TOp op) {
    VisitVectorElement<Floating>(instr.ElementSizeInBytes(), [&]<typename T>() {
        EvalVectorLanes<T, Boolean, Unary>(regs, instr, op);
    });
}

int64_t SignedVectorLane(uint64_t value, size_t size) {
    const int shift = static_cast<int>(64 - size * 8);
    return std::bit_cast<int64_t>(value << shift) >> shift;
}

template<typename TOp>
void EvalSignedVectorComparison(TRegisterFile& regs, const TVMInstr& instr, TOp op) {
    EvalVectorAlu<false, true>(regs, instr, [&](uint64_t a, uint64_t b) {
        return op(SignedVectorLane(a, instr.ElementSizeInBytes()), SignedVectorLane(b, instr.ElementSizeInBytes()));
    });
}

template<typename TVisitor>
void VisitVectorCastElement(size_t size, bool floating, bool signedInteger, TVisitor visitor) {
    if (floating) {
        VisitVectorElement<true>(size, visitor);
    } else if (signedInteger) {
        VisitVectorElement<false, true>(size, visitor);
    } else {
        VisitVectorElement<false>(size, visitor);
    }
}

void EvalVectorCast(TRegisterFile& regs, const TVMInstr& instr) {
    const bool sourceFloat = instr.Op == EVMOp::VF2I || instr.Op == EVMOp::VFCast;
    const bool resultFloat = instr.Op == EVMOp::VI2F || instr.Op == EVMOp::VFCast;
    VisitVectorCastElement(instr.ElementSizeInBytes(), sourceFloat, instr.Format & TVMInstr::SignedElement,
        [&]<typename TSource>() {
            VisitVectorCastElement(instr.Operands[2].Imm.Value, resultFloat, instr.Format & TVMInstr::SignedResult,
                [&]<typename TResult>() {
                    for (size_t i = 0; i < instr.LaneCount(); ++i) {
                        const auto value = ReadVectorOperand<TSource>(regs, instr.Operands[1], static_cast<int>(i * sizeof(TSource)));
                        regs.Get<TResult>(instr.Operands[0].Tmp.Idx + static_cast<int>(i * sizeof(TResult))) = static_cast<TResult>(value);
                    }
                });
        });
}

uint64_t EvalVectorIndex(const TRegisterFile& regs, const TVMInstr& instr) {
    const int64_t index = ReadOperand(regs, instr.Operands[2]);
    if (index < 0 || static_cast<uint64_t>(index) >= instr.LaneCount()) {
        throw std::runtime_error("Vector index out of bounds");
    }
    const size_t size = instr.ElementSizeInBytes();
    const int offset = static_cast<int>(index * size);
    uint64_t value;
    VisitVectorElement<false>(size, [&]<typename T>() {
        value = ReadVectorOperand<T>(regs, instr.Operands[1], offset);
    });
    if ((instr.Format & TVMInstr::SignedElement) && size < 8) {
        value = std::bit_cast<uint64_t>(SignedVectorLane(value, size));
    }
    return value;
}

ITypeErasedFuture* MakeCompletedVoidFuture() {
    auto promise = std::make_shared<TPromise<void>>();
    promise->return_void();
    return new TWrappedFuture<void>(MakeExternalFuture<void>(promise));
}

ITypeErasedFuture* MakeCompletedValueFuture(uint64_t value) {
    auto promise = std::make_shared<TPromise<uint64_t>>();
    promise->return_value(value);
    return new TWrappedFuture<uint64_t>(MakeExternalFuture<uint64_t>(promise));
}

} // namespace

TInterpreter::TInterpreter(
    TModule& module,
    TVMCompiler& compiler,
    std::ostream& out,
    std::istream& in,
    IDebugger* debugger)
    : Out(out)
    , In(in)
    , Module(module)
    , Compiler(compiler)
    , Debugger(debugger)
{ }

std::optional<std::string> TInterpreter::Eval(TFunction& function, std::vector<int64_t> args, TInterpreter::TOptions options)
{
    if (Module.ModuleConstructorFunctionId != -1) {
        DoEvalRaw(Module.Functions[Module.ModuleConstructorFunctionId], {}, options);
    }
    auto raw = DoEvalRaw(function, std::move(args), options);
    if (!raw) {
        if (Module.ModuleDestructorFunctionId != -1) {
            DoEvalRaw(Module.Functions[Module.ModuleDestructorFunctionId], {}, options);
        }
        return std::nullopt;
    }
    std::ostringstream out;
    const int resultTypeId = function.IsCoroutine && function.CoroutineResultTypeId >= 0
        ? function.CoroutineResultTypeId
        : function.ReturnTypeId;
    if (function.ReturnTypeIsString) {
        // TODO: remove me, clutch: support string returnType
        char* strPtr = reinterpret_cast<char*>(std::bit_cast<uint64_t>(*raw));
        out << strPtr;
        NRuntime::str_release(strPtr);
    } else if (resultTypeId >= 0) {
        // Structured results live in Runtime, not in the completed coroutine frame;
        // format them before the module destructor can invalidate that storage.
        Module.Types.Format(out, std::bit_cast<uint64_t>(*raw), resultTypeId);
    } else {
        out << *raw;
    }
    if (Module.ModuleDestructorFunctionId != -1) {
        DoEvalRaw(Module.Functions[Module.ModuleDestructorFunctionId], {}, options);
    }
    return out.str();
}

std::optional<int64_t> TInterpreter::EvalRaw(TFunction& function, std::vector<int64_t> args, TInterpreter::TOptions options)
{
    if (Module.ModuleConstructorFunctionId != -1) {
        DoEvalRaw(Module.Functions[Module.ModuleConstructorFunctionId], {}, options);
    }
    auto ans = DoEvalRaw(function, std::move(args), options);
    if (Module.ModuleDestructorFunctionId != -1) {
        DoEvalRaw(Module.Functions[Module.ModuleDestructorFunctionId], {}, options);
    }
    return ans;
}

size_t TInterpreter::ProcessAsyncRuntimeEvents() {
    size_t processed = 0;
    processed += NRuntime::robot_process_events();
    processed += NRuntime::turtle_process_events();
    processed += NRuntime::drawer_process_events();
    processed += NRuntime::painter_process_events();
    processed += NRuntime::io_process_events();
    return processed;
}

std::optional<int64_t> TInterpreter::DoEvalRaw(TFunction& function, std::vector<int64_t> args, TOptions options) {
    if (!function.Exec) {
        function.Exec = &Compiler.Compile(function, options.PrintByteCode);
    }
    TFuture<std::optional<int64_t>> future = [&]() -> TFuture<std::optional<int64_t>> {
        if (Debugger) {
            Debugger->SetRuntime(Runtime);
            return DoEvalRawAsync<true>(function, std::move(args), options);
        } else {
            return DoEvalRawAsync<false>(function, std::move(args), options);
        }
    }();
    while (!future.done()) {
        bool hasEvents = ProcessAsyncRuntimeEvents() > 0;
        assert(hasEvents && "coroutine suspended with no pending async events");
    }
    ProcessAsyncRuntimeEvents();
    return future.await_resume();
}

template<bool EnableDebug>
TFuture<std::optional<int64_t>> TInterpreter::DoEvalRawAsync(TFunction& function, std::vector<int64_t> args, TInterpreter::TOptions options) {
    if (!function.Exec) {
        function.Exec = &Compiler.Compile(function, options.PrintByteCode);
    }
    Runtime.CallStack.clear();
    auto& callStack = Runtime.CallStack;
    callStack.reserve(16);
    auto* execFunc = function.Exec;
    callStack.push_back(TFrame {
        .Exec = execFunc,
        .StackBase = 0,
        .PC = &execFunc->VMCode[0],
        .Name = function.Name,
    });

    static constexpr size_t MaxStackSize = 128 * 1024 * 1024; // 128M

    Runtime.Regs.Reset(execFunc->RegisterFileSize, execFunc->RegisterFileAlignment);
    Runtime.Stack.reserve(MaxStackSize);
    Runtime.Stack.resize(execFunc->NumLocals, 0); // NumLocals is frame size in bytes
    if (args.size() != function.ArgLocals.size()) {
        std::cerr << "Function " << function.Name << " expects " << function.ArgLocals.size() << " arguments, got " << args.size() << "\n";
        co_return std::nullopt;
    }

    auto copyArgsToFrame = [&](char* frameBase, const TExecFunc* exec,
                                const int64_t* srcArgs, int srcCount) {
        for (int i = 0; i < srcCount; ++i) {
            const int byteOff = (i < (int)exec->ArgByteOffsets.size())
                ? exec->ArgByteOffsets[i] : i * 8;
            const int typeId = (i < (int)exec->ArgTypeIds.size()) ? exec->ArgTypeIds[i] : -1;
            const int argSize = Module.Types.SizeInBytes(typeId);
            if (typeId >= 0 && Module.Types.GetKind(typeId) == EKind::Struct) {
                // struct arg: value is a pointer - copy the struct into the frame
                std::memcpy(frameBase + byteOff, reinterpret_cast<const void*>(srcArgs[i]), argSize);
            } else if (typeId >= 0 && argSize == 16 && i < (int)Runtime.Args128.size()) {
                std::memcpy(frameBase + byteOff, &Runtime.Args128[i], 16);
            } else {
                std::memcpy(frameBase + byteOff, &srcArgs[i], 8);
            }
        }
    };

    copyArgsToFrame(Runtime.Stack.data(), execFunc, args.data(), (int)args.size());

    std::optional<int64_t> retVal;
    bool retIs128 = false;
    auto materializeStructTmp = [&](const TFrame& targetFrame, int32_t registerOffset, const void* src) -> std::optional<int64_t> {
        const TExecFunc* exec = targetFrame.Exec;
        auto it = exec->StructRegisters.find(registerOffset);
        if (it == exec->StructRegisters.end()) {
            return std::nullopt;
        }
        const size_t size = static_cast<size_t>(it->second.Size);
        const size_t byteOffset = targetFrame.StackBase + it->second.FrameOffset;
        assert(byteOffset + size <= Runtime.Stack.size());
        char* temp = Runtime.Stack.data() + byteOffset;
        if (src) {
            std::memcpy(temp, src, size);
        } else {
            std::memset(temp, 0, size);
        }
        return reinterpret_cast<int64_t>(temp);
    };

    while (!callStack.empty()) {
        auto& frame = callStack.back();
        assert(frame.PC <= &frame.Exec->VMCode[frame.Exec->VMCode.size()-1]);
        assert(frame.PC >= &frame.Exec->VMCode[0]);

        if constexpr (EnableDebug) {
            Debugger->OnInstruction(frame);
        }

        const auto& instr = *frame.PC++;

        switch (instr.Op) {
        case EVMOp::StructStore: { // dst=Local (byte offset in frame), src=Tmp (pointer), size=Imm
            const size_t byteOffset = frame.StackBase + instr.Operands[0].Local.Idx;
            void* dst = Runtime.Stack.data() + byteOffset;
            void* src = reinterpret_cast<void*>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]));
            int64_t size = instr.Operands[2].Imm.Value;
            std::memcpy(dst, src, static_cast<size_t>(size));
            break;
        }
        case EVMOp::Copy: { // dst/src are Tmp (pointers), size is Imm
            void* dst = reinterpret_cast<void*>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[0]));
            void* src = reinterpret_cast<void*>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]));
            int64_t size = instr.Operands[2].Imm.Value;
            std::memcpy(dst, src, static_cast<size_t>(size));
            break;
        }
        case EVMOp::SAlloc: {
            const size_t offset = static_cast<size_t>(instr.Operands[1].Imm.Value);
            const size_t size = static_cast<size_t>(instr.Operands[2].Imm.Value);
            const size_t byteOffset = frame.StackBase + offset;
            assert(byteOffset + size <= Runtime.Stack.size());
            char* addrPtr = Runtime.Stack.data() + byteOffset;
            std::memset(addrPtr, 0, size);
            int64_t addr = reinterpret_cast<int64_t>(addrPtr);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = addr;
            break;
        }
        case EVMOp::Ste: {
            int64_t intAddr = ReadOperand<int64_t>(Runtime.Regs, instr.Operands[0]);
            void* addr = reinterpret_cast<void*>(intAddr);
            int64_t value = ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]);
            size_t size = static_cast<size_t>(instr.Operands[2].Imm.Value);
            if (size == 0 || size > sizeof(int64_t)) {
                size = sizeof(int64_t);
            }
            //std::cerr << "ste addr " << std::hex << addr << std::dec << " = " << value << "\n";
            std::memcpy(addr, &value, size);
            break;
        }
        case EVMOp::Lde: {
            int64_t intAddr = ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]);
            void* addr = reinterpret_cast<void*>(intAddr);
            int64_t value = 0;
            size_t size = static_cast<size_t>(instr.Operands[2].Imm.Value);
            if (size == 0 || size > sizeof(int64_t)) {
                size = sizeof(int64_t);
            }
            std::memcpy(&value, addr, size);
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = value;
            break;
        }
        case EVMOp::Lea: {
            // load addr of local or slot operand
            assert(instr.Operands[0].Tmp.Idx >= 0);
            if (instr.Operands[1].Type == TVMOperand::EType::Slot) {
                const auto& s = instr.Operands[1].Slot;
                const size_t byteOffset = s.Idx * 8;
                assert(s.Idx >= 0 && byteOffset < Runtime.Globals.size());
                int64_t addr = reinterpret_cast<int64_t>(Runtime.Globals.data() + byteOffset);
                Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = addr;
            } else if (instr.Operands[1].Type == TVMOperand::EType::Local) {
                const auto& l = instr.Operands[1].Local;
                const size_t byteOffset = frame.StackBase + l.Idx; // l.Idx is byte offset from vmcompiler
                assert(l.Idx >= 0 && byteOffset < Runtime.Stack.size());
                int64_t addr = reinterpret_cast<int64_t>(Runtime.Stack.data() + byteOffset);
                Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = addr;
            } else {
                assert(false && "Invalid operand for lea");
            }
            break;
        }
        case EVMOp::Load: {
            const size_t size = instr.SizeInBytes();
            const auto& dest = instr.Operands[0];
            assert(dest.Type == TVMOperand::EType::Tmp && dest.Tmp.Idx >= 0);
            assert(static_cast<size_t>(dest.Tmp.Idx) + size <= Runtime.Regs.Size());
            auto* value = Runtime.Regs.Data() + dest.Tmp.Idx;
            if (instr.Operands[1].Type == TVMOperand::EType::Slot) {
                const auto& s = instr.Operands[1].Slot;
                const size_t byteOffset = static_cast<size_t>(s.Idx) * 8;
                assert(s.Idx >= 0 && byteOffset + size <= Runtime.Globals.size());
                std::memcpy(value, Runtime.Globals.data() + byteOffset, size);
            } else if (instr.Operands[1].Type == TVMOperand::EType::Local) {
                const auto& l = instr.Operands[1].Local;
                const size_t byteOffset = frame.StackBase + l.Idx; // l.Idx is byte offset
                assert(l.Idx >= 0 && byteOffset + size <= Runtime.Stack.size());
                std::memcpy(value, Runtime.Stack.data() + byteOffset, size);
            } else if (instr.Operands[1].Type == TVMOperand::EType::Tmp) {
                const auto* address = reinterpret_cast<const void*>(ReadOperand(Runtime.Regs, instr.Operands[1]));
                std::memcpy(value, address, size);
            } else {
                assert(false && "Invalid operand for load");
            }
            break;
        }
        case EVMOp::Store: {
            const size_t size = instr.SizeInBytes();
            const auto& source = instr.Operands[1];
            char* destination;
            if (instr.Operands[0].Type == TVMOperand::EType::Slot) {
                const auto& s = instr.Operands[0].Slot;
                assert(s.Idx >= 0);
                const size_t byteOffset = static_cast<size_t>(s.Idx) * 8;
                if (byteOffset + size > Runtime.Globals.size()) {
                    Runtime.Globals.resize(byteOffset + size, 0);
                }
                destination = Runtime.Globals.data() + byteOffset;
            } else if (instr.Operands[0].Type == TVMOperand::EType::Local) {
                const auto& l = instr.Operands[0].Local;
                const size_t byteOffset = frame.StackBase + l.Idx; // l.Idx is byte offset
                assert(l.Idx >= 0 && byteOffset + size <= Runtime.Stack.size());
                destination = Runtime.Stack.data() + byteOffset;
            } else if (instr.Operands[0].Type == TVMOperand::EType::Tmp) {
                destination = reinterpret_cast<char*>(ReadOperand(Runtime.Regs, instr.Operands[0]));
            } else {
                assert(false && "Invalid operand for store");
                break;
            }
            CopyOperand(destination, size, source, Runtime.Regs);
            break;
        }

        case EVMOp::VIndex:
            Runtime.Regs.Get<uint64_t>(instr.Operands[0].Tmp.Idx) = EvalVectorIndex(Runtime.Regs, instr);
            break;
        case EVMOp::VAll:
        case EVMOp::VAny: {
            const bool all = instr.Op == EVMOp::VAll;
            bool result = all;
            for (size_t i = 0; i < instr.LaneCount(); ++i) {
                const bool lane = ReadVectorOperand<uint8_t>(Runtime.Regs, instr.Operands[1], static_cast<int>(i)) != 0;
                if (lane != all) {
                    result = lane;
                    break;
                }
            }
            Runtime.Regs.Get<uint64_t>(instr.Operands[0].Tmp.Idx) = result;
            break;
        }
        case EVMOp::VShuffle: {
            if (instr.Operands[1].Type == TVMOperand::EType::Imm) {
                assert(instr.Operands[1].Imm.Value == 0);
                std::memset(Runtime.Regs.Data() + instr.Operands[0].Tmp.Idx, 0, instr.SizeInBytes());
                break;
            }
            const auto* mask = reinterpret_cast<const int*>(instr.Operands[2].Imm.Value);
            std::array<uint8_t, 256> result;
            const auto size = instr.ElementSizeInBytes();
            const auto* source = Runtime.Regs.Data() + instr.Operands[1].Tmp.Idx;
            for (size_t i = 0; i < instr.LaneCount(); ++i) {
                assert(mask[i] >= 0 && static_cast<size_t>(mask[i]) < instr.LaneCount());
                std::memcpy(result.data() + i * size, source + mask[i] * size, size);
            }
            std::memcpy(Runtime.Regs.Data() + instr.Operands[0].Tmp.Idx, result.data(), instr.SizeInBytes());
            break;
        }
        case EVMOp::VICast:
        case EVMOp::VI2F:
        case EVMOp::VF2I:
        case EVMOp::VFCast:
            EvalVectorCast(Runtime.Regs, instr);
            break;
        case EVMOp::VIAdd:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::plus<>{});
            break;
        case EVMOp::VISub:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::minus<>{});
            break;
        case EVMOp::VIMul:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::multiplies<>{});
            break;
        case EVMOp::VFAdd:
            EvalVectorAlu<true>(Runtime.Regs, instr, std::plus<>{});
            break;
        case EVMOp::VFSub:
            EvalVectorAlu<true>(Runtime.Regs, instr, std::minus<>{});
            break;
        case EVMOp::VFMul:
            EvalVectorAlu<true>(Runtime.Regs, instr, std::multiplies<>{});
            break;

        case EVMOp::VIAnd:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::bit_and<>{});
            break;
        case EVMOp::VIOr:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::bit_or<>{});
            break;
        case EVMOp::VIXor:
            EvalVectorAlu<false>(Runtime.Regs, instr, std::bit_xor<>{});
            break;
        case EVMOp::VIShl:
            EvalVectorAlu<false>(Runtime.Regs, instr, [&](uint64_t a, uint64_t b) {
                return a << (b & (instr.ElementSizeInBytes() * 8 - 1));
            });
            break;
        case EVMOp::VIShrU:
            EvalVectorAlu<false>(Runtime.Regs, instr, [&](uint64_t a, uint64_t b) {
                return a >> (b & (instr.ElementSizeInBytes() * 8 - 1));
            });
            break;
        case EVMOp::VIShrS:
            EvalVectorAlu<false>(Runtime.Regs, instr, [&](uint64_t a, uint64_t b) {
                return SignedVectorLane(a, instr.ElementSizeInBytes()) >> (b & (instr.ElementSizeInBytes() * 8 - 1));
            });
            break;
        case EVMOp::VIBitNot:
            EvalVectorAlu<false, false, true>(Runtime.Regs, instr, [](uint64_t a, uint64_t) { return ~a; });
            break;
        case EVMOp::VINot:
        case EVMOp::VI2B:
            EvalVectorAlu<false, true, true>(Runtime.Regs, instr, [&](uint64_t a, uint64_t) {
                return (a != 0) != (instr.Op == EVMOp::VINot);
            });
            break;
        case EVMOp::VFNot:
        case EVMOp::VF2B:
            EvalVectorAlu<true, true, true>(Runtime.Regs, instr, [&](auto a, auto) {
                return (a != 0) != (instr.Op == EVMOp::VFNot);
            });
            break;
        case EVMOp::VICmpLTU:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::less<>{});
            break;
        case EVMOp::VICmpLTS:
            EvalSignedVectorComparison(Runtime.Regs, instr, std::less<>{});
            break;
        case EVMOp::VICmpGTU:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::greater<>{});
            break;
        case EVMOp::VICmpGTS:
            EvalSignedVectorComparison(Runtime.Regs, instr, std::greater<>{});
            break;
        case EVMOp::VICmpLEU:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::less_equal<>{});
            break;
        case EVMOp::VICmpLES:
            EvalSignedVectorComparison(Runtime.Regs, instr, std::less_equal<>{});
            break;
        case EVMOp::VICmpGEU:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::greater_equal<>{});
            break;
        case EVMOp::VICmpGES:
            EvalSignedVectorComparison(Runtime.Regs, instr, std::greater_equal<>{});
            break;
        case EVMOp::VICmpEQ:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::equal_to<>{});
            break;
        case EVMOp::VICmpNE:
            EvalVectorAlu<false, true>(Runtime.Regs, instr, std::not_equal_to<>{});
            break;
        case EVMOp::VFCmpLT:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::less<>{});
            break;
        case EVMOp::VFCmpGT:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::greater<>{});
            break;
        case EVMOp::VFCmpLE:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::less_equal<>{});
            break;
        case EVMOp::VFCmpGE:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::greater_equal<>{});
            break;
        case EVMOp::VFCmpEQ:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::equal_to<>{});
            break;
        case EVMOp::VFCmpNE:
            EvalVectorAlu<true, true>(Runtime.Regs, instr, std::not_equal_to<>{});
            break;

        case EVMOp::INeg:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = -ReadOperand(Runtime.Regs, instr.Operands[1]);
            break;
        case EVMOp::FNeg: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            double tmp = ReadOperand<double>(Runtime.Regs, instr.Operands[1]);
            tmp = -tmp;
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(tmp);
            break;
        }
        case EVMOp::INot:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = !ReadOperand(Runtime.Regs, instr.Operands[1]);
            break;
        case EVMOp::IBitNot: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            auto value = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[1]);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(~value);
            break;
        }

        case EVMOp::IAdd:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::plus<int64_t>{});
            break;
        case EVMOp::FAdd:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::plus<double>{});
            break;

        case EVMOp::ISub:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::minus<int64_t>{});
            break;
        case EVMOp::FSub:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::minus<double>{});
            break;

        case EVMOp::IMulS:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::multiplies<int64_t>{});
            break;
        case EVMOp::FMul:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::multiplies<double>{});
            break;

        case EVMOp::IDivS:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::divides<int64_t>{});
            break;
        case EVMOp::IDivU:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::divides<uint64_t>{});
            break;
        case EVMOp::IRemS:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::modulus<int64_t>{});
            break;
        case EVMOp::IRemU:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::modulus<uint64_t>{});
            break;
        case EVMOp::FDiv:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::divides<double>{});
            break;

        case EVMOp::IAnd:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::bit_and<int64_t>{});
            break;
        case EVMOp::IOr:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::bit_or<int64_t>{});
            break;
        case EVMOp::IXor:
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::bit_xor<int64_t>{});
            break;
        case EVMOp::IShl: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            auto lhs = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[1]);
            auto rhs = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[2]) & 63;
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(lhs << rhs);
            break;
        }
        case EVMOp::IShrS: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            auto lhs = ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]);
            auto rhs = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[2]) & 63;
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = lhs >> rhs;
            break;
        }
        case EVMOp::IShrU: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            auto lhs = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[1]);
            auto rhs = ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[2]) & 63;
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(lhs >> rhs);
            break;
        }

        case EVMOp::ICmpLTS: // <
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::less<int64_t>{});
            break;
        case EVMOp::ICmpLTU: // <
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::less<uint64_t>{});
            break;
        case EVMOp::FCmpLT: // <
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::less<double>{});
            break;

        case EVMOp::ICmpGTS: // >
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::greater<int64_t>{});
            break;
        case EVMOp::ICmpGTU: // >
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::greater<uint64_t>{});
            break;
        case EVMOp::FCmpGT: // >
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::greater<double>{});
            break;

        case EVMOp::ICmpLES: // <=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::less_equal<int64_t>{});
            break;
        case EVMOp::ICmpLEU: // <=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::less_equal<uint64_t>{});
            break;
        case EVMOp::FCmpLE: // <=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::less_equal<double>{});
            break;

        case EVMOp::ICmpGES: // >=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::greater_equal<int64_t>{});
            break;
        case EVMOp::ICmpGEU: // >=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<uint64_t>(Runtime.Regs, instr, std::greater_equal<uint64_t>{});
            break;
        case EVMOp::FCmpGE: // >=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::greater_equal<double>{});
            break;

        case EVMOp::ICmpEQ: // ==
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::equal_to<int64_t>{});
            break;
        case EVMOp::FCmpEQ: // ==
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::equal_to<double>{});
            break;

        case EVMOp::ICmpNE: // !=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<int64_t>(Runtime.Regs, instr, std::not_equal_to<int64_t>{});
            break;
        case EVMOp::FCmpNE: // !=
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = EvalAlu<double>(Runtime.Regs, instr, std::not_equal_to<double>{});
            break;

        case EVMOp::Cmov:
            // TODO: dont use ReadOperand
        case EVMOp::Mov: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            if (instr.Format != 3) {
                const size_t size = instr.SizeInBytes();
                assert(static_cast<size_t>(instr.Operands[0].Tmp.Idx) + size <= Runtime.Regs.Size());
                CopyOperand(Runtime.Regs.Data() + instr.Operands[0].Tmp.Idx, size, instr.Operands[1], Runtime.Regs);
                break;
            }
            int64_t val = ReadOperand(Runtime.Regs, instr.Operands[1]);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = val;
            break;
        }
        case EVMOp::Bitcast: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = ReadOperand(
                Runtime.Regs,
                instr.Operands[1]);
            break;
        }
        case EVMOp::I2F: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            int64_t ival = ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]);
            double fval = static_cast<double>(ival);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(fval);
            break;
        }
        case EVMOp::F2I: {
            assert(instr.Operands[0].Tmp.Idx >= 0);
            double fval = ReadOperand<double>(Runtime.Regs, instr.Operands[1]);
            int64_t ival = static_cast<int64_t>(fval);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = ival;
            break;
        }

        case EVMOp::INeg128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = -ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]);
            break;
        case EVMOp::IBitNot128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = ~ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]);
            break;
        case EVMOp::INot128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = !ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]);
            break;
        case EVMOp::I2B128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]) != 0;
            break;
        case EVMOp::IAdd128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::plus<__int128_t>{});
            break;
        case EVMOp::ISub128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::minus<__int128_t>{});
            break;
        case EVMOp::IMul128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::multiplies<__int128_t>{});
            break;
        case EVMOp::IDivS128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::divides<__int128_t>{});
            break;
        case EVMOp::IDivU128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<__int128_t>(
                AluU128(Runtime.Regs, instr, std::divides<__uint128_t>{}));
            break;
        case EVMOp::IRemS128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::modulus<__int128_t>{});
            break;
        case EVMOp::IRemU128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<__int128_t>(
                AluU128(Runtime.Regs, instr, std::modulus<__uint128_t>{}));
            break;
        case EVMOp::IAnd128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::bit_and<__int128_t>{});
            break;
        case EVMOp::IOr128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::bit_or<__int128_t>{});
            break;
        case EVMOp::IXor128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::bit_xor<__int128_t>{});
            break;
        case EVMOp::IShl128: {
            auto lhs = std::bit_cast<__uint128_t>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]));
            auto rhs = static_cast<unsigned>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[2])) & 127u;
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<__int128_t>(lhs << rhs);
            break;
        }
        case EVMOp::IShrS128: {
            auto lhs = ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]);
            auto rhs = static_cast<unsigned>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[2])) & 127u;
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = lhs >> rhs;
            break;
        }
        case EVMOp::IShrU128: {
            auto lhs = std::bit_cast<__uint128_t>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]));
            auto rhs = static_cast<unsigned>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[2])) & 127u;
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<__int128_t>(lhs >> rhs);
            break;
        }
        case EVMOp::ICmpLTS128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::less<__int128_t>{});
            break;
        case EVMOp::ICmpLTU128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = AluU128(Runtime.Regs, instr, std::less<__uint128_t>{});
            break;
        case EVMOp::ICmpGTS128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::greater<__int128_t>{});
            break;
        case EVMOp::ICmpGTU128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = AluU128(Runtime.Regs, instr, std::greater<__uint128_t>{});
            break;
        case EVMOp::ICmpLES128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::less_equal<__int128_t>{});
            break;
        case EVMOp::ICmpLEU128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = AluU128(Runtime.Regs, instr, std::less_equal<__uint128_t>{});
            break;
        case EVMOp::ICmpGES128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::greater_equal<__int128_t>{});
            break;
        case EVMOp::ICmpGEU128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = AluU128(Runtime.Regs, instr, std::greater_equal<__uint128_t>{});
            break;
        case EVMOp::ICmpEQ128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::equal_to<__int128_t>{});
            break;
        case EVMOp::ICmpNE128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = Alu128(Runtime.Regs, instr, std::not_equal_to<__int128_t>{});
            break;

        case EVMOp::CmovS128:
        case EVMOp::SExt128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) =
                static_cast<__int128_t>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]));
            break;
        case EVMOp::CmovU128:
        case EVMOp::ZExt128:
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<__int128_t>(
                static_cast<__uint128_t>(ReadOperand<uint64_t>(Runtime.Regs, instr.Operands[1])));
            break;
        case EVMOp::Trunc128:
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) =
                static_cast<int64_t>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]));
            break;
        case EVMOp::I2F128S: {
            double fval = static_cast<double>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]));
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(fval);
            break;
        }
        case EVMOp::I2F128U: {
            double fval = static_cast<double>(
                std::bit_cast<__uint128_t>(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1])));
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = std::bit_cast<int64_t>(fval);
            break;
        }
        case EVMOp::F2I128: {
            double fval = ReadOperand<double>(Runtime.Regs, instr.Operands[1]);
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = static_cast<__int128_t>(fval);
            break;
        }
        case EVMOp::Lde128: {
            void* addr = reinterpret_cast<void*>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[1]));
            __int128_t value = 0;
            std::memcpy(&value, addr, 16);
            Runtime.Regs.Get<__int128_t>(instr.Operands[0].Tmp.Idx) = value;
            break;
        }
        case EVMOp::Ste128: {
            void* addr = reinterpret_cast<void*>(ReadOperand<int64_t>(Runtime.Regs, instr.Operands[0]));
            __int128_t value = ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[1]);
            std::memcpy(addr, &value, 16);
            break;
        }
        case EVMOp::ArgTmp128: {
            Runtime.Args.push_back(0);
            Runtime.Args128.push_back(ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[0]));
            break;
        }

        case EVMOp::Jmp: {
            assert(instr.Operands[0].Type == TVMOperand::EType::Imm);
            frame.PC = reinterpret_cast<TVMInstr*>(instr.Operands[0].Imm.Value);
            break;
        }
        case EVMOp::Cmp: {
            int64_t cmp = ReadOperand(Runtime.Regs, instr.Operands[0]);
            assert(instr.Operands[1].Type == TVMOperand::EType::Imm);
            assert(instr.Operands[2].Type == TVMOperand::EType::Imm);
            int64_t trueLabel = instr.Operands[1].Imm.Value;
            int64_t falseLabel = instr.Operands[2].Imm.Value;
            if (cmp) {
                frame.PC = reinterpret_cast<TVMInstr*>(trueLabel);
            } else {
                frame.PC = reinterpret_cast<TVMInstr*>(falseLabel);
            }
            break;
        }
        case EVMOp::ArgTmp: // TODO: optimize
        case EVMOp::ArgConst: {
            auto value = ReadOperand(Runtime.Regs, instr.Operands[0]);
            Runtime.Args.push_back(value);
            Runtime.Args128.push_back(static_cast<__int128_t>(value));
            break;
        }
        case EVMOp::ECall: {// external call
            auto* func = reinterpret_cast<NFFI::IFunction*>(instr.Operands[1].Imm.Value);
            const int32_t dstTmp = instr.Operands[0].Tmp.Idx;
            auto structDst = materializeStructTmp(frame, dstTmp, nullptr);
            if (structDst) {
                Runtime.Args.insert(Runtime.Args.begin(), *structDst);
            }

            if (dstTmp >= 0) {
                auto ret = (*func)(reinterpret_cast<const uint64_t*>(Runtime.Args.data()), Runtime.Args.size());
                Runtime.Regs.Get<int64_t>(dstTmp) = structDst.value_or(static_cast<int64_t>(ret));
            } else {
                (*func)(reinterpret_cast<const uint64_t*>(Runtime.Args.data()), Runtime.Args.size());
            }
            Runtime.Args.clear();
            Runtime.Args128.clear();

            break;
        }
        case EVMOp::Call: {
            assert(instr.Operands[1].Type == TVMOperand::EType::Imm && "callee must be Imm(id)");
            const int64_t calleeId = instr.Operands[1].Imm.Value;

            assert(calleeId >=0 && calleeId < Module.Functions.size() && "Invalid callee id");
            TFunction* calleeFn = Module.Functions.data() + calleeId;

            if (!calleeFn->Exec) {
                calleeFn->Exec = &Compiler.Compile(*calleeFn);
            }
            auto* calleeExec = calleeFn->Exec;

            const auto& localArgs = calleeFn->ArgLocals;
            const int argCount = (int)Runtime.Args.size();
            assert(argCount <= (int)localArgs.size() && "too many arguments for callee");

            const size_t savedRegsBytes = frame.Exec->RegisterFileSize;
            const size_t oldSize = Runtime.Stack.size();
            Runtime.Stack.resize(oldSize + savedRegsBytes);
            if (savedRegsBytes != 0) {
                std::memcpy(Runtime.Stack.data() + oldSize, Runtime.Regs.Data(), savedRegsBytes);
            }
            auto base = Runtime.Stack.size();

            Runtime.Regs.Reset(calleeExec->RegisterFileSize, calleeExec->RegisterFileAlignment);
            Runtime.Stack.resize(Runtime.Stack.size() + calleeExec->NumLocals, 0); // NumLocals is bytes
            if (Runtime.Stack.size() > MaxStackSize) {
                throw std::runtime_error("Stack overflow in interpreter");
            }

            copyArgsToFrame(Runtime.Stack.data() + base, calleeExec,
                            Runtime.Args.data(), argCount);

            auto link = TReturnLink {
                .CallerDst = instr.Operands[0].Tmp.Idx,
                .CalleeIsCoroutine = calleeFn->IsCoroutine,
                .CalleeReturnsVoid = calleeFn->CoroutineResultTypeId >= 0
                    && Module.Types.IsVoid(calleeFn->CoroutineResultTypeId),
            };

            Runtime.Args.clear();
            Runtime.Args128.clear();
            callStack.emplace_back(TFrame {
                .Exec = calleeExec,
                .StackBase = base,
                .PC = &calleeExec->VMCode[0],
                .Name = calleeFn->Name,
                .ReturnLink = link,
            });
            break;
        }
        case EVMOp::Await: {
            ITypeErasedFuture* future = reinterpret_cast<ITypeErasedFuture*>(ReadOperand(Runtime.Regs, instr.Operands[1]));
            auto value = co_await AwaitTypeErasedFuture<uint64_t>(future);
            Runtime.Regs.Get<int64_t>(instr.Operands[0].Tmp.Idx) = static_cast<int64_t>(value);
            break;
        }
        case EVMOp::AwaitVoid: {
            ITypeErasedFuture* future = reinterpret_cast<ITypeErasedFuture*>(ReadOperand(Runtime.Regs, instr.Operands[0]));
            co_await AwaitTypeErasedFuture<void>(future);
            break;
        }
        case EVMOp::Ret128:
            Runtime.Ret128Value = ReadOperand<__int128_t>(Runtime.Regs, instr.Operands[0]);
            retVal = static_cast<int64_t>(Runtime.Ret128Value);
            retIs128 = true;
            [[fallthrough]];
        case EVMOp::Ret:
            if (!retIs128) {
                retVal = ReadOperand(Runtime.Regs, instr.Operands[0]);
            }
        case EVMOp::RetVoid: {
            auto base = frame.StackBase;
            auto returnLink = callStack.back().ReturnLink;
            callStack.pop_back();
            if (callStack.empty()) {
                break;
            } else {
                auto& callerFrame = callStack.back();
                assert(returnLink);
                auto link = *returnLink;

                std::optional<int64_t> materializedRet;
                if (retVal.has_value()) {
                    materializedRet = materializeStructTmp(
                        callerFrame, link.CallerDst, reinterpret_cast<const void*>(*retVal));
                }

                Runtime.Stack.resize(base);
                // restore saved caller regs
                Runtime.Regs.Reset(callerFrame.Exec->RegisterFileSize, callerFrame.Exec->RegisterFileAlignment);
                const size_t savedRegsBytes = callerFrame.Exec->RegisterFileSize;
                const size_t savedRegsStart = base - savedRegsBytes;
                if (savedRegsBytes != 0) {
                    std::memcpy(Runtime.Regs.Data(), Runtime.Stack.data() + savedRegsStart, savedRegsBytes);
                }
                if (link.CallerDst >= 0 && link.CalleeIsCoroutine) {
                    ITypeErasedFuture* completed = nullptr;
                    if (link.CalleeReturnsVoid) {
                        completed = MakeCompletedVoidFuture();
                    } else {
                        completed = MakeCompletedValueFuture(static_cast<uint64_t>(retVal.value_or(0)));
                    }
                    Runtime.Regs.Get<int64_t>(link.CallerDst) = reinterpret_cast<int64_t>(completed);
                } else if (retIs128 && link.CallerDst >= 0) {
                    Runtime.Regs.Get<__int128_t>(link.CallerDst) = Runtime.Ret128Value;
                } else if (retVal.has_value() && link.CallerDst >= 0) {
                    Runtime.Regs.Get<int64_t>(link.CallerDst) = materializedRet.value_or(*retVal);
                }
                Runtime.Stack.resize(savedRegsStart);
                retVal = std::nullopt;
                retIs128 = false;
            }
            break;
        }
        default:
            std::cerr << "Unknown instruction: '" << (int)instr.Op << "'\n";
            throw std::runtime_error("Unknown instruction");
            break;
        }
    }

    co_return retVal;
}

} // namespace NIR
} // namespace NQumir
