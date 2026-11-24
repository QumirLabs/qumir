#include "runner_llvm.h"

#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/modules/system/system.h>
#include <qumir/modules/builtins/builtins.h>
#include <qumir/modules/turtle/turtle.h>
#include <qumir/modules/robot/robot.h>
#include <qumir/modules/drawer/drawer.h>
#include <qumir/modules/painter/painter.h>
#include <qumir/modules/colors/colors.h>
#include <qumir/modules/keyboard/keyboard.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>
#include <qumir/codegen/llvm/llvm_backend.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <cassert>
#include <sstream>

namespace NQumir {

using namespace NIR;

namespace {

bool IsKnown32BitTarget(const std::string& targetTriple) {
    return targetTriple.starts_with("wasm32-");
}

NFrontend::TFrontendOptions FrontendOptions(const TLLVMRunnerOptions& options) {
    return {
        .PrintAst = options.PrintAst,
        .PrintTransformedAst = options.PrintTransformedAst,
        .PrintIr = options.PrintIr,
        .CoreInput = options.CoreInput,
        .ResolveCoreInput = options.ResolveCoreInput,
        .RunDefiniteAssignment = options.RunDefiniteAssignment,
        .OptLevel = options.OptLevel,
        .ModuleSearchPaths = options.ModuleSearchPaths,
        .ModuleFiles = options.ModuleFiles,
        .SourceFilePath = options.SourceFilePath,
        .DebugOptions = {.EmitDebugInfo = options.EmitDebugInfo && options.OptLevel == 0},
    };
}

} // namespace

TLLVMRunner::TLLVMRunner(
    TLLVMRunnerOptions options,
    std::unique_ptr<NCodeGen::ICodegenBackend> backend,
    NAst::TTypeFactory* types)
    : TFrontend(FrontendOptions(options), types)
    , Options(std::move(options))
    // Run() loads the object into this process, so the default targets the
    // host whatever TargetTriple says; TargetTriple is for the kernel objects.
    , Backend_(backend ? std::move(backend) : std::make_unique<NCodeGen::TLLVMBackend>(NCodeGen::TLLVMBackendOptions{
        .NativeCode = Options.NativeCode,
        .OptLevel = Options.OptLevel,
        .PrintLlvm = Options.PrintLlvm,
        .PrintAsm = Options.PrintAsm,
    }))
    , LlvmRunner_({
        .EnablePerfJitEventListener = Options.EnablePerfJitEventListener,
    })
{
    if (IsKnown32BitTarget(Options.TargetTriple)) {
        Module.Types.SetPointerSize(4);
    }
    if (Options.AllowOverloads) {
        Resolver.ApplyPragmas({NAst::TPragma{"language", {"overloads"}, {}}});
    }

    RegisterModule(std::make_shared<NRegistry::SystemModule>(Types), /*import=*/false);
    // Byte primitives (memcpy/memmove/memcmp) every host needs; always
    // imported, independent of CoreInput/Prelude (qumir/modules/builtins).
    RegisterModule(std::make_shared<NRegistry::BuiltinsModule>(Types), /*import=*/true);
    // TODO: register other modules

    AvailableModules.push_back(std::make_shared<NRegistry::TurtleModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::RobotModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::DrawerModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::PainterModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::ColorsModule>(Types));
    AvailableModules.push_back(std::make_shared<NRegistry::KeyboardModule>(Types));

    for (const auto& mod : AvailableModules) {
        Resolver.RegisterModule(mod.get());
    }

    if (!Options.CoreInput) {
        // Kumir prelude: standard runtime modules plus legacy module aliases.
        for (const auto& mod : RegisteredModules) {
            (void)Resolver.ImportModule(mod->Name());
        }
        for (const auto& [alias, canonical] : NSemantics::NKumir::ModuleAliases()) {
            Resolver.RegisterModuleAlias(alias, canonical);
        }
    } else {
        // Core has no implicit prelude; the host imports what it needs.
        for (const auto& name : Options.Prelude) {
            (void)Resolver.ImportModule(name);
        }
    }
}

void TLLVMRunner::RegisterModule(std::shared_ptr<NRegistry::IModule> module, bool import) {
    if (!module) {
        return;
    }
    auto name = module->Name();
    if (RegisteredModuleNames.count(name)) {
        if (import) {
            (void)Resolver.ImportModule(name);
        }
        return;
    }
    Resolver.RegisterModule(module.get());
    RegisteredModuleNames.insert(name);
    RegisteredModules.push_back(std::move(module));
    if (import) {
        (void)Resolver.ImportModule(name);
    }
}

std::expected<std::optional<std::string>, TError> TLLVMRunner::Run(std::istream& input) {
    if (auto compiled = Compile(input); !compiled) {
        return std::unexpected(compiled.error());
    }

    auto object = Backend_->EmitObject(Module, NCodeGen::EObjectUse::Load);
    if (!object) {
        return std::unexpected(object.error());
    }

    auto* mainFun = Module.GetEntryPoint();
    if (!mainFun) {
        return std::unexpected(TError({}, std::string("entry point not found")));
    }

    // Link the object into this process with the ORC JIT and run it
    NCodeGen::TLlvmRunner runner({
        .EnablePerfJitEventListener = Options.EnablePerfJitEventListener,
    });
    try {
        std::string runErr;
        auto coroutineResultFormatter = [&]() -> std::function<std::optional<std::string>(const void*)> {
            if (!mainFun->IsCoroutine || mainFun->CoroutineResultTypeId < 0
                || Module.Types.IsVoid(mainFun->CoroutineResultTypeId)) {
                return {};
            }
            return [&](const void* promisePtr) -> std::optional<std::string> {
                if (!promisePtr) {
                    return std::nullopt;
                }
                uint64_t bitRepr = 0;
                std::memcpy(&bitRepr, promisePtr,
                    std::min<int>(sizeof(bitRepr), Module.Types.SizeInBytes(mainFun->CoroutineResultTypeId)));
                std::ostringstream out;
                Module.Types.Format(out, bitRepr, mainFun->CoroutineResultTypeId);
                return out.str();
            };
        }();
        auto res = runner.RunObject(
            *object,
            Module,
            *mainFun,
            &runErr,
            std::move(coroutineResultFormatter));
        if (!runErr.empty()) {
            return std::unexpected(TError({}, std::string("llvm run error: ") + runErr));
        }
        return res;
    } catch (const std::exception& e) {
        // TODO: free resources?
        return std::unexpected(TError(std::string("runtime error: ") + e.what()));
    }
}

void* TLLVMRunner::CompileKernelAst(
    const NAst::TExprPtr& ast, const std::string& entryName, std::string* error)
{
    auto entries = CompileKernelAst(ast, std::vector<std::string>{entryName}, error);
    auto it = entries.find(entryName);
    return it == entries.end() ? nullptr : it->second;
}

void* TLLVMRunner::CompileKernelAst(
    NFrontend::TComposeResult composed,
    const std::string& entryName,
    std::string* error)
{
    auto entries = CompileKernelAst(
        std::move(composed), std::vector<std::string>{entryName}, error);
    auto it = entries.find(entryName);
    return it == entries.end() ? nullptr : it->second;
}

bool TLLVMRunner::LowerKernelAst(
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error,
    std::vector<NAst::TPragma> mainPragmas)
{
    if (error) {
        error->clear();
    }
    if (Options.AllowOverloads) {
        auto hasOverloads = std::any_of(
            mainPragmas.begin(), mainPragmas.end(),
            [](const auto& pragma) {
                return pragma.Group == "language"
                    && std::find(pragma.Values.begin(), pragma.Values.end(), "overloads")
                        != pragma.Values.end();
            });
        if (!hasOverloads) {
            mainPragmas.push_back(NAst::TPragma{"language", {"overloads"}, {}});
        }
    }

    NFrontend::TSourceModuleLoader loader(Types);
    for (const auto& dir : Options.ModuleSearchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : Options.ModuleFiles) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            if (error) {
                *error = reg.error().ToString();
            }
            return false;
        }
    }
    {
        auto composed = NFrontend::LoadAndCompose(loader, ast, mainPragmas);
        if (!composed) {
            if (error) {
                *error = composed.error().ToString();
            }
            return false;
        }
        ast = std::move(composed->Ast);
        Resolver.ApplyPragmas(composed->Pragmas);
    }
    // Kernels keep only the module functions their entries reach: the rest skip the semantics.
    NFrontend::DropUnreachableModuleFunctions(ast, entryNames);

    // Contract: an entrypoint is looked up by its source name, so its emit symbol
    // must equal that name - never mangle/cache it, whatever flags it carries.
    std::unordered_set<std::string> entrySet(entryNames.begin(), entryNames.end());
    if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(ast)) {
        for (auto& stmt : block.Cast()->Stmts) {
            if (auto fd = NAst::TMaybeNode<NAst::TFunDecl>(stmt);
                fd && entrySet.count(fd.Cast()->Name)) {
                fd.Cast()->Cacheable = false;
            }
        }
    }

    auto scope = Resolver.GetOrCreateRootScope();
    // scope->AllowsRedeclare = true;
    scope->RootLevel = false;

    NTransform::TPipelineOptions pipelineOptions{
        .RunDefiniteAssignment = Options.RunDefiniteAssignment,
    };
    if (!Options.CoreInput) {
        pipelineOptions.Extensions = NSemantics::NKumir::PipelineExtensions();
    }
    auto transformResult = NTransform::Pipeline(
        ast, Resolver, std::move(pipelineOptions));
    if (!transformResult) {
        if (error) {
            *error = transformResult.error().ToString();
        }
        return false;
    }

    auto lowerRes = Lowerer.LowerTop(ast, &entrySet);
    if (!lowerRes) {
        if (error) {
            *error = lowerRes.error().ToString();
        }
        return false;
    }

    // Another backend prints the IR after the Qumir passes, in EmitBackendKernels.
    if (!KernelsUseBackend()) {
        PrintModuleIr();
    }

    if (Module.Functions.empty()) {
        if (error) {
            *error = "no functions after lowering";
        }
        return false;
    }
    for (const auto& entryName : entryNames) {
        auto entry = std::find_if(
            Module.Functions.begin(), Module.Functions.end(),
            [&](const auto& function) { return function.Name == entryName; });
        if (entry == Module.Functions.end()) {
            if (error) {
                *error = "entry function not found: " + entryName;
            }
            return false;
        }
    }
    return true;
}

