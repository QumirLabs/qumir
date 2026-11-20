#include "runner_llvm.h"

#include <qumir/runner/backend_llvm.h>
#include <qumir/semantics/kumir/pipeline.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/semantics/transform/transform.h>
#include <qumir/frontend/compose.h>
#include <qumir/frontend/source_module_loader.h>

#include <algorithm>
#include <iostream>
#include <cassert>
#include <sstream>

namespace NQumir {

using namespace NIR;

namespace {

TJitRunnerOptions JitRunnerOptions(const TLLVMRunnerOptions& options) {
    return {
        .PrintAst = options.PrintAst,
        .PrintTransformedAst = options.PrintTransformedAst,
        .PrintIr = options.PrintIr,
        .CoreInput = options.CoreInput,
        .ResolveCoreInput = options.ResolveCoreInput,
        .AllowOverloads = options.AllowOverloads,
        .RunDefiniteAssignment = options.RunDefiniteAssignment,
        .OptLevel = options.OptLevel,
        .Prelude = options.Prelude,
        .ModuleSearchPaths = options.ModuleSearchPaths,
        .ModuleFiles = options.ModuleFiles,
        .TargetTriple = options.TargetTriple,
        .SourceFilePath = options.SourceFilePath,
        .EmitDebugInfo = options.EmitDebugInfo,
    };
}

TLlvmBackendOptions LlvmBackendOptions(const TLLVMRunnerOptions& options) {
    return {
        .PrintLlvm = options.PrintLlvm,
        .PrintAsm = options.PrintAsm,
        .NativeCode = options.NativeCode,
        .EnablePerfJitEventListener = options.EnablePerfJitEventListener,
        .OptLevel = options.OptLevel,
        .TargetTriple = options.TargetTriple,
    };
}

} // namespace

TLLVMRunner::TLLVMRunner(TLLVMRunnerOptions options)
    : TJitRunner(
        JitRunnerOptions(options),
        std::make_unique<TLlvmBackend>(LlvmBackendOptions(options)))
    , Options(std::move(options))
    , LlvmRunner_({
        .EnablePerfJitEventListener = Options.EnablePerfJitEventListener,
    })
{}

void* TLLVMRunner::CompileKernelAst(
    NAst::TExprPtr ast, const std::string& entryName, std::string* error)
{
    auto entries = CompileKernelAst(std::move(ast), std::vector<std::string>{entryName}, error);
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

    NFrontend::TSourceModuleLoader loader;
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

    if (auto err = Resolver.Resolve(ast)) {
        if (error) {
            *error = err->ToString();
        }
        return false;
    }

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
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error,
    std::vector<NAst::TPragma> mainPragmas,
    const std::vector<std::string>* llvmBitcode)
{
    if (!LowerKernelAst(std::move(ast), entryNames, error, std::move(mainPragmas))) {
        return nullptr;
    }
    std::unordered_set<std::string> exports(entryNames.begin(), entryNames.end());
    return EmitLoweredModule(nullptr, nullptr, error, llvmBitcode, nullptr, &exports);
}

std::unordered_map<std::string, void*> TLLVMRunner::CompileKernelAst(
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    auto artifacts = EmitKernelArtifacts(std::move(ast), entryNames, error);
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
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    auto artifacts = EmitKernelArtifacts(std::move(ast), entryNames, error);
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
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    if (error) {
        error->clear();
    }
    auto started = std::chrono::steady_clock::now();
    if (!LowerKernelAst(std::move(ast), entryNames, error)) {
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
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    auto tStart = std::chrono::steady_clock::now();
    auto prepared = PrepareFusedKernelsCached(
        std::move(ast), entryNames, cacheDir, cacheSchema, kernelLibVersion, error);
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
    NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    const std::string& cacheSchema,
    const std::string& kernelLibVersion,
    std::string* error)
{
    auto tStart = std::chrono::steady_clock::now();
    auto prepared = PrepareFusedKernelsCached(
        std::move(ast), entryNames, cacheDir, cacheSchema, kernelLibVersion, error);
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

    NFrontend::TSourceModuleLoader loader;
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
    NAst::NCore::TParser p;
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
