#include "backend_llvm.h"

#include <qumir/codegen/llvm/llvm_codegen.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>

namespace NQumir {

namespace {

class TLlvmCompiledModule : public ICompiledModule {
public:
    TLlvmCompiledModule(
        std::shared_ptr<NCodeGen::TLlvmRunner::TLoadedModule> loaded,
        const NIR::TModule& module,
        const NIR::TFunction& entry)
        : Loaded_(std::move(loaded))
        , Module_(module)
        , Entry_(entry)
    {}

    std::expected<std::optional<std::string>, TError> Run() override {
        try {
            std::string runErr;
            auto res = NCodeGen::TLlvmRunner::RunLoaded(
                *Loaded_,
                &runErr,
                Entry_.ReturnTypeIsString,
                CoroutineResultFormatter());
            if (!runErr.empty()) {
                return std::unexpected(TError({}, std::string("llvm run error: ") + runErr));
            }
            return res;
        } catch (const std::exception& e) {
            // TODO: free resources?
            return std::unexpected(TError(std::string("runtime error: ") + e.what()));
        }
    }

private:
    std::function<std::optional<std::string>(const void*)> CoroutineResultFormatter() const {
        if (!Entry_.IsCoroutine || Entry_.CoroutineResultTypeId < 0
            || Module_.Types.IsVoid(Entry_.CoroutineResultTypeId))
        {
            return {};
        }
        return [&](const void* promisePtr) -> std::optional<std::string> {
            if (!promisePtr) {
                return std::nullopt;
            }
            uint64_t bitRepr = 0;
            std::memcpy(
                &bitRepr,
                promisePtr,
                std::min<int>(sizeof(bitRepr), Module_.Types.SizeInBytes(Entry_.CoroutineResultTypeId)));
            std::ostringstream out;
            Module_.Types.Format(out, bitRepr, Entry_.CoroutineResultTypeId);
            return out.str();
        };
    }

    std::shared_ptr<NCodeGen::TLlvmRunner::TLoadedModule> Loaded_;
    const NIR::TModule& Module_;
    const NIR::TFunction& Entry_;
};

} // namespace

TLlvmBackend::TLlvmBackend(TLlvmBackendOptions options)
    : Options_(std::move(options))
    , Jit_({
        .EnablePerfJitEventListener = Options_.EnablePerfJitEventListener,
    })
{}

std::expected<std::unique_ptr<ICompiledModule>, TError> TLlvmBackend::Compile(NIR::TModule& module) {
    NCodeGen::TLLVMCodeGen cg({
        .NativeCode = Options_.NativeCode,
        .TargetTriple = Options_.TargetTriple,
    });
    std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> artifacts;
    try {
        artifacts = cg.Emit(module, Options_.OptLevel);
    } catch (const std::exception& e) {
        return std::unexpected(TError({}, std::string("llvm codegen error: ") + e.what()));
    }

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

    auto* mainFun = module.GetEntryPoint();
    if (!mainFun) {
        return std::unexpected(TError({}, std::string("entry point not found")));
    }

    try {
        std::string loadErr;
        auto loaded = Jit_.Load(std::move(artifacts), mainFun->Name, &loadErr);
        if (!loaded) {
            return std::unexpected(TError({}, std::string("llvm run error: ") + loadErr));
        }
        return std::make_unique<TLlvmCompiledModule>(std::move(loaded), module, *mainFun);
    } catch (const std::exception& e) {
        return std::unexpected(TError(std::string("runtime error: ") + e.what()));
    }
}

} // namespace NQumir