std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> TLLVMRunner::EmitLoweredModule(
    const std::unordered_set<std::string>* restrictToDefinitions,
    const std::unordered_set<std::string>* emitAsExternal,
    std::string* error,
    const std::vector<std::string>* llvmBitcode,
    const std::unordered_set<std::string>* inlineDefinitions,
    const std::unordered_set<std::string>* exports)
{
    if (error) {
        error->clear();
    }
    assert(!(restrictToDefinitions && emitAsExternal) &&
           "partition modes are mutually exclusive");

    NCodeGen::TLLVMCodeGen cg({
        .NativeCode = Options.NativeCode,
        .TargetTriple = Options.TargetTriple,
        .RestrictToDefinitions = restrictToDefinitions,
        .EmitAsExternal = emitAsExternal,
        .InlineDefinitions = inlineDefinitions,
        .Exports = exports,
        .LlvmBitcode = llvmBitcode,
    });
    std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> artifacts;
    try {
        artifacts = cg.Emit(Module, Options.OptLevel);
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("llvm codegen error: ") + e.what();
        }
        return nullptr;
    }

    if (Options.PrintLlvm) {
        artifacts->PrintModule(std::cerr);
    }
    if (Options.PrintAsm) {
        std::cerr << "=========== ASM: ===========\n";
        artifacts->Generate(std::cerr, /*generateAsm=*/true, /*generateObj=*/false);
        std::cerr << "============================\n\n";
    }

    return artifacts;
}

