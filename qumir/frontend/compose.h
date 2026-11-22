#pragma once

#include <qumir/error.h>
#include <qumir/frontend/source_module.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/pragma.h>

#include <expected>
#include <string>
#include <vector>

namespace NQumir {
namespace NFrontend {

struct TComposeResult {
    NAst::TExprPtr Ast;
    std::vector<NAst::TPragma> Pragmas;
    std::vector<std::string> LlvmBitcode;
};

// Builds a single compilation unit from the imported source modules and the
// main program. `modules` is in dependency-first order; the main program is
// appended last. The result root block orders declarations as the IR lowering
// requires: external (runtime) `use`s, then types, then globals, then
// functions. `use`s that name a composed source module are dropped - those
// modules are inlined directly. Pragmas of all units are merged (conflicting
// values for the same group is an error), and duplicate exported names are
// reported with their originating files before name resolution runs. Set
// `cloneSourceModules` when the cached parsed modules will be sent through an
// independent, mutating semantic pipeline more than once.
std::expected<TComposeResult, TError> Compose(
    const std::vector<const TSourceModule*>& modules,
    const NAst::TExprPtr& mainAst,
    const std::vector<NAst::TPragma>& mainPragmas,
    const std::string& mainLabel,
    bool cloneSourceModules = false);

class TSourceModuleLoader;

// Frontend entry shared by runners: loads the `.oz` source modules referenced
// by `use` in the main program and composes them with the main AST. When no
// `use` resolves to a source module, returns the main AST unchanged with
// `corePragmas`. The loader overload reuses an already-configured loader (e.g.
// one consulted during parsing) instead of building a fresh one.
std::expected<TComposeResult, TError> LoadAndCompose(
    TSourceModuleLoader& loader,
    const NAst::TExprPtr& mainAst,
    const std::vector<NAst::TPragma>& corePragmas,
    bool cloneSourceModules = false);

std::expected<TComposeResult, TError> LoadAndCompose(
    NAst::TTypeFactory& types,
    const NAst::TExprPtr& mainAst,
    const std::vector<NAst::TPragma>& corePragmas,
    const std::vector<std::string>& searchPaths,
    const std::vector<std::string>& explicitModules = {});

} // namespace NFrontend
} // namespace NQumir
