#include "llvm_runner.h"
#include "llvm_codegen_impl.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Module.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/MemoryBuffer.h>
#if defined(__linux__)
#include <llvm/ExecutionEngine/Orc/Debugging/PerfSupportPlugin.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderPerf.h>
#endif
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <vector>
#include <sstream>
#include <setjmp.h>
#include <stdexcept>
#include <functional>
#include <type_traits>
#include <utility>

#include <qumir/runtime/string.h> // for str_release
#include <qumir/runtime/runtime.h> // for __ensure and longjmp escape hatch
#include <qumir/runtime/robot.h>
#include <qumir/runtime/turtle.h>
#include <qumir/runtime/drawer.h>
#include <qumir/runtime/painter.h>
#include <qumir/runtime/future.h>

#include <cassert>


// Symbol anchors: prevent macOS from dead-stripping __qumir_future_* and
// __qumir_wrap_coro. These are only called from JIT-compiled IR, so the
// static linker sees no compile-time references; without this -rdynamic
// has nothing to export for the JIT symbol lookup. The anchor lives here
// (in llvm_runner.cpp) because this TU is always included in the link.
__attribute__((used))
static const void* const kQumir_jit_symbol_anchors[] = {
    reinterpret_cast<const void*>(&NQumir::__qumir_future_destroy),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_done),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_resume),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_address),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_await_ready),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_await_suspend),
    reinterpret_cast<const void*>(&NQumir::__qumir_future_await_resume),
    reinterpret_cast<const void*>(&NQumir::__qumir_wrap_coro),
};

namespace NQumir::NCodeGen {

using namespace NIR;

namespace {

void InitializeNativeJitTarget() {
    static const bool initialized = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        return true;
    }();
    (void)initialized;
}

std::string ToString(llvm::Error error) {
    return llvm::toString(std::move(error));
}

template <typename T>
std::optional<T> TakeExpected(llvm::Expected<T> expected, std::string* error) {
    if (!expected) {
        if (error) {
            *error = ToString(expected.takeError());
        } else {
            llvm::consumeError(expected.takeError());
        }
        return std::nullopt;
    }
    return std::move(*expected);
}

bool EnablePerfSupport(llvm::orc::LLJIT& jit, std::string* error) {
#if defined(__linux__)
    auto* objectLayer = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>(&jit.getObjLinkingLayer());
    if (!objectLayer) {
        if (error) {
            *error = "ORC perf support requires ObjectLinkingLayer";
        }
        return false;
    }
    auto& epc = jit.getExecutionSession().getExecutorProcessControl();
    if (!epc.getTargetTriple().isOSBinFormatELF()) {
        if (error) {
            *error = "ORC perf support is only available for ELF targets";
        }
        return false;
    }
    auto plugin = std::make_shared<llvm::orc::PerfSupportPlugin>(
        epc,
        llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderPerfStart),
        llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderPerfEnd),
        llvm::orc::ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderPerfImpl),
        /*EmitDebugInfo=*/true,
        /*EmitUnwindInfo=*/true);
    objectLayer->addPlugin(std::move(plugin));
    return true;
#else
    (void)jit;
    (void)error;
    return true;
#endif
}

void SetObjectLinkingLayerCreator(llvm::orc::LLJITBuilder& builder) {
#if LLVM_VERSION_MAJOR <= 20
    builder.setObjectLinkingLayerCreator(
        [](llvm::orc::ExecutionSession& es, const llvm::Triple&)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
        {
            return std::make_unique<llvm::orc::ObjectLinkingLayer>(es);
        });
#elif LLVM_VERSION_MAJOR >= 23
    builder.setObjectLinkingLayerCreator(
        [](llvm::orc::ExecutionSession& es, llvm::jitlink::JITLinkMemoryManager& memoryManager)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
        {
            return std::make_unique<llvm::orc::ObjectLinkingLayer>(es, memoryManager);
        });
#else
    builder.setObjectLinkingLayerCreator(
        [](llvm::orc::ExecutionSession& es)
            -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
        {
            return std::make_unique<llvm::orc::ObjectLinkingLayer>(es);
        });
#endif
}