std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> TLLVMRunner::EmitKernelArtifacts(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    std::string* error,
    std::vector<NAst::TPragma> mainPragmas,
    const std::vector<std::string>* llvmBitcode)
{
    if (!LowerKernelAst(ast, entryNames, error, std::move(mainPragmas))) {
        return nullptr;
    }
    std::unordered_set<std::string> exports(entryNames.begin(), entryNames.end());
    return EmitLoweredModule(nullptr, nullptr, error, llvmBitcode, nullptr, &exports);
}

bool TLLVMRunner::KernelsUseBackend() const {
    return dynamic_cast<const NCodeGen::TLLVMBackend*>(Backend_.get()) == nullptr;
}

void TLLVMRunner::PrintModuleIr() {
    if (Options.PrintIr) {
        std::cerr << "=========== IR: ============\n";
        for (const auto& function : Module.Functions) {
            if (PrintedChunks.insert(function.UniqueId).second) {
                std::ostringstream oss;
                function.Print(oss, Module);
                std::cerr << oss.str() << std::endl;
            }
        }
        std::cerr << "============================\n\n";
    }
}

std::optional<std::string> TLLVMRunner::EmitBackendKernels(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    std::string* error,
    std::vector<NAst::TPragma> mainPragmas,
    const std::vector<std::string>& llvmBitcode)
{
    if (!llvmBitcode.empty()) {
        if (error) {
            *error = "the LLVM bitcode of source modules needs the LLVM codegen backend";
        }
        return std::nullopt;
    }
    if (!LowerKernelAst(ast, entryNames, error, std::move(mainPragmas))) {
        return std::nullopt;
    }
    // The LLVM codegen leaves the IR to LLVM -O; another backend gets the
    // Qumir IR passes, as Compile() runs them for Run().
    if (Options.OptLevel > 0) {
        Optimize();
    }
    PrintModuleIr();
    auto object = Backend_->EmitObject(Module, NCodeGen::EObjectUse::Load);
    if (!object) {
        if (error) {
            *error = object.error().ToString();
        }
        return std::nullopt;
    }
    return std::move(*object);
}

