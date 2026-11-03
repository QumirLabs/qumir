#pragma once

#include <qumir/parser/ast.h>
#include <qumir/location.h>
#include <qumir/error.h>
#include <qumir/optional.h>
#include <qumir/module_manager.h>

#include <expected>
#include <map>
#include <span>
#include <tuple>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace NQumir {

namespace NRegistry {

class IModule;

} // namespace NRegistry

namespace NSemantics {

struct TScopeId {
    int32_t Id;
};

struct TSymbolId {
    int32_t Id;
};

struct TSymbolInfo {
    int32_t Id;
    int32_t DeclScopeId;
    int32_t ScopeLevelIdx;
    int32_t FunctionLevelIdx;
    int32_t FuncScopeId;
};

struct TSymbol {
    TSymbolId Id {-1};
    TScopeId ScopeId {-1};
    int32_t ScopeLevelIdx {-1};
    int32_t FunctionLevelIdx {-1}; // index among function-local symbols, -1 if not in function scope
    TScopeId FuncScopeId {-1}; // function scope id, -1 if not in function scope
    std::string Name;
    std::vector<uint32_t> CodePoints;
    NAst::TExprPtr Node;
};

struct TSuggestion {
    std::string OriginalName;
    std::string Name;
    std::optional<std::string> RequiredModuleName;
    int Distance;

    std::string ToString() const {
        if (Distance == 0 && RequiredModuleName) {
            return "\n `" + Name + "' - команда модуля `" + *RequiredModuleName + "',\n чтобы её использовать, добавьте строку `использовать " + *RequiredModuleName + "' в начало программы.";
        }
        std::string result = "\n Возможно вы имели в виду `" + Name + "'";;
        if (RequiredModuleName) {
            result += " из модуля `" + *RequiredModuleName + "',\n добавьте строку `использовать " + *RequiredModuleName + "' в начало программы и замените `" + OriginalName + "' на `" + Name + "'.";
        }
        return result;
    }
};

struct TNameResolverOptions {
    bool AllowOverloads = false;
};

struct TScope;
using TScopePtr = std::shared_ptr<TScope>;

struct TScope {
    TScopeId Id;
    TScopePtr Parent;
    TScopePtr FuncScope;
    std::unordered_set<int32_t> Symbols;
    std::unordered_set<int32_t> FuncSymbols; // if scope is function scope - all local symbols of function
    std::unordered_map<std::string, TSymbolId> NameToSymbolId;
    // Functions with multiple overloads: canonical name -> list of symbol IDs (each under a synthName).
    std::unordered_map<std::string, std::vector<TSymbolId>> OverloadSets;
    std::unordered_set<std::string> GenericTypeParams;
    std::unordered_map<std::string, NAst::TTypePtr> GenericValueParams;
    bool AllowsRedeclare{false};
    bool RootLevel{false};
    // Set on the function-level scope (the one created for the function's
    // params, see TFunDecl::Scope) by AnnotateFunDecl. For coroutines this is
    // the unwrapped Future<T> result type. Looked up via FuncScope by
    // AnnotateReturn for any (return ...) inside the function body.
    NAst::TTypePtr RetType;
};

class TEditDistance {
public:
    template<typename T>
    int Calc(std::span<const T> a, std::span<const T> b) {
        int n = a.size();
        int m = b.size();
        DP.resize((n + 1) * (m + 1), 0);

        for (int i = 0; i <= n; i++) {
            DP[i * (m + 1) + 0] = i;
        }
        for (int j = 0; j <= m; j++) {
            DP[0 * (m + 1) + j] = j;
        }

        for (int i = 1; i <= n; i++) {
            for (int j = 1; j <= m; j++) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                DP[i * (m + 1) + j] = std::min(
                    DP[(i - 1) * (m + 1) + j] + 1, // deletion
                    std::min(
                        DP[i * (m + 1) + (j - 1)] + 1, // insertion
                        DP[(i - 1) * (m + 1) + (j - 1)] + cost // substitution
                    )
                );
            }
        }

        return DP[n * (m + 1) + m];
    }

private:
    std::vector<int> DP;
};

class TNameResolver : public IModuleManager {
public:
    TNameResolver(const TNameResolverOptions& options = {});

    std::optional<TError> Resolve(NAst::TExprPtr root);
    void ApplyPragmas(const std::vector<NAst::TPragma>& pragmas) override;
    TScopePtr GetOrCreateRootScope();

