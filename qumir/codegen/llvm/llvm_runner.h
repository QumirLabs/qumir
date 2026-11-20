#pragma once

#include "llvm_codegen.h"
#include "symbol_object_cache.h"

#include <istream>
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace NQumir::NCodeGen {

struct TLlvmRunnerOptions {
    bool EnablePerfJitEventListener = false;
};

// Runner: lowers code to NIR, translates to LLVM IR, returns full module IR text.
class TLlvmRunner {
public:
    TLlvmRunner(TLlvmRunnerOptions options = {});

    // No IR generation API here; only execution of already generated module.

    // Does not modify internal Module; purely consumes the artifacts.
    std::optional<std::string> Run(
        std::unique_ptr<ILLVMModuleArtifacts> artifacts,
        const std::string& entryPoint,
        std::string* error = nullptr,
        bool returnTypeIsString = false /* TODO: remove me, clutch: support string returnType */,
        std::function<std::optional<std::string>(const void*)> coroutineResultFormatter = {});

    // Run() in two steps: Load() JIT-compiles the module and resolves the entry,
    // RunLoaded() only executes code. The handle keeps the JIT alive.
    struct TLoadedModule;
    std::shared_ptr<TLoadedModule> Load(
        std::unique_ptr<ILLVMModuleArtifacts> artifacts,
        const std::string& entryPoint,
        std::string* error = nullptr);

    static std::optional<std::string> RunLoaded(
        TLoadedModule& module,
        std::string* error = nullptr,
        bool returnTypeIsString = false,
        std::function<std::optional<std::string>(const void*)> coroutineResultFormatter = {});

    // Compiles the module via JIT and returns a function pointer by name.
    // The pointer is valid for the lifetime of this TLlvmRunner.
    void* Lookup(
        std::unique_ptr<ILLVMModuleArtifacts> artifacts,
        const std::string& name,
        std::string* error = nullptr);

    std::unordered_map<std::string, void*> LookupMany(
        std::unique_ptr<ILLVMModuleArtifacts> artifacts,
        const std::vector<std::string>& names,
        std::string* error = nullptr);

    // Entry pointers plus an opaque handle that keeps their JIT alive. The
    // pointers are valid exactly as long as Lifetime is held, so the caller can
    // scope a query's compiled code to the query (not to the whole runner).
    struct TLinkedModule {
        std::unordered_map<std::string, void*> Entries;
        std::shared_ptr<void> Lifetime;
    };

    // Links prebuilt objects (from files and/or in-memory blobs) and an optional
    // IR module into one JIT, then looks up `names`. Objects are added first so
    // the module can reference their symbols; each symbol must be defined once.
    // On failure returns an empty Entries map (and sets *error).
    TLinkedModule LinkAndLookup(
        const std::vector<std::string>& objectPaths,
        const std::vector<std::string>& objectBlobs,
        std::unique_ptr<ILLVMModuleArtifacts> kernelModule,
        bool nativeCode,
        const std::vector<std::string>& names,
        std::string* error = nullptr);

private:
    TLlvmRunnerOptions Options_;
    std::string LastError; // currently unused (kept for future diagnostics)

    // Keeps JIT engines alive so function pointers returned by Lookup remain valid.
    // Type-erased to avoid including heavy LLVM headers here.
    std::vector<std::shared_ptr<void>> LiveEngines_;
};

// Builds a fingerprint from the current LLVM/target settings plus the caller's
// schema and dep-library versions.
TBuildFingerprint MakeBuildFingerprint(
    bool nativeCode,
    const std::string& targetTriple,
    int optLevel,
    std::string cacheSchema,
    std::string kernelLibVersion);

} // namespace NQumir::NCodeGen