std::unordered_map<std::string, void*> TLLVMRunner::LinkBackendKernels(
    std::string object,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    auto linked = LlvmRunner_.LinkAndLookup(
        {}, {std::move(object)}, nullptr, Options.NativeCode, entryNames, error);
    if (linked.Entries.empty()) {
        return {};
    }
    BackendKernels_.push_back(std::move(linked.Lifetime));
    return std::move(linked.Entries);
}

std::unordered_map<std::string, void*> TLLVMRunner::CompileKernelAst(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    if (KernelsUseBackend()) {
        auto object = EmitBackendKernels(ast, entryNames, error);
        if (!object) {
            return {};
        }
        return LinkBackendKernels(std::move(*object), entryNames, error);
    }
    auto artifacts = EmitKernelArtifacts(ast, entryNames, error);
    if (!artifacts) {
        return {};
    }

    std::string runErr;
    auto entries = LlvmRunner_.LookupMany(std::move(artifacts), entryNames, &runErr);
    if (entries.empty()) {
        if (error) {
            *error = runErr.empty() ? "function lookup failed" : runErr;
        }
        return {};
    }
    return entries;
}

std::unordered_map<std::string, void*> TLLVMRunner::CompileKernelAst(
    NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    if (KernelsUseBackend()) {
        auto object = EmitBackendKernels(
            std::move(composed.Ast), entryNames, error, std::move(composed.Pragmas), composed.LlvmBitcode);
        if (!object) {
            return {};
        }
        return LinkBackendKernels(std::move(*object), entryNames, error);
    }
    auto llvmBitcode = std::move(composed.LlvmBitcode);
    auto artifacts = EmitKernelArtifacts(
        std::move(composed.Ast),
        entryNames,
        error,
        std::move(composed.Pragmas),
        &llvmBitcode);
    if (!artifacts) {
        return {};
    }

    std::string runErr;
    auto entries = LlvmRunner_.LookupMany(std::move(artifacts), entryNames, &runErr);
    if (entries.empty()) {
        if (error) {
            *error = runErr.empty() ? "function lookup failed" : runErr;
        }
        return {};
    }
    return entries;
}

