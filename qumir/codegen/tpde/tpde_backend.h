#pragma once

#include <qumir/codegen/backend.h>

namespace NQumir {
namespace NCodeGen {

// TPDE (https://github.com/tpde2/tpde) compiles Qumir IR straight to an
// AArch64 ELF object, without LLVM. JIT objects only (EObjectUse::Load).
class TTpdeBackend : public ICodegenBackend {
public:
    std::expected<std::string, TError> EmitObject(NIR::TModule& module, EObjectUse use) override;
    std::expected<std::string, TError> EmitAsm(NIR::TModule& module) override;
};

} // namespace NCodeGen
} // namespace NQumir
