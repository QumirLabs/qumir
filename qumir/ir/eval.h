#pragma once

#include "builder.h"
#include "vmcompiler.h"
#include "register_file.h"

#include <coroutine>
#include <cstdint>
#include <expected>
#include <ostream>
#include <vector>

#include <qumir/error.h>
#include <qumir/future.h>

namespace NQumir {
namespace NIR {

// Link to caller frame for returning
struct TReturnLink {
    int32_t CallerDst; // destination register byte offset in caller, -1 if none
    bool CalleeIsCoroutine = false;
    bool CalleeReturnsVoid = false;
};

struct TFrame {
    const TExecFunc* Exec{nullptr};
    const uint64_t StackBase = 0;
    TVMInstr* PC{nullptr};
    std::string_view Name;
    std::optional<TReturnLink> ReturnLink;
};

struct TRuntime {
    std::vector<char> Globals; // byte array; each variable slot is 8 bytes (64-bit aligned)
    std::vector<char> Stack;   // byte array; each variable slot is 8 bytes (64-bit aligned)
    std::vector<int64_t> Args; // call arguments, will be copied on stack on call, TODO: remove
    std::vector<__int128_t> Args128; // parallel to Args, filled for 128-bit arguments only
    std::vector<char> ArgBytes; // vector argument bytes; Args holds their offsets
    std::vector<char> RetBytes; // returned vector bytes
    __int128_t Ret128Value = 0;
    TRegisterFile Regs;
    std::vector<TFrame> CallStack;
};

struct TExecFunc;

struct IDebugger {
    virtual ~IDebugger() = default;
    virtual void SetRuntime(const TRuntime& runtime) = 0;
    virtual void SetModule(const TModule& module) = 0;
    virtual void OnFunctionCompilationFinished(const TFunction& function) = 0;
    virtual void OnModuleCompilationFinished() = 0;
    virtual void OnInstruction(const TFrame& frame) = 0;
};

class TInterpreter {
public:
    TInterpreter(
        TModule& module,
        TVMCompiler& compiler,
        std::ostream& out,
        std::istream& in,
        IDebugger* debugger = nullptr);

    struct TOptions {
        bool PrintByteCode = false;
    };

    TRuntime& GetRuntime() { return Runtime; }

    std::optional<std::string> Eval(TFunction& function, std::vector<int64_t> args, TOptions options);
    // Returns the VM register bits directly. This is intended for scalar POD
    // results whose caller already knows the function's return type.
    std::optional<int64_t> EvalRaw(TFunction& function, std::vector<int64_t> args, TOptions options);

    // Calls a function accepted by TVMCompiler::Prepare; never compiles, never pumps async
    // events. Needs Initialize(). Rejects non-struct arguments wider than 64 bits.
    // One interpreter per thread; Prepare must not run concurrently with Invoke.
    std::expected<int64_t, TError> Invoke(TFunction& function, std::vector<int64_t> args);
    // Module constructor/destructor, once per instance; Finalize is idempotent.
    std::expected<void, TError> Initialize();
    std::expected<void, TError> Finalize();

private:
    std::expected<int64_t, TError> InvokePrepared(TFunction& function, std::vector<int64_t> args);
    std::optional<int64_t> DoEvalRaw(TFunction& function, std::vector<int64_t> args, TOptions options);

    template<bool EnableDebug = false>
    TFuture<std::optional<int64_t>> DoEvalRawAsync(TFunction& function, std::vector<int64_t> args, TOptions options);
    size_t ProcessAsyncRuntimeEvents();

    std::ostream& Out;
    std::istream& In;
    TModule& Module;
    TRuntime Runtime;
    TVMCompiler& Compiler;
    IDebugger* Debugger{nullptr};
    bool Initialized{false};
};

} // namespace NIR
} // namespace NQumir