std::optional<std::string> TLLVMRunner::CompileKernelAstToObject(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    if (KernelsUseBackend()) {
        return EmitBackendKernels(ast, entryNames, error);
    }
    auto artifacts = EmitKernelArtifacts(ast, entryNames, error);
    if (!artifacts) {
        return std::nullopt;
    }

    std::ostringstream obj(std::ios::binary);
    artifacts->Generate(obj, /*generateAsm=*/false, /*generateObj=*/true);
    return obj.str();
}

std::optional<std::string> TLLVMRunner::CompileKernelAstToObject(
    NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    if (KernelsUseBackend()) {
        return EmitBackendKernels(
            std::move(composed.Ast), entryNames, error, std::move(composed.Pragmas), composed.LlvmBitcode);
    }
    auto llvmBitcode = std::move(composed.LlvmBitcode);
    auto artifacts = EmitKernelArtifacts(
        std::move(composed.Ast),
        entryNames,
        error,
        std::move(composed.Pragmas),
        &llvmBitcode);
    if (!artifacts) {
        return std::nullopt;
    }

    std::ostringstream obj(std::ios::binary);
    artifacts->Generate(obj, /*generateAsm=*/false, /*generateObj=*/true);
    return obj.str();
}

std::optional<TLLVMRunner::TPreparedCachedCompilation>
TLLVMRunner::PrepareFusedKernelsCached(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    if (error) {
        error->clear();
    }
    if (KernelsUseBackend()) {
        if (error) {
            *error = "the kernel object cache needs the LLVM codegen backend";
        }
        return std::nullopt;
    }
    auto started = std::chrono::steady_clock::now();
    if (!LowerKernelAst(ast, entryNames, error)) {
        return std::nullopt;
    }
    auto frontendDone = std::chrono::steady_clock::now();

    // The full cacheable set of the (monomorphized) module. This is transitively
    // closed: if a cacheable A calls a cacheable B, B is instantiated here too,
    // so Resolve pulls B's object even when A is a miss and B a hit. The cache
    // therefore need not store per-object dependencies.
    auto required = NCodeGen::CollectCacheableSymbols(Module);

    auto fp = NCodeGen::MakeBuildFingerprint(
        Options.NativeCode, Options.TargetTriple, Options.OptLevel, cacheSchema, kernelLibVersion);
    // Dependency objects now inline selected callees; older objects remain
    // in a separate generation and cannot silently preserve call overhead.
    fp.OptSettings += ";cache-inline-selective-v3";
    auto cache = NCodeGen::TSymbolObjectCache::Open(cacheDir, fp);
    if (!cache) {
        if (error) {
            *error = cache.error().what();
        }
        return std::nullopt;
    }
    auto plan = cache->Resolve(required);
    auto resolved = std::chrono::steady_clock::now();

    // Compile and persist each missing dependency as its own object, so a kernel
    // loads exactly the symbols it needs. Each object is self-contained: it
    // references only other cacheable symbols (their objects, pulled by the
    // transitively-closed required set) and runtime symbols.
    std::vector<std::string> depBlobs;
    std::unordered_set<std::string> depSet(required.begin(), required.end());
    for (const auto& miss : plan.Misses) {
        std::unordered_set<std::string> one{miss};
        auto imports = Options.OptLevel > 0
            ? NCodeGen::CollectInlineDefinitions(Module, depSet, one)
            : std::unordered_set<std::string>{};
        auto depArt = EmitLoweredModule(&one, nullptr, error, nullptr, &imports);
        if (!depArt) {
            return std::nullopt;
        }
        auto provided = depArt->GetDefinedFunctionNames();
        if (provided.size() != 1 || provided[0] != miss) {
            if (error) {
                *error = "cache: per-symbol dependency codegen produced unexpected definitions";
            }
            return std::nullopt;
        }
        std::ostringstream obj(std::ios::binary);
        depArt->Generate(obj, /*generateAsm=*/false, /*generateObj=*/true);
        std::string depBytes = obj.str();
        if (auto reg = cache->Register(depBytes, provided); !reg) {
            if (error) {
                *error = reg.error().what();
            }
            return std::nullopt;
        }
        depBlobs.push_back(std::move(depBytes));
    }

    auto dependenciesDone = std::chrono::steady_clock::now();
    // Native objects own the exports. Only selected bodies are rebuilt for
    // inlining into this module, including when every dependency is a hit.
    std::unordered_set<std::string> definitions;
    for (const auto& function : Module.Functions) {
        if (!depSet.count(function.Name) && !function.Blocks.empty()) {
            definitions.insert(function.Name);
        }
    }
    auto imports = Options.OptLevel > 0
        ? NCodeGen::CollectInlineDefinitions(Module, depSet, definitions)
        : std::unordered_set<std::string>{};
    std::unordered_set<std::string> exports(entryNames.begin(), entryNames.end());
    auto kernelArt = EmitLoweredModule(
        /*restrict=*/nullptr, required.empty() ? nullptr : &depSet, error,
        nullptr, &imports, &exports);
    if (!kernelArt) {
        return std::nullopt;
    }

    auto kernelDone = std::chrono::steady_clock::now();
    auto milliseconds = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const size_t hitCount = plan.ObjectFiles.size();
    const size_t missCount = plan.Misses.size();
    return TPreparedCachedCompilation{
        .ObjectFiles = std::move(plan.ObjectFiles),
        .ObjectBlobs = std::move(depBlobs),
        .KernelModule = std::move(kernelArt),
        .RequiredCount = required.size(),
        .HitCount = hitCount,
        .MissCount = missCount,
        .ImportedCount = imports.size(),
        .FrontendMs = milliseconds(frontendDone - started),
        .ResolveMs = milliseconds(resolved - frontendDone),
        .DependenciesMs = milliseconds(dependenciesDone - resolved),
        .KernelMs = milliseconds(kernelDone - dependenciesDone),
    };
}