std::unique_ptr<llvm::orc::LLJIT> CreateOrcJit(
    bool nativeCode,
    bool enablePerf,
    std::string* error)
{
    llvm::orc::JITTargetMachineBuilder jtmb{
        llvm::Triple(llvm::sys::getProcessTriple())};
    if (nativeCode) {
        auto nativeJtmb = TakeExpected(llvm::orc::JITTargetMachineBuilder::detectHost(), error);
        if (!nativeJtmb) {
            return nullptr;
        }
        jtmb = std::move(*nativeJtmb);
    }
    jtmb.setRelocationModel(llvm::Reloc::PIC_);

    llvm::orc::LLJITBuilder builder;
    builder.setJITTargetMachineBuilder(std::move(jtmb));
    SetObjectLinkingLayerCreator(builder);
    auto jit = TakeExpected(builder.create(), error);
    if (!jit) {
        return nullptr;
    }
    if (enablePerf && !EnablePerfSupport(**jit, error)) {
        return nullptr;
    }
    return std::move(*jit);
}

bool AddArtifactsToJit(TLLVMModuleArtifacts& artifacts, llvm::orc::LLJIT& jit, std::string* error) {
    artifacts.Module->setDataLayout(jit.getDataLayout());
    auto module = llvm::orc::ThreadSafeModule(
        std::move(artifacts.Module),
        std::move(artifacts.Ctx));
    if (auto err = jit.addIRModule(std::move(module))) {
        if (error) {
            *error = ToString(std::move(err));
        } else {
            llvm::consumeError(std::move(err));
        }
        return false;
    }
    return true;
}

template <typename TFunction>
std::optional<TFunction> LookupFunction(llvm::orc::LLJIT& jit, const std::string& name, std::string* error) {
    auto addr = jit.lookup(name);
    if (!addr) {
        if (error) {
            *error = ToString(addr.takeError());
        } else {
            llvm::consumeError(addr.takeError());
        }
        return std::nullopt;
    }
    return addr->toPtr<TFunction>();
}

enum class EReturnKind {
    Void,
    Bool,
    Integer,
    Float,
    Double,
    Pointer,
    Unsupported,
};

struct TEntryInfo {
    std::string Name;
    std::string ConstructorName;
    std::string DestructorName;
    std::string CoroutinePromisePtrName;
    EReturnKind ReturnKind = EReturnKind::Unsupported;
    unsigned IntegerBits = 0;
    bool IsCoroutineModule = false;
};

EReturnKind ReturnKindOf(llvm::Type* type) {
    if (type->isVoidTy()) {
        return EReturnKind::Void;
    }
    if (type->isIntegerTy()) {
        return type->getIntegerBitWidth() == 1 ? EReturnKind::Bool : EReturnKind::Integer;
    }
    if (type->isFloatTy()) {
        return EReturnKind::Float;
    }
    if (type->isDoubleTy()) {
        return EReturnKind::Double;
    }
    if (type->isPointerTy()) {
        return EReturnKind::Pointer;
    }
    return EReturnKind::Unsupported;
}

std::optional<TEntryInfo> PrepareRunEntry(llvm::Module& module, const std::string& entryPoint, std::string* error) {
    TEntryInfo info;
    llvm::Function* target = nullptr;
    llvm::Function* last = nullptr;
    for (auto& f : module) {
        last = &f;
        std::string name = f.getName().str();
        if (name == entryPoint) {
            target = &f;
        }
        if (name == "$$module_constructor") {
            f.setLinkage(llvm::Function::ExternalLinkage);
            info.ConstructorName = std::move(name);
        } else if (name == "$$module_destructor") {
            f.setLinkage(llvm::Function::ExternalLinkage);
            info.DestructorName = std::move(name);
        } else if (name == "__qumir_coro_promise_ptr") {
            info.CoroutinePromisePtrName = std::move(name);
        }
    }
    if (!target) {
        target = last;
    }
    if (!target) {
        if (error) {
            *error = "no function in module";
        }
        return std::nullopt;
    }

    auto* type = target->getFunctionType();
    if (type->getNumParams() != 0) {
        if (error) {
            *error = "function requires arguments (unsupported)";
        }
        return std::nullopt;
    }

    auto* retType = type->getReturnType();
    info.Name = target->getName().str();
    info.ReturnKind = ReturnKindOf(retType);
    if (retType->isIntegerTy()) {
        info.IntegerBits = retType->getIntegerBitWidth();
    }
    info.IsCoroutineModule = (module.getGlobalVariable("__qumir_is_coroutine") != nullptr);
    return info;
}

} // namespace

