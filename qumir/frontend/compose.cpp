#include "compose.h"

#include <qumir/frontend/source_module_loader.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace NQumir {
namespace NFrontend {

using namespace NAst;

namespace {

struct TUnit {
    const std::vector<TExprPtr>* Stmts;
    std::string Label;
    std::string Origin;
    bool IsMain;
};

bool IsGlobal(const TExprPtr& stmt) {
    return TMaybeNode<TVarStmt>(stmt) || TMaybeNode<TVarsBlockExpr>(stmt)
        || TMaybeNode<TAssignExpr>(stmt) || TMaybeNode<TArrayAssignExpr>(stmt);
}

std::string Loc(const TLocation& loc) {
    return std::to_string(loc.Line) + ":" + std::to_string(loc.Column);
}

std::expected<std::vector<TPragma>, TError> MergePragmas(
    const std::vector<const TSourceModule*>& modules,
    const std::vector<TPragma>& mainPragmas,
    const std::string& mainLabel)
{
    std::vector<TPragma> merged;
    std::unordered_map<std::string, size_t> byGroup;

    auto add = [&](const TPragma& pragma, const std::string& label) -> std::optional<TError> {
        auto it = byGroup.find(pragma.Group);
        if (it == byGroup.end()) {
            byGroup[pragma.Group] = merged.size();
            merged.push_back(pragma);
            return std::nullopt;
        }
        if (merged[it->second].Values != pragma.Values) {
            return TError(pragma.Location, "несовместимые прагмы группы `" + pragma.Group
                + "' в составном модуле (" + label + ")");
        }
        return std::nullopt;
    };

    for (const auto* m : modules) {
        for (const auto& pragma : m->Pragmas) {
            if (auto err = add(pragma, m->Path.string())) {
                return std::unexpected(*err);
            }
        }
    }
    for (const auto& pragma : mainPragmas) {
        if (auto err = add(pragma, mainLabel)) {
            return std::unexpected(*err);
        }
    }
    return merged;
}

bool AllowsOverloads(const std::vector<TPragma>& pragmas) {
    for (const auto& pragma : pragmas) {
        if (pragma.Group == "language"
            && std::find(pragma.Values.begin(), pragma.Values.end(), "overloads") != pragma.Values.end()) {
            return true;
        }
    }
    return false;
}

struct TDecl {
    EExportKind Kind;
    std::string Label;
    TLocation Location;
};

std::optional<TError> CheckConflicts(const std::vector<TUnit>& units, bool allowOverloads) {
    std::unordered_map<std::string, std::vector<TDecl>> byName;
    for (const auto& unit : units) {
        for (const auto& stmt : *unit.Stmts) {
            if (auto type = TMaybeNode<TTypeDeclStmt>(stmt)) {
                auto named = TMaybeType<TNamedType>(type.Cast()->Type);
                byName[named.Cast()->Name].push_back({EExportKind::Type, unit.Label, stmt->Location});
            } else if (auto fun = TMaybeNode<TFunDecl>(stmt)) {
                byName[fun.Cast()->Name].push_back({EExportKind::Function, unit.Label, stmt->Location});
            } else if (auto var = TMaybeNode<TVarStmt>(stmt)) {
                byName[var.Cast()->Name].push_back({EExportKind::Global, unit.Label, stmt->Location});
            }
        }
    }

    for (const auto& [name, decls] : byName) {
        auto funcs = std::count_if(decls.begin(), decls.end(),
            [](const TDecl& d) { return d.Kind == EExportKind::Function; });
        // Types and globals are unique (non-overloadable); functions may
        // overload when enabled.
        auto unique = decls.size() - funcs;
        bool conflict = unique > 1
            || (unique >= 1 && funcs >= 1)
            || (funcs > 1 && !allowOverloads);
        if (conflict) {
            const auto& a = decls[0];
            const auto& b = decls[1];
            return TError(b.Location, "имя `" + name + "' объявлено повторно: "
                + a.Label + ":" + Loc(a.Location) + " и " + b.Label + ":" + Loc(b.Location));
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<TComposeResult, TError> Compose(
    const std::vector<const TSourceModule*>& modules,
    const TExprPtr& mainAst,
    const std::vector<TPragma>& mainPragmas,
    const std::string& mainLabel,
    bool cloneSourceModules)
{
    auto mainBlock = TMaybeNode<TBlockExpr>(mainAst);
    if (!mainBlock) {
        return std::unexpected(TError(mainAst ? mainAst->Location : TLocation{},
            "корневое выражение программы должно быть блоком"));
    }

    std::vector<TUnit> units;
    for (const auto* m : modules) {
        units.push_back({&TMaybeNode<TBlockExpr>(m->Ast).Cast()->Stmts, m->Path.string(), m->Name, false});
    }
    units.push_back({&mainBlock.Cast()->Stmts, mainLabel, {}, true});

    auto pragmas = MergePragmas(modules, mainPragmas, mainLabel);
    if (!pragmas) {
        return std::unexpected(pragmas.error());
    }
    bool allowOverloads = AllowsOverloads(*pragmas);

    if (auto err = CheckConflicts(units, allowOverloads)) {
        return std::unexpected(*err);
    }

    std::unordered_set<std::string> moduleNames;
    for (const auto* m : modules) {
        moduleNames.insert(m->Name);
    }

    // Kumir's entry point is the first function in the program (any name, no
    // args - see TModule::GetEntryPoint). The main program's functions must
    // therefore precede module functions so the entry stays first.
    std::vector<TExprPtr> uses, types, globals, mainFunctions, moduleFunctions, other;
    std::unordered_set<std::string> seenUses;
    for (const auto& unit : units) {
        for (const auto& sourceStmt : *unit.Stmts) {
            auto stmt = !unit.IsMain && cloneSourceModules
                ? DeepCloneExpr(sourceStmt)
                : sourceStmt;
            if (!unit.IsMain) {
                stmt->Origin = unit.Origin;
            }
            if (auto use = TMaybeNode<TUseExpr>(stmt)) {
                auto useNode = use.Cast();
                const auto& name = useNode->ModuleName;
                if (moduleNames.contains(name)) {
                    useNode->Resolved = true;
                }
                if (seenUses.insert(name).second) {
                    uses.push_back(stmt);
                }
            } else if (TMaybeNode<TTypeDeclStmt>(stmt)) {
                types.push_back(stmt);
            } else if (TMaybeNode<TFunDecl>(stmt)) {
                (unit.IsMain ? mainFunctions : moduleFunctions).push_back(stmt);
            } else if (IsGlobal(stmt)) {
                // Modules precede main in `units`, so globals are initialized in
                // dependency order (dependencies first, main last); destruction
                // runs in reverse.
                globals.push_back(stmt);
            } else if (unit.IsMain) {
                other.push_back(stmt);
            }
        }
    }

    std::vector<TExprPtr> stmts;
    stmts.reserve(uses.size() + types.size() + globals.size()
        + mainFunctions.size() + moduleFunctions.size() + other.size());
    for (auto* group : {&uses, &types, &globals, &mainFunctions, &moduleFunctions, &other}) {
        stmts.insert(stmts.end(), group->begin(), group->end());
    }

    std::vector<std::string> llvmBitcode;
    for (const auto* module : modules) {
        llvmBitcode.insert(
            llvmBitcode.end(), module->LlvmBitcode.begin(), module->LlvmBitcode.end());
    }

    return TComposeResult{
        MakeIntrusive<TBlockExpr>(mainAst->Location, std::move(stmts)),
        std::move(*pragmas),
        std::move(llvmBitcode),
    };
}

std::expected<TComposeResult, TError> LoadAndCompose(
    TSourceModuleLoader& loader,
    const TExprPtr& mainAst,
    const std::vector<TPragma>& corePragmas,
    bool cloneSourceModules)
{
    if (auto block = TMaybeNode<TBlockExpr>(mainAst)) {
        for (const auto& stmt : block.Cast()->Stmts) {
            auto use = TMaybeNode<TUseExpr>(stmt);
            if (use && loader.Resolvable(use.Cast()->ModuleName)) {
                if (auto loaded = loader.Load(use.Cast()->ModuleName); !loaded) {
                    return std::unexpected(loaded.error());
                }
            }
        }
    }

    auto modules = loader.TopologicalOrder();
    if (modules.empty()) {
        return TComposeResult{mainAst, corePragmas, {}};
    }
    return Compose(
        modules, mainAst, corePragmas, "<main>", cloneSourceModules);
}

std::expected<TComposeResult, TError> LoadAndCompose(
    const TExprPtr& mainAst,
    const std::vector<TPragma>& corePragmas,
    const std::vector<std::string>& searchPaths,
    const std::vector<std::string>& explicitModules)
{
    TSourceModuleLoader loader;
    for (const auto& dir : searchPaths) {
        loader.AddSearchPath(dir);
    }
    for (const auto& file : explicitModules) {
        if (auto reg = loader.RegisterSourceModule(file); !reg) {
            return std::unexpected(reg.error());
        }
    }
    return LoadAndCompose(loader, mainAst, corePragmas);
}

} // namespace NFrontend
} // namespace NQumir