NCodeGen::TLlvmRunner::TLinkedModule TLLVMRunner::CompileFusedKernelsCached(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    auto tStart = std::chrono::steady_clock::now();
    auto prepared = PrepareFusedKernelsCached(
        ast, entryNames, cacheDir, cacheSchema, kernelLibVersion, error);
    if (!prepared) {
        return {};
    }
    auto tPrepared = std::chrono::steady_clock::now();

    auto linked = LlvmRunner_.LinkAndLookup(
        prepared->ObjectFiles,
        prepared->ObjectBlobs,
        std::move(prepared->KernelModule),
        Options.NativeCode,
        entryNames,
        error);

    if (std::getenv("QDB_JIT_CACHE_STATS")) {
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::cerr << "[cache] " << (entryNames.empty() ? "?" : entryNames[0])
                  << " req=" << prepared->RequiredCount
                  << " hit=" << prepared->HitCount
                  << " miss=" << prepared->MissCount
                  << " | prepare=" << ms(tPrepared - tStart) << "ms"
                  << " link=" << ms(std::chrono::steady_clock::now() - tPrepared) << "ms"
                  << " imported=" << prepared->ImportedCount
                  << " frontend=" << prepared->FrontendMs << "ms"
                  << " resolve=" << prepared->ResolveMs << "ms"
                  << " dependencies=" << prepared->DependenciesMs << "ms"
                  << " kernel=" << prepared->KernelMs << "ms\n";
    }
    return linked;
}

std::optional<TLLVMRunner::TCachedObjectModule>
TLLVMRunner::CompileFusedKernelsToObjectsCached(
    const NAst::TExprPtr& ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    auto tStart = std::chrono::steady_clock::now();
    auto prepared = PrepareFusedKernelsCached(
        ast, entryNames, cacheDir, cacheSchema, kernelLibVersion, error);
    if (!prepared) {
        return std::nullopt;
    }
    auto tPrepared = std::chrono::steady_clock::now();

    std::ostringstream object(std::ios::binary);
    prepared->KernelModule->Generate(
        object, /*generateAsm=*/false, /*generateObj=*/true);

    if (std::getenv("QDB_JIT_CACHE_STATS")) {
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::cerr << "[cache] " << (entryNames.empty() ? "?" : entryNames[0])
                  << " req=" << prepared->RequiredCount
                  << " hit=" << prepared->HitCount
                  << " miss=" << prepared->MissCount
                  << " | prepare=" << ms(tPrepared - tStart) << "ms"
                  << " objectEmit="
                  << ms(std::chrono::steady_clock::now() - tPrepared) << "ms"
                  << " imported=" << prepared->ImportedCount
                  << " frontend=" << prepared->FrontendMs << "ms"
                  << " resolve=" << prepared->ResolveMs << "ms"
                  << " dependencies=" << prepared->DependenciesMs << "ms"
                  << " kernel=" << prepared->KernelMs << "ms\n";
    }

    return TCachedObjectModule{
        .ObjectFiles = std::move(prepared->ObjectFiles),
        .ObjectBlobs = std::move(prepared->ObjectBlobs),
        .KernelObject = object.str(),
    };
}