#ifdef __APPLE__
// On macOS, the JIT needs __dso_handle for global constructors/destructors
// registered via __cxa_atexit. Provide a dummy symbol for the JIT to resolve.
extern "C" {
    void* __dso_handle = (void*)&__dso_handle;
} // extern "C"
#endif

// Wraps a single JIT entry call with a setjmp guard so that if the JIT
// program calls __ensure which triggers longjmp, we rethrow as a normal
// C++ exception (through host frames) rather than trying to unwind through
// JIT frames that lack DWARF unwind info (fatal on macOS).
//
// Must be noinline: inlining into Run() would put C++ objects with dtors
// between the setjmp and the potential longjmp.
template <typename TResult, typename TFunction, typename... TArgs>
[[gnu::noinline]] static TResult SafeJitCall(TFunction function, TArgs... args)
{
    jmp_buf jb;
    __set_jmp_target(&jb);
    if (setjmp(jb) != 0) {
        __clear_jmp_target();
        throw std::runtime_error(__get_runtime_error());
    }
    if constexpr (std::is_void_v<TResult>) {
        function(args...);
        __clear_jmp_target();
        return;
    } else {
        auto result = function(args...);
        __clear_jmp_target();
        return result;
    }
}

template <typename TInteger>
std::optional<int64_t> RunIntegerFunction(llvm::orc::LLJIT& jit, const std::string& name, std::string* error) {
    using TFn = TInteger (*)();
    auto function = LookupFunction<TFn>(jit, name, error);
    if (!function) {
        return std::nullopt;
    }
    return static_cast<int64_t>(SafeJitCall<TInteger>(*function));
}

static std::optional<std::string> RunEntryFunction(
    llvm::orc::LLJIT& jit,
    const TEntryInfo& entry,
    bool returnTypeIsString,
    std::string* error)
{
    std::ostringstream oss;
    switch (entry.ReturnKind) {
        case EReturnKind::Void: {
            using TFn = void (*)();
            auto function = LookupFunction<TFn>(jit, entry.Name, error);
            if (!function) {
                return std::nullopt;
            }
            SafeJitCall<void>(*function);
            return std::nullopt;
        }
        case EReturnKind::Bool: {
            using TFn = bool (*)();
            auto function = LookupFunction<TFn>(jit, entry.Name, error);
            if (!function) {
                return std::nullopt;
            }
            oss << (SafeJitCall<bool>(*function) ? "true" : "false");
            return oss.str();
        }
        case EReturnKind::Integer: {
            std::optional<int64_t> result;
            if (entry.IntegerBits <= 8) {
                result = RunIntegerFunction<int8_t>(jit, entry.Name, error);
            } else if (entry.IntegerBits <= 16) {
                result = RunIntegerFunction<int16_t>(jit, entry.Name, error);
            } else if (entry.IntegerBits <= 32) {
                result = RunIntegerFunction<int32_t>(jit, entry.Name, error);
            } else if (entry.IntegerBits <= 64) {
                result = RunIntegerFunction<int64_t>(jit, entry.Name, error);
            } else {
                if (error) {
                    *error = "unsupported integer return width: " + std::to_string(entry.IntegerBits);
                }
                return std::nullopt;
            }
            if (!result) {
                return std::nullopt;
            }
            oss << *result;
            return oss.str();
        }
        case EReturnKind::Float: {
            using TFn = float (*)();
            auto function = LookupFunction<TFn>(jit, entry.Name, error);
            if (!function) {
                return std::nullopt;
            }
            oss << std::fixed << std::setprecision(15) << SafeJitCall<float>(*function);
            return oss.str();
        }
        case EReturnKind::Double: {
            using TFn = double (*)();
            auto function = LookupFunction<TFn>(jit, entry.Name, error);
            if (!function) {
                return std::nullopt;
            }
            oss << std::fixed << std::setprecision(15) << SafeJitCall<double>(*function);
            return oss.str();
        }
        case EReturnKind::Pointer: {
            using TFn = void* (*)();
            auto function = LookupFunction<TFn>(jit, entry.Name, error);
            if (!function) {
                return std::nullopt;
            }
            auto* ptr = static_cast<char*>(SafeJitCall<void*>(*function));
            if (ptr) {
                oss << ptr;
                if (returnTypeIsString) {
                    NRuntime::str_release(ptr);
                }
            } else {
                oss << "(null)";
            }
            return oss.str();
        }
        case EReturnKind::Unsupported:
            break;
    }
    if (error) {
        *error = "unsupported function return type";
    }
    return std::nullopt;
}

