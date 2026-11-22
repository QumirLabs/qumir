#pragma once

#include <qumir/error.h>
#include <qumir/frontend/source_module.h>

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace NQumir {
namespace NFrontend {

// Loads `.oz` modules and resolves their `use` dependencies into a stable
// dependency-first order. A module's name is its file stem (`math.oz` ->
// `math`). A `use` that resolves to a `.oz` file is followed as a source
// dependency; any other `use` is recorded as external and left to a later
// runtime-import stage. The loader parses and validates only - no name
// resolution, lowering or codegen.
class TSourceModuleLoader {
public:
    // Seeds search paths with the built-in module dirs relative to the binary.
    TSourceModuleLoader();

    void AddSearchPath(std::filesystem::path dir);

    // Maps an import name to a module name, so a `.oz` file (e.g. complex.oz)
    // can be imported under a display name (e.g. "Комплексные числа").
    void AddAlias(std::string alias, std::string moduleName);

    std::expected<std::monostate, TError> RegisterSourceModule(std::filesystem::path file);

    std::expected<const TSourceModule*, TError> Load(const std::string& name);

    // Loads an already parsed source module through the same interface and
    // dependency validation path as a module read from an `.oz` file.
    std::expected<const TSourceModule*, TError> LoadAst(
        const std::string& name,
        const NAst::TExprPtr& ast,
        std::vector<std::string> llvmBitcode = {},
        std::vector<NAst::TPragma> pragmas = {},
        std::filesystem::path origin = {});

    bool Resolvable(const std::string& name) const {
        return Cache.contains(name) || ResolvePath(name).has_value();
    }

    std::vector<const TSourceModule*> TopologicalOrder() const;

    const TSourceModule* Find(const std::string& name) const;

private:
    struct TCacheEntry {
        ESourceModuleState State = ESourceModuleState::Loading;
        std::unique_ptr<TSourceModule> Module;
    };

    std::optional<std::filesystem::path> ResolvePath(const std::string& name) const;

    std::vector<std::filesystem::path> SearchPaths;
    std::map<std::string, std::filesystem::path> ExplicitModules;
    std::map<std::string, std::string> Aliases;

    std::map<std::string, TCacheEntry> Cache;
    std::vector<std::string> LoadStack;
    std::vector<std::string> CompletionOrder;
};

} // namespace NFrontend
} // namespace NQumir