void* TLLVMRunner::CompileKernel(const std::string& source, std::string* error) {
    std::vector<NAst::TPragma> mainPragmas;
    if (Options.AllowOverloads) {
        mainPragmas.push_back(NAst::TPragma{"language", {"overloads"}, {}});
    }

    NFrontend::TSourceModuleLoader loader(Types);
    for (const auto& dir : Options.ModuleSearchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : Options.ModuleFiles) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            if (error) {
                *error = reg.error().ToString();
            }
            return nullptr;
        }
    }

    std::istringstream input(source);

    NAst::NCore::TTokenStream ts(input);
    NAst::NCore::TParser p(Types);
    auto parsed = p.Parse(ts);
    if (!parsed) {
        if (error) {
            *error = parsed.error().ToString();
        }
        return nullptr;
    }
    mainPragmas.insert(mainPragmas.end(), p.Pragmas.begin(), p.Pragmas.end());

    std::string funcName;
    if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(*parsed)) {
        for (const auto& stmt : block.Cast()->Stmts) {
            if (auto fun = NAst::TMaybeNode<NAst::TFunDecl>(stmt)) {
                funcName = fun.Cast()->Name;
            }
        }
    }
    if (funcName.empty()) {
        if (error) {
            *error = "no functions in source";
        }
        return nullptr;
    }

    {
        auto composed = NFrontend::LoadAndCompose(loader, *parsed, mainPragmas);
        if (!composed) {
            if (error) {
                *error = composed.error().ToString();
            }
            return nullptr;
        }
        *parsed = std::move(composed->Ast);
        Resolver.ApplyPragmas(composed->Pragmas);
    }

    auto scope = Resolver.GetOrCreateRootScope();
    // scope->AllowsRedeclare = true;
    scope->RootLevel = false;

    if (auto err = Resolver.Resolve(*parsed)) {
        if (error) {
            *error = err->ToString();
        }
        return nullptr;
    }

    auto transformResult = NTransform::Pipeline(
        *parsed,
        Resolver,
        {.RunDefiniteAssignment = Options.RunDefiniteAssignment});
    if (!transformResult) {
        if (error) {
            *error = transformResult.error().ToString();
        }
        return nullptr;
    }

    auto lowerRes = Lowerer.LowerTop(*parsed);
    if (!lowerRes) {
        if (error) {
            *error = lowerRes.error().ToString();
        }
        return nullptr;
    }

    if (Module.Functions.empty()) {
        if (error) {
            *error = "no functions after lowering";
        }
        return nullptr;
    }
    NCodeGen::TLLVMCodeGen cg({
        .NativeCode = Options.NativeCode,
        .TargetTriple = Options.TargetTriple,
    });
    std::unique_ptr<NCodeGen::ILLVMModuleArtifacts> artifacts;
    try {
        artifacts = cg.Emit(Module, 0);
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("llvm codegen error: ") + e.what();
        }
        return nullptr;
    }

    std::string runErr;
    void* fnPtr = LlvmRunner_.Lookup(std::move(artifacts), funcName, &runErr);
    if (!fnPtr) {
        if (error) {
            *error = runErr.empty() ? "function not found: " + funcName : runErr;
        }
        return nullptr;
    }
    return fnPtr;
}

} // namespace NQumir