static bool RunVoidFunctionIfPresent(
    llvm::orc::LLJIT& jit,
    const std::string& name,
    std::string* error)
{
    if (name.empty()) {
        return true;
    }
    using TFn = void (*)();
    auto function = LookupFunction<TFn>(jit, name, error);
    if (!function) {
        return false;
    }
    SafeJitCall<void>(*function);
    return true;
}

static void* RunCoroutineEntry(llvm::orc::LLJIT& jit, const std::string& name, std::string* error) {
    using TFn = void* (*)();
    auto function = LookupFunction<TFn>(jit, name, error);
    if (!function) {
        return nullptr;
    }
    return SafeJitCall<void*>(*function);
}

static std::optional<void*> RunPromisePtrFunction(
    llvm::orc::LLJIT& jit,
    const std::string& name,
    void* handle,
    std::string* error)
{
    using TFn = void* (*)(void*);
    auto function = LookupFunction<TFn>(jit, name, error);
    if (!function) {
        return std::nullopt;
    }
    auto* result = SafeJitCall<void*>(*function, handle);
    return result;
}

TLlvmRunner::TLlvmRunner(TLlvmRunnerOptions options)
    : Options_(std::move(options))
{
    InitializeNativeJitTarget();
}

std::optional<std::string> TLlvmRunner::Run(
    std::unique_ptr<ILLVMModuleArtifacts> iartifacts,
    const std::string& entryPoint,
    std::string* error,
    bool returnTypeIsString,
    std::function<std::optional<std::string>(const void*)> coroutineResultFormatter) {
    if (error) {
        error->clear();
    }
    std::string localError;
    auto* runError = error ? error : &localError;

    auto* artifacts = static_cast<TLLVMModuleArtifacts*>(iartifacts.get());
    if (!artifacts || !artifacts->Module) {
        *runError = "null artifacts";
        return std::nullopt;
    }
    auto entry = PrepareRunEntry(*artifacts->Module, entryPoint, runError);
    if (!entry) {
        return std::nullopt;
    }

    InitializeNativeJitTarget();

    // Make symbols from the current process available to the JIT. On Linux,
    // this requires the executable to be linked with -rdynamic as well.
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    auto jit = CreateOrcJit(
        artifacts->NativeCode,
        Options_.EnablePerfJitEventListener,
        runError);
    if (!jit) {
        return std::nullopt;
    }
    if (!AddArtifactsToJit(*artifacts, *jit, runError)) {
        return std::nullopt;
    }

    if (!RunVoidFunctionIfPresent(*jit, entry->ConstructorName, runError)) {
        return std::nullopt;
    }

    auto processEvents = [&]() {
        size_t processed = 0;
        processed += NRuntime::robot_process_events();
        processed += NRuntime::turtle_process_events();
        processed += NRuntime::drawer_process_events();
        processed += NRuntime::painter_process_events();
        processed += NRuntime::io_process_events();
        return processed;
    };

    if (entry->IsCoroutineModule) {
        void* handle = RunCoroutineEntry(*jit, entry->Name, runError);
        if (!handle) {
            return std::nullopt;
        }
        // Wrap the raw coro frame in ITypeErasedFuture* and use the public
        // __qumir_future_* API for the event loop. This avoids any dependency
        // on the __qumir_coro_* LLVM-intrinsic wrappers.
        ITypeErasedFuture* future = __qumir_wrap_coro(handle, 0);

        while (!__qumir_future_done(future)) {
            size_t processed = processEvents();
            assert(processed > 0 && "coroutine suspended with no pending async events");
            if (!__qumir_future_done(future)) {
                __qumir_future_resume(future);
            }
        }
        // Flush any remaining batched calls (e.g. painter drawing commands).
        processEvents();

        std::optional<std::string> result;
        if (coroutineResultFormatter && !entry->CoroutinePromisePtrName.empty()) {
            auto promisePtr = RunPromisePtrFunction(*jit, entry->CoroutinePromisePtrName, handle, runError);
            if (promisePtr) {
                result = coroutineResultFormatter(*promisePtr);
            }
        }

        __qumir_future_destroy(future);

        if (!RunVoidFunctionIfPresent(*jit, entry->DestructorName, runError)) {
            return std::nullopt;
        }
        return result;
    }

    auto result = RunEntryFunction(*jit, *entry, returnTypeIsString, runError);
    if (!runError->empty()) {
        return std::nullopt;
    }
    if (!RunVoidFunctionIfPresent(*jit, entry->DestructorName, runError)) {
        return std::nullopt;
    }
    return result;
}

