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
    // Loaded by a JIT, in this process or on another node: no init lists run,
    // so the object exports `$$module_constructor` and `$$module_destructor`
    // for the loader to call when it needs them.
    Load,
};

// Qumir IR module to machine code for one target. The object leaves runtime
// and registry symbols undefined; with coroutines it defines the two
// `__qumir_*` coroutine helpers.
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
