#pragma once

#include <expected>
#include <string>
#include <vector>

#include <qumir/parser/pragma.h>
#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

namespace NQumir {

namespace NRegistry {
    class IModule;

    struct TLiteralSuffix {
        std::string Suffix;
        std::string CtorFunction;
    };
} // namespace NRegistry

class IModuleManager {
public:
    virtual ~IModuleManager() = default;

    // Import a module by name.
    // Returns true if freshly imported, false if already imported (idempotent).
    // Returns error string on failure (unknown module or name conflict).
    virtual std::expected<bool, std::string> ImportModule(const std::string& name) = 0;
    virtual NAst::TTypePtr LookupType(const std::string& name) const = 0;
    // Registers a type exported by an imported source (`.oz`) module so it
    // resolves during parsing of the importing program. Default no-op.
    virtual void RegisterImportedType(const std::string& name, const NAst::TTypePtr& type) {}
    // All type names currently imported (including transitive dependencies)
    virtual std::vector<std::string> GetAllImportedTypeNames() const = 0;
    // All literal suffixes currently imported (including transitive dependencies)
    virtual std::vector<NRegistry::TLiteralSuffix> GetAllImportedLiteralSuffixes() const = 0;

    virtual void ApplyPragmas(const std::vector<NAst::TPragma>& pragmas) {}
};

} // namespace NQumir
