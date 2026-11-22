#pragma once

#include <qumir/modules/module.h>

namespace NQumir {
namespace NRegistry {

// Byte-level primitives every host needs, named with a `builtin::` prefix so
// LLVM codegen can recognize them by name (qumir/codegen/llvm/llvm_codegen.cpp)
// and lower memcpy/memmove to their LLVM intrinsic instead of a declared
// external call. Always registered and imported by every runner (TLLVMRunner,
// TIRRunner), independent of CoreInput/Prelude, so `builtin::*` resolves
// without a `use` statement anywhere.
class BuiltinsModule : public IModule {
public:
    static constexpr const char* ModuleName = "Builtins";

    explicit BuiltinsModule(NAst::TTypeFactory& types);

    const std::string& Name() const override {
        static const std::string name = ModuleName;
        return name;
    }

    const std::vector<TExternalFunction>& ExternalFunctions() const override {
        return ExternalFunctions_;
    }

    const std::vector<TExternalType>& ExternalTypes() const override {
        return ExternalTypes_;
    }

    const std::vector<TLiteralSuffix>& LiteralSuffixes() const override {
        return LiteralSuffixes_;
    }

    const std::vector<std::string>& Dependencies() const override {
        return Dependencies_;
    }

    bool IsImplicit() const override {
        return true;
    }

private:
    std::vector<TExternalFunction> ExternalFunctions_;
    std::vector<TExternalType> ExternalTypes_;
    std::vector<TLiteralSuffix> LiteralSuffixes_;
    std::vector<std::string> Dependencies_ = {};
};

} // namespace NRegistry
} // namespace NQumir
