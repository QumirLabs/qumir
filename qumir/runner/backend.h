#pragma once

#include <qumir/error.h>
#include <qumir/ir/builder.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace NQumir {

// Machine code of a lowered module, loaded into this process.
class ICompiledModule {
public:
    virtual ~ICompiledModule() = default;

    // Runs the module constructor, the entry point and the module destructor.
    // Returns the printed result of the entry point, if it has one.
    virtual std::expected<std::optional<std::string>, TError> Run() = 0;
};

// Code generator behind TJitRunner. Compile() does all code generation, so
// the runner can time it apart from Run() or stop before running.
class ICodegenBackend {
public:
    virtual ~ICodegenBackend() = default;

    // Compiles `module` and resolves its entry point. In a REPL session the
    // module grows between calls.
    virtual std::expected<std::unique_ptr<ICompiledModule>, TError> Compile(NIR::TModule& module) = 0;
};

} // namespace NQumir