void* TLlvmRunner::Lookup(
    std::unique_ptr<ILLVMModuleArtifacts> iartifacts,
    const std::string& name,
    std::string* error)
{
    if (error) {
        error->clear();
    }
    std::string localError;
    auto* lookupError = error ? error : &localError;

    auto entries = LookupMany(std::move(iartifacts), {name}, lookupError);
    auto it = entries.find(name);
    return it == entries.end() ? nullptr : it->second;
}

std::unordered_map<std::string, void*> TLlvmRunner::LookupMany(
    std::unique_ptr<ILLVMModuleArtifacts> iartifacts,
    const std::vector<std::string>& names,
    std::string* error)
{
    if (error) {
        error->clear();
    }
    std::string localError;
    auto* lookupError = error ? error : &localError;

    auto* artifacts = static_cast<TLLVMModuleArtifacts*>(iartifacts.get());
    if (!artifacts || !artifacts->Module) {
        *lookupError = "null artifacts";
        return {};
    }

    InitializeNativeJitTarget();

    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    auto jit = CreateOrcJit(
        artifacts->NativeCode,
        Options_.EnablePerfJitEventListener,
        lookupError);
    if (!jit) {
        return {};
    }
    if (!AddArtifactsToJit(*artifacts, *jit, lookupError)) {
        return {};
    }

    std::unordered_map<std::string, void*> entries;
    entries.reserve(names.size());
    for (const auto& name : names) {
        auto addr = jit->lookup(name);
        if (!addr) {
            *lookupError = ToString(addr.takeError());
            return {};
        }
        entries.emplace(name, addr->toPtr<void*>());
    }

    struct TLiveJit {
        std::unique_ptr<llvm::orc::LLJIT> Jit;
    };

    auto live = std::make_shared<TLiveJit>();
    live->Jit = std::move(jit);
    LiveEngines_.push_back(std::move(live));
    return entries;
}

