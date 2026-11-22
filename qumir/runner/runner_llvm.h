#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/modules/module.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/frontend.h>

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/lower_ast.h>

#include <qumir/codegen/backend.h>
#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_runner.h>

#include <expected>
#include <istream>
#include <optional>
#include <unordered_set>
#include <unordered_map>

namespace NQumir {

struct TLLVMRunnerOptions {
    bool PrintAst = false;
    bool PrintTransformedAst = false;
    bool PrintIr = false;
    bool PrintLlvm = false;
    bool PrintAsm = false;
    bool NativeCode = false;
    bool CoreInput = false;
    bool ResolveCoreInput = true;
    bool AllowOverloads = false; // enable function overloads / generics (pragma language overloads)
    bool EnablePerfJitEventListener = false;
    bool RunDefiniteAssignment = true;
    int OptLevel = 0; // 0-3
    // Core frontend host prelude: modules imported on behalf of the host.
    // Empty means pure core-lang imports nothing. Ignored for the Kumir
    // frontend, which imports its own prelude.
    std::vector<std::string> Prelude;
    // Directories searched for `.oz` source modules referenced by `use`.
    std::vector<std::string> ModuleSearchPaths;
    // Explicitly registered `.oz` module files (bound by file stem).
    std::vector<std::string> ModuleFiles;
    // Target triple override (e.g. "wasm32-unknown-unknown"). Empty means the host default.
    std::string TargetTriple;
    std::string SourceFilePath;
    bool EmitDebugInfo = false;
};

// A single compilation session: holds persistent frontend state (Module,
// Resolver, Builder) that accumulates across calls, so it is not thread-safe.
// Use a fresh runner per independent compilation (e.g. one per query).
// Run() is the shared frontend, the codegen backend and the ORC JIT linker.
class TLLVMRunner : public NFrontend::TFrontend {
public:
    struct TCachedObjectModule {
        std::vector<std::string> ObjectFiles;
        std::vector<std::string> ObjectBlobs;
        std::string KernelObject;
    };

    // The host picks the codegen backend of Run() and of the kernel APIs; null
    // means TLLVMBackend. The cache kernel APIs need TLLVMBackend.
    // `types` is the host's factory, which must outlive the runner; null: the runner owns one.
    TLLVMRunner(
        TLLVMRunnerOptions options = {},
        std::unique_ptr<NCodeGen::ICodegenBackend> backend = nullptr,
        NAst::TTypeFactory* types = nullptr);

    void RegisterModule(std::shared_ptr<NRegistry::IModule> module, bool import = false);

    // Parses, resolves, lowers to IR and executes via LLVM JIT.
    // Returns numeric result (if any) produced by the compiled chunk.
    std::expected<std::optional<std::string>, TError> Run(std::istream& input);

    // Compiles a core-lang kernel source string and returns a JIT function pointer.
    // The pointer is valid for the lifetime of this TLLVMRunner.
    void* CompileKernel(const std::string& source, std::string* error = nullptr);
    // Selects the JIT entry point by source function name. This keeps the entry
    // stable when generic specializations are appended during type annotation.
    void* CompileKernelAst(
        const NAst::TExprPtr& ast, const std::string& entryName, std::string* error);
    // Compiles an AST already processed by LoadAndCompose, including any LLVM
    // bitcode carried by its source modules; another backend rejects such bitcode.
    void* CompileKernelAst(
        NFrontend::TComposeResult composed,
        const std::string& entryName,
        std::string* error);
    std::unordered_map<std::string, void*> CompileKernelAst(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        std::string* error);
    // A backend other than TLLVMBackend compiles all entries into one object,
    // which cannot link the LLVM bitcode of source modules.
    std::unordered_map<std::string, void*> CompileKernelAst(
        NFrontend::TComposeResult composed,
        const std::vector<std::string>& entryNames,
        std::string* error);

    // Same pipeline as CompileKernelAst, but emits a target object file (per
    // Options.TargetTriple) instead of JIT-compiling. Returns object bytes.
    // Another backend emits, for the host, the object that CompileKernelAst links.
    std::optional<std::string> CompileKernelAstToObject(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        std::string* error);
    std::optional<std::string> CompileKernelAstToObject(
        NFrontend::TComposeResult composed,
        const std::vector<std::string>& entryNames,
        std::string* error);

