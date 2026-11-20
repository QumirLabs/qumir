#pragma once

#include <qumir/runner/backend.h>
#include <qumir/codegen/llvm/llvm_runner.h>

#include <string>
#include <unordered_set>

namespace NQumir {

struct TLlvmBackendOptions {
    bool PrintLlvm = false;
    bool PrintAsm = false;
    bool NativeCode = false;
    bool EnablePerfJitEventListener = false;
    int OptLevel = 0; // 0-3
    // Target triple override (e.g. "wasm32-unknown-unknown"). Empty means the host default.
    std::string TargetTriple;
};

// Compiles with LLVM and loads the code with the ORC JIT.
class TLlvmBackend : public ICodegenBackend {
public:
    explicit TLlvmBackend(TLlvmBackendOptions options = {});

    std::expected<std::unique_ptr<ICompiledModule>, TError> Compile(NIR::TModule& module) override;

private:
    TLlvmBackendOptions Options_;
    std::unordered_set<int> PrintedFunctions_;
    NCodeGen::TLlvmRunner Jit_;
};

} // namespace NQumir
