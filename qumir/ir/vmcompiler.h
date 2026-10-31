#pragma once

#include <qumir/ir/builder.h>
#include <qumir/ir/vminstr.h>
#include <qumir/ir/ffi.h>
#include <qumir/error.h>

#include <expected>
#include <memory>
#include <unordered_set>
#include <vector>

namespace NQumir {
namespace NIR {

struct TExecFunc {
    int UniqueId;
    std::vector<TVMInstr> VMCode;
    int32_t RegisterFileSize = 0;
    int32_t RegisterFileAlignment = 8;
    int32_t NumLocals{0};        // frame size in bytes (not variable count)
    std::vector<int> ArgByteOffsets; // byte offset of each argument local in the frame (for func args only!)
    std::vector<int> ArgTypeIds;     // IR typeId of each argument (eval uses SizeInBytes to handle struct)
    std::unordered_map<int, int> TmpByteOffsets; // IR tmp id -> register file byte offset
    struct TStructRegister {
        int FrameOffset;
        int Size;
    };
    std::unordered_map<int, TStructRegister> StructRegisters; // register byte offset -> frame storage

    // debug
    std::vector<int> LocalByteOffsets; // byte offset of each local in the frame (for debug only)
    std::vector<TInstrDebugInfo> InstrDebugInfo;
};

class TVMCompiler {
public:
    TVMCompiler(TModule& module)
        : Module(module)
    {}

    TExecFunc& Compile(TFunction& function, bool printByteCode = false);

    // Compiles the roots, everything they call and the module constructor/destructor.
    // After success TInterpreter::Invoke needs no compilation and writes no shared data.
    std::expected<void, TError> Prepare(const std::vector<TFunction*>& roots);
    bool IsPrepared(const TExecFunc* exec) const { return Prepared.contains(exec); }

private:
    void CompileUltraLow(const TFunction& function, TExecFunc& out);
    bool CompileVectorInstruction(const TFunction& function, const TInstr& instr, TVMInstr& out);
    void AllocateRegisters(
        const TFunction& function,
        const std::vector<int>& tmpFrameOffsets,
        TExecFunc& out);

    // nullptr if the symbol is missing or the signature is unsupported.
    NFFI::IFunction* GetOrCreateExternalThunk(int externIdx);

    TModule& Module;
    std::unordered_map<int, TExecFunc> CodeCache;
    std::vector<std::unique_ptr<NFFI::IFunction>> ExternalThunks;
    std::unordered_map<int, NFFI::IFunction*> ExternalThunkCache;
    std::unordered_set<const TExecFunc*> Prepared;
};

} // namespace NIR
} // namespace NQumir