TLlvmRunner::TLinkedModule TLlvmRunner::LinkAndLookup(
    const std::vector<std::string>& objectPaths,
    const std::vector<std::string>& objectBlobs,
    std::unique_ptr<ILLVMModuleArtifacts> kernelModule,
    bool nativeCode,
    const std::vector<std::string>& names,
    std::string* error)
{
    std::string localError;
    auto* err = error ? error : &localError;
    err->clear();

    InitializeNativeJitTarget();
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);

    auto jit = CreateOrcJit(nativeCode, Options_.EnablePerfJitEventListener, err);
    if (!jit) {
        return {};
    }

    auto addObject = [&](std::unique_ptr<llvm::MemoryBuffer> buf) -> bool {
        if (auto e = jit->addObjectFile(std::move(buf))) {
            *err = ToString(std::move(e));
            return false;
        }
        return true;
    };

    // Objects first so the kernel module resolves their symbols by name.
    for (const auto& path : objectPaths) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) {
            *err = "cannot read object file " + path + ": " + buf.getError().message();
            return {};
        }
        if (!addObject(std::move(*buf))) {
            return {};
        }
    }
    for (size_t i = 0; i < objectBlobs.size(); ++i) {
        auto buf = llvm::MemoryBuffer::getMemBufferCopy(
            objectBlobs[i], "cached-object-" + std::to_string(i));
        if (!addObject(std::move(buf))) {
            return {};
        }
    }

    if (kernelModule) {
        auto* artifacts = dynamic_cast<TLLVMModuleArtifacts*>(kernelModule.get());
        if (!artifacts || !artifacts->Module) {
            *err = "unexpected kernel artifacts implementation";
            return {};
        }
        if (!AddArtifactsToJit(*artifacts, *jit, err)) {
            return {};
        }
    }

    TLinkedModule linked;
    linked.Entries.reserve(names.size());
    for (const auto& name : names) {
        auto addr = jit->lookup(name);
        if (!addr) {
            *err = ToString(addr.takeError());
            return {};
        }
        linked.Entries.emplace(name, addr->toPtr<void*>());
    }

    struct TLiveJit {
        std::unique_ptr<llvm::orc::LLJIT> Jit;
    };
    auto live = std::make_shared<TLiveJit>();
    live->Jit = std::move(jit);
    linked.Lifetime = std::move(live);
    return linked;
}

TBuildFingerprint MakeBuildFingerprint(
    bool nativeCode,
    const std::string& targetTriple,
    int optLevel,
    std::string cacheSchema,
    std::string kernelLibVersion)
{
    TBuildFingerprint fp;
    fp.CacheSchema = std::move(cacheSchema);
    fp.KernelLibVersion = std::move(kernelLibVersion);
    fp.LlvmVersion = LLVM_VERSION_STRING;
    fp.Triple = targetTriple.empty() ? llvm::sys::getProcessTriple() : targetTriple;

    llvm::orc::JITTargetMachineBuilder jtmb{llvm::Triple(fp.Triple)};
    if (nativeCode) {
        std::vector<std::string> feats;
        for (const auto& f : llvm::sys::getHostCPUFeatures()) {
            feats.push_back((f.getValue() ? "+" : "-") + f.getKey().str());
        }
        std::sort(feats.begin(), feats.end());
        std::string joined = llvm::sys::getHostCPUName().str();
        for (const auto& f : feats) {
            joined += ",";
            joined += f;
        }
        fp.CpuFeatures = std::move(joined);

        InitializeNativeJitTarget();
        if (auto host = llvm::orc::JITTargetMachineBuilder::detectHost()) {
            jtmb = std::move(*host);
        } else {
            llvm::consumeError(host.takeError());
        }
    } else {
        fp.CpuFeatures = "generic";
    }
    // Authoritative DataLayout from the same target machine that codegen uses.
    if (auto tm = jtmb.createTargetMachine()) {
        fp.DataLayout = (*tm)->createDataLayout().getStringRepresentation();
    } else {
        llvm::consumeError(tm.takeError());
    }
    fp.OptSettings = "O" + std::to_string(optLevel);
    return fp;
}

} // namespace NQumir::NCodeGen
