#pragma once

#include <qumir/codegen/backend.h>

#include <string>
#include <unordered_set>

namespace NQumir {
namespace NCodeGen {

struct TLLVMBackendOptions {
    // Target triple override (e.g. "wasm32-unknown-unknown"). Empty means the host.
    std::string TargetTriple;
    bool NativeCode = false;
    int OptLevel = 0; // 0-3
    // EmitObject() prints the LLVM IR and the assembly to stderr (qumiri).
    bool PrintLlvm = false;
    bool PrintAsm = false;
};

// TLLVMCodeGen as a codegen backend.
class TLLVMBackend : public ICodegenBackend {
public:
    explicit TLLVMBackend(TLLVMBackendOptions options);

    std::expected<std::string, TError> EmitObject(NIR::TModule& module, EObjectUse use) override;
    std::expected<std::string, TError> EmitAsm(NIR::TModule& module) override;

private:
    TLLVMBackendOptions Options_;
    // Functions whose LLVM IR is printed: a REPL session prints each one once.
    std::unordered_set<int> PrintedFunctions_;
};

} // namespace NCodeGen
} // namespace NQumir