    // Used by the type annotator to read/write per-scope state (e.g. TScope::RetType).
    TScopePtr GetScope(TScopeId id) const { return Scopes.at(id.Id); }

    std::optional<TSymbolInfo> Lookup(const std::string& name, TScopeId scope) const;
    // Returns all overloads for name. If non-overloaded single function, returns {id}. Empty if not found.
    std::vector<TSymbolId> LookupOverloads(const std::string& name, TScopeId scope) const;
    std::optional<TSuggestion> Suggest(const std::string& name, TScopeId scope, bool includeFunctions);
    // Modules, not imported yet, that have a command `name` taking `argCount` arguments.
    std::vector<std::string> ModulesWithFunction(const std::string& name, size_t argCount) const;

    TSymbolId DeclareFunction(const std::string& name, NAst::TExprPtr node);
    NAst::TExprPtr GetSymbolNode(TSymbolId id) const;
    std::vector<std::pair<int, std::shared_ptr<NAst::TFunDecl>>> GetExternalFunctions();

    // A clone needs fresh scopes: the generic declaration's symbols refer to
    // its original AST nodes.
    std::expected<TSymbolId, TError> ResolveInstantiatedFunDecl(
        std::shared_ptr<NAst::TFunDecl> fdecl,
        bool publish = true);

    // Includes instantiations triggered while resolving earlier clones.
    const std::vector<std::shared_ptr<NAst::TFunDecl>>& GetGenericInstantiations() const {
        return GenericInstantiations;
    }

    // Annotation runs again, so drain clones before they are appended twice.
    std::vector<std::shared_ptr<NAst::TFunDecl>> TakeGenericInstantiations() {
        return std::exchange(GenericInstantiations, {});
    }

    TSymbolId Declare(const std::string& name, NAst::TExprPtr node, TSymbolInfo parentSymbol);

    // just adds module to dict of modules
    void RegisterModule(NRegistry::IModule* module);
    // Registers a frontend module alias (e.g. Kumir "Файлы" -> "System"). The
    // canonical module must already be registered. Imports and the module list
    // resolve the alias transparently.
    void RegisterModuleAlias(const std::string& alias, const std::string& canonical);
    // IModuleManager: imports module symbols; returns module pointer on success,
    // error message on failure (unknown module or name conflict between modules).
    std::expected<bool, std::string> ImportModule(const std::string& name) override;
    std::vector<std::string> GetAllImportedTypeNames() const override;
    std::vector<NRegistry::TLiteralSuffix> GetAllImportedLiteralSuffixes() const override;
    std::string ModulesList() const;
    // Lists the commands of the imported Робот module, empty if it is not imported.
    std::string RobotCommandsHint() const;
    std::optional<std::string> ClosestModuleName(const std::string& name);

    // For testing/debugging
    const std::vector<TSymbol>& GetSymbols() const {
        return Symbols;
    }

    std::vector<TSymbolInfo> GetGlobals() const {
        if (Scopes.empty()) {
            return {};
        }
        auto& rootScope = Scopes[0];
        std::vector<TSymbolInfo> result;
        for (auto& symbolId : rootScope->Symbols) {
            auto& symbol = Symbols[symbolId];
            //std::cerr << "Global symbol: " << symbol.Name << " " << symbol.Id.Id << "\n";
            result.push_back(TSymbolInfo {
                .Id = symbol.Id.Id,
                .DeclScopeId = symbol.ScopeId.Id,
                .ScopeLevelIdx = symbol.ScopeLevelIdx,
                .FunctionLevelIdx = symbol.FunctionLevelIdx,
                .FuncScopeId = symbol.FuncScopeId.Id,
            });
        }
        return result;
    }

    void PrintSymbols(std::ostream& os) const;

    // Returns synthetic function name for cast from->to if registered by an imported module, nullopt otherwise
    std::optional<std::string> GetCast(const NAst::TTypePtr& from, const NAst::TTypePtr& to) const;

    struct TRegisteredOp {
        std::string SynthName;
        NAst::TTypePtr ReturnType;
    };
    // Exact match only; two-level (cast) logic lives in the type annotator
    std::optional<TRegisteredOp> GetBinaryOp(const std::string& op,
        const NAst::TTypePtr& left, const NAst::TTypePtr& right) const;
    std::optional<TRegisteredOp> GetUnaryOp(const std::string& op,
        const NAst::TTypePtr& operand) const;
    void RegisterUnaryOp(const std::string& op, const NAst::TTypePtr& operand, TRegisteredOp target);
    std::vector<std::shared_ptr<NAst::TFunDecl>> LookupGenericOperatorDecls(
        const std::string& op,
        std::size_t arity) const;

