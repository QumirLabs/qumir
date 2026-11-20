#pragma once

#include <qumir/error.h>

#include <expected>
#include <string>

namespace NQumir {

namespace NIR {
struct TModule;
} // namespace NIR

namespace NCodeGen {

// How the host uses an emitted object.
enum class EObjectUse {
    // Linked into a program: the module constructor and destructor run from
    // the platform init and fini lists.
    Link,
    // Loaded into this process by the JIT, which calls `$$module_constructor`
    // and `$$module_destructor` itself, so the object exports them.
    Load,
};

// Code generator from a Qumir IR module to machine code for one target. The
// object defines the functions of the module and leaves runtime and registry
// module symbols undefined; with coroutines it also defines
// `__qumir_is_coroutine` and `__qumir_coro_promise_ptr`.
class ICodegenBackend {
public:
    virtual ~ICodegenBackend() = default;

    // A relocatable object file.
    virtual std::expected<std::string, TError> EmitObject(NIR::TModule& module, EObjectUse use) = 0;
    // Assembly text of the same code (qumirc -S).
    virtual std::expected<std::string, TError> EmitAsm(NIR::TModule& module) = 0;
};

} // namespace NCodeGen
} // namespace NQumir
