#include "llvm_backend.h"
#include "llvm_codegen_impl.h"

#include <llvm/IR/Function.h>

#include <exception>
#include <iostream>
#include <sstream>

namespace NQumir {
namespace NCodeGen {

TLLVMBackend::TLLVMBackend(TLLVMBackendOptions options)
    : Options_(std::move(options))
{}

std::expected<std::string, TError> TLLVMBackend::EmitObject(NIR::TModule& module, EObjectUse use) {
    try {
        TLLVMCodeGen cg({
            .NativeCode = Options_.NativeCode,
            .TargetTriple = Options_.TargetTriple,
        });
        auto artifacts = cg.Emit(module, Options_.OptLevel);

        if (Options_.PrintLlvm) {
            std::cerr << "=========== LLVM: ==========\n";
            for (const auto& function : module.Functions) {
                if (PrintedFunctions_.insert(function.UniqueId).second) {
                    cg.PrintFunction(function.SymId, std::cerr);
                }
            }
            std::cerr << "============================\n\n";
        }

        if (Options_.PrintAsm) {
            std::cerr << "=========== ASM: ===========\n";
            artifacts->Generate(std::cerr, /*generateAsm=*/true, /*generateObj=*/false);
            std::cerr << "============================\n\n";
        }

        if (use == EObjectUse::Load) {
            // They stay in llvm.global_ctors and llvm.global_dtors: the JIT runs no init lists.
            auto& llvmModule = *static_cast<TLLVMModuleArtifacts&>(*artifacts).Module;
            for (const char* name : {"$$module_constructor", "$$module_destructor"}) {
                if (auto* function = llvmModule.getFunction(name)) {
                    function->setLinkage(llvm::Function::ExternalLinkage);
                }
            }
        }

        std::ostringstream object(std::ios::binary);
        artifacts->Generate(object, /*generateAsm=*/false, /*generateObj=*/true);
        return object.str();
    } catch (const std::exception& e) {
        return std::unexpected(TError({}, std::string("llvm codegen error: ") + e.what()));
    }
}

std::expected<std::string, TError> TLLVMBackend::EmitAsm(NIR::TModule& module) {
    try {
        TLLVMCodeGen cg({
            .NativeCode = Options_.NativeCode,
            .TargetTriple = Options_.TargetTriple,
        });
        auto artifacts = cg.Emit(module, Options_.OptLevel);
        std::ostringstream text;
        artifacts->Generate(text, /*generateAsm=*/true, /*generateObj=*/false);
        return text.str();
    } catch (const std::exception& e) {
        return std::unexpected(TError({}, std::string("llvm codegen error: ") + e.what()));
    }
}

} // namespace NCodeGen
} // namespace NQumir
