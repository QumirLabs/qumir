#pragma once

// Experimental TPDE backend (https://github.com/tpde2/tpde): compiles Qumir IR
// straight to machine code without LLVM. Draft: AArch64 ELF only, a subset of
// the IR; callers check support first and fall back to the LLVM backend.

#include <qumir/ir/builder.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NQumir::NCodeGen {

// Op kinds (or other constructs) the TPDE backend cannot compile -> count.
using TTpdeUnsupported = std::map<std::string, int>;

// Returns true when every function of the module can be compiled by TPDE.
bool TpdeCheckModule(NIR::TModule& module, TTpdeUnsupported* unsupported);

struct TTpdeStats {
    double CompileMs = 0; // IR -> machine code in memory (TPDE compile())
    double ObjectMs = 0;  // ELF object serialization, only if requested
    double MapMs = 0;     // mapping into executable memory, only for JIT
    size_t ObjectBytes = 0;
    size_t TextBytes = 0;
};

// Compiled and mapped module; function pointers live as long as this object.
class TTpdeModule {
public:
    virtual ~TTpdeModule() = default;
    virtual void* Lookup(const std::string& name) const = 0;
};

// Compiles the module into an ELF object file.
std::optional<std::vector<uint8_t>> TpdeCompileObject(
    NIR::TModule& module, TTpdeStats* stats, std::string* error);

// Compiles the module and maps it into memory; external symbols resolve
// against the current process (like the LLVM JIT).
std::unique_ptr<TTpdeModule> TpdeCompileAndMap(
    NIR::TModule& module, TTpdeStats* stats, std::string* error);

// Runs $$module_constructor, the entry and $$module_destructor like
// TLlvmRunner::Run; returns the printed entry result.
std::optional<std::string> TpdeRunEntry(
    const TTpdeModule& compiled,
    const NIR::TModule& module,
    const NIR::TFunction& entry,
    double* entryMs,
    std::string* error);

} // namespace NQumir::NCodeGen
