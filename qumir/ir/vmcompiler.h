#pragma once

#include <qumir/ir/builder.h>
#include <qumir/ir/vminstr.h>
#include <qumir/ir/ffi.h>
#include <qumir/ir/passes/transforms/allocate_registers.h>
#include <qumir/error.h>

#include <expected>
#include <memory>
#include <unordered_set>
#include <vector>

namespace NQumir {
namespace NIR {

// How a parameter lives in the callee's registers.
enum class EArgKind : uint8_t {
    Word, // the word itself, 8 bytes
    Struct, // the word points to the struct, which the callee copies into its frame
    Vector, // the bytes of the vector
    Int128, // the 16 bytes
};

// Parameters sit at the start of the callee's register file, so a caller
// writes its arguments straight into the callee's registers.
struct TParamLayout {
    std::vector<int> TypeIds; // IR typeId of each parameter (Invoke checks entry arguments)
    std::vector<int> Sizes;
    std::vector<EArgKind> Kinds;
    std::vector<int> Offsets; // register byte offset of each parameter
    int Bytes = 0; // end of the last parameter
    int Alignment = 8;
};

struct TExecFunc {
    int UniqueId;
    std::vector<TVMInstr> VMCode;
    int32_t RegisterFileSize = 0;
    int32_t RegisterFileAlignment = 8;
    int32_t NumLocals{0};        // frame size in bytes (not variable count)
    TParamLayout Params;
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

    // Compiles the roots, their callees and the module constructor/destructor, all or nothing.
    std::expected<void, TError> Prepare(const std::vector<TFunction*>& roots);
    bool IsPrepared(const TExecFunc* exec) const {
        return Prepared.contains(exec);
    }

private:
    void CompileUltraLow(const TFunction& function, NPasses::TRegisterAllocation& allocation, TExecFunc& out);
    bool CompileVectorInstruction(const TFunction& function, const TInstr& instr, TVMInstr& out);
    void BindRegisters(
        const TFunction& function,
        const std::vector<int>& tmpFrameOffsets,
        NPasses::TRegisterAllocation& allocation,
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
