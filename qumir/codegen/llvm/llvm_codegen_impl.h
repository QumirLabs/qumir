#pragma once

#include "llvm_codegen.h"

#include <llvm/IR/Module.h>

namespace NQumir::NCodeGen {

struct TLLVMModuleArtifacts : ILLVMModuleArtifacts {
    std::unique_ptr<llvm::LLVMContext> Ctx;
    std::unique_ptr<llvm::Module> Module;
    std::vector<std::string> FunctionNames; // defined (non-declaration) function names in module
    bool NativeCode = false;
    bool DebugPoints = false;
    const std::vector<std::string>& GetDefinedFunctionNames() const override { return FunctionNames; }
    void PrintModule(std::ostream& os) const override;
    void GenerateBitcode(std::ostream& os) const override;
    void Generate(std::ostream& os, bool generateAsm, bool generateObj) const override;
};

} // namespace NQumir::NCodeGen