    // Compiles `ast` using the symbol-granular object cache in `cacheDir`.
    // Cacheable dependency symbols are loaded from cache when present and
    // compiled+persisted when missing; the query-specific kernel is always
    // recompiled and links against the deps by name. Returns the entry pointers
    // with a lifetime handle that scopes their JIT to the caller.
    NCodeGen::TLlvmRunner::TLinkedModule CompileFusedKernelsCached(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        const std::string& cacheDir,
        const std::string& cacheSchema,
        const std::string& kernelLibVersion,
        std::string* error);

    // Object-emitting counterpart of CompileFusedKernelsCached. Cache hits are
    // returned as paths, freshly compiled dependencies as object blobs, and the
    // query-specific kernel as a separate object. The caller owns final linking.
    std::optional<TCachedObjectModule> CompileFusedKernelsToObjectsCached(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        const std::string& cacheDir,
        const std::string& cacheSchema,
        const std::string& kernelLibVersion,
        std::string* error);

private:
    struct TPreparedCachedCompilation {
        std::vector<std::string> ObjectFiles;
        std::vector<std::string> ObjectBlobs;
        std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> KernelModule;
        size_t RequiredCount = 0;
        size_t HitCount = 0;
        size_t MissCount = 0;
        size_t ImportedCount = 0;
        double FrontendMs = 0;
        double ResolveMs = 0;
        double DependenciesMs = 0;
        double KernelMs = 0;
    };

    std::optional<TPreparedCachedCompilation> PrepareFusedKernelsCached(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        const std::string& cacheDir,
        const std::string& cacheSchema,
        const std::string& kernelLibVersion,
        std::string* error);

    // Frontend: compose, resolve, transform and lower `ast` into Module, then
    // check every entry exists. Returns false on error.
    bool LowerKernelAst(
        NAst::TExprPtr ast,
        const std::vector<std::string>& entryNames,
        std::string* error,
        std::vector<NAst::TPragma> mainPragmas = {});

    // Codegen the already-lowered Module, optionally partitioned (see
    // TLLVMCodeGenOptions). Returns nullptr on error.
    std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> EmitLoweredModule(
        const std::unordered_set<std::string>* restrictToDefinitions,
        const std::unordered_set<std::string>* emitAsExternal,
        std::string* error,
        const std::vector<std::string>* llvmBitcode = nullptr,
        const std::unordered_set<std::string>* inlineDefinitions = nullptr,
        const std::unordered_set<std::string>* exports = nullptr);

    // True unless Backend_ is TLLVMBackend, whose kernels keep the partitioned
    // LLVM codegen (entry exports, bitcode linking, the object cache).
    bool KernelsUseBackend() const;
    // Prints the IR chunks not printed yet when Options.PrintIr is set.
    void PrintModuleIr();
    // Lowers `ast` and emits Module with Backend_ into one object.
    std::optional<std::string> EmitBackendKernels(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        std::string* error,
        std::vector<NAst::TPragma> mainPragmas = {},
        const std::vector<std::string>& llvmBitcode = {});
    // Links an object of EmitBackendKernels with ORC JITLink.
    std::unordered_map<std::string, void*> LinkBackendKernels(
        std::string object,
        const std::vector<std::string>& entryNames,
        std::string* error);

    // LLVM-backend frontend of the CompileKernelAst overloads: lower then emit.
    std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> EmitKernelArtifacts(
        const NAst::TExprPtr& ast,
        const std::vector<std::string>& entryNames,
        std::string* error,
        std::vector<NAst::TPragma> mainPragmas = {},
        const std::vector<std::string>* llvmBitcode = nullptr);

    TLLVMRunnerOptions Options;

    std::unordered_set<std::string> RegisteredModuleNames;

    std::vector<std::shared_ptr<NRegistry::IModule>> RegisteredModules;
    std::vector<std::shared_ptr<NRegistry::IModule>> AvailableModules;

    std::unique_ptr<NCodeGen::ICodegenBackend> Backend_; // Run(), and the kernel APIs unless TLLVMBackend
    NCodeGen::TLlvmRunner LlvmRunner_; // persistent; keeps compiled kernels alive
    std::vector<std::shared_ptr<void>> BackendKernels_; // JITs of LinkBackendKernels
};

} // namespace NQumir