    NAst::TTypePtr LookupType(const std::string& name) const override;
    void RegisterType(const std::string& name, NAst::TTypePtr type);
    void RegisterImportedType(const std::string& name, NAst::TTypePtr type) override {
        RegisterType(name, std::move(type));
    }

private:
    struct TRegisteredTypeDecl {
        NAst::TTypePtr UnderlyingType;
        std::vector<NAst::TGenericParam> GenericParams;
    };

    using TTask = TExpectedTask<std::monostate, TError, TLocation>;
    TTask Resolve(NAst::TExprPtr node, TScopePtr parentScope, TScopePtr funcScope);
    TTask ResolveTopFuncDecl(NAst::TExprPtr node, TScopePtr scope);
    std::optional<TError> ResolveTypeRef(NAst::TTypePtr& type, const TLocation& loc, TScopePtr typeScope);
    std::optional<TError> RegisterTypeDecls(const NAst::TExprPtr& root);
    // Registers top-level functions carrying an (operator "X") attribute into
    // the type-keyed op/cast dispatch tables, with the function's own name as
    // the call target. Mirrors how ImportModule lifts runtime IsOp functions.
    std::optional<TError> RegisterOperatorDecls(const NAst::TExprPtr& root);
    void ImportUseStmts(const NAst::TExprPtr& root);
    std::expected<TSymbolId, TError> Declare(const std::string& name, NAst::TExprPtr node, TScopePtr scope, TScopePtr funcScope);
    TScopePtr NewScope(TScopePtr parent, TScopePtr funcScope);

    // Returns true if two TFunDecl have identical parameter type signatures.
    static bool ParamTypesSame(const NAst::TFunDecl& a, const NAst::TFunDecl& b);

    void RegisterTypeDecl(
        const std::string& name,
        NAst::TTypePtr underlying,
        std::vector<NAst::TGenericParam> genericParams);

    // Registers one TFunDecl into an existing overload set under a generated synthName.
    // Returns an error if param types match an already-registered overload.
    std::expected<TSymbolId, TError> RegisterOverloadEntry(
        const std::string& canonicalName,
        NAst::TExprPtr node,
        std::vector<TSymbolId>& overloads);

    // Called on the first name collision between two TFunDecl.
    // Moves both into a new overload set, erases the canonical name from NameToSymbolId.
    std::expected<TSymbolId, TError> StartOverloadSet(
        const std::string& name,
        TSymbolId existingSymId,
        NAst::TExprPtr newNode,
        TScopePtr scope);

    TNameResolverOptions Options;
    std::vector<TSymbol> Symbols;
    std::vector<std::shared_ptr<NAst::TFunDecl>> GenericInstantiations;

    std::vector<TScopePtr> Scopes;

    std::unordered_map<std::string, NRegistry::IModule*> Modules;
    std::unordered_map<std::string, std::string> ModuleAliases; // frontend alias -> canonical module name
    std::unordered_set<std::string> ImportedModules;        // modules already imported (for idempotency)
    std::unordered_map<std::string, std::string> ImportedModuleSymbols; // symbol/type name -> source module
    std::unordered_map<std::string, NAst::TTypePtr> ImportedTypes; // type name -> resolved type
    std::unordered_map<std::string, TRegisteredTypeDecl> RegisteredTypes;
    std::vector<std::shared_ptr<NAst::TFunDecl>> ImportedOperators; // operator overloads (IsOp=true), may have duplicates
    std::vector<std::shared_ptr<NAst::TFunDecl>> GenericOperatorDecls;

    // cast operator map: {TypeKey(from), TypeKey(to)} -> synthetic name registered via DeclareFunction
    std::map<std::pair<std::string,std::string>, std::string> ImportedCasts;
    // {op, TypeKey(left), TypeKey(right)} -> registered op
    std::map<std::tuple<std::string,std::string,std::string>, TRegisteredOp> ImportedBinaryOps;
    // {op, TypeKey(operand)} -> registered op
    std::map<std::pair<std::string,std::string>, TRegisteredOp> ImportedUnaryOps;


    TEditDistance EditDistanceCalculator;
};

} // namespace NSemantics
} // namespace NQumir
