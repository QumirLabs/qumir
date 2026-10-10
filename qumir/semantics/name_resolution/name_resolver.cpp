#include "name_resolver.h"
#include <qumir/semantics/vector_intrinsics.h>

#include <qumir/modules/module.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <iostream>

namespace NQumir {
namespace NSemantics {

using namespace NAst;

namespace {

TTypePtr CopyTypeAttrs(TTypePtr result, const TTypePtr& source) {
    if (result && source) {
        static_cast<TType&>(*result) = static_cast<const TType&>(*source);
    }
    return result;
}

TTypePtr CloneTypeWithGenericBindings(
    const TTypePtr& type,
    const std::unordered_set<std::string>& genericTypeParams,
    const std::map<std::string, TTypePtr>& bindings,
    const std::unordered_set<std::string>& genericValueParams,
    const std::map<std::string, std::string>& valueBindings);

std::vector<TGenericArg> CloneGenericArgsWithBindings(
    const std::vector<TGenericArg>& args,
    const std::unordered_set<std::string>& genericTypeParams,
    const std::map<std::string, TTypePtr>& bindings,
    const std::unordered_set<std::string>& genericValueParams,
    const std::map<std::string, std::string>& valueBindings)
{
    std::vector<TGenericArg> result;
    result.reserve(args.size());
    for (const auto& arg : args) {
        if (arg.Kind == TGenericArg::EKind::Type) {
            result.push_back(TGenericArg::TypeArg(
                CloneTypeWithGenericBindings(arg.Type, genericTypeParams, bindings, genericValueParams, valueBindings)));
        } else {
            auto it = valueBindings.find(arg.Value);
            result.push_back(TGenericArg::ValueArg(
                it != valueBindings.end() && genericValueParams.contains(arg.Value)
                    ? it->second
                    : arg.Value));
        }
    }
    return result;
}

TTypePtr CloneTypeWithGenericBindings(
    const TTypePtr& type,
    const std::unordered_set<std::string>& genericTypeParams,
    const std::map<std::string, TTypePtr>& bindings,
    const std::unordered_set<std::string>& genericValueParams,
    const std::map<std::string, std::string>& valueBindings)
{
    if (!type) {
        return type;
    }
    if (auto t = TMaybeType<TIntegerType>(type)) {
        return CopyTypeAttrs(std::make_shared<TIntegerType>(t.Cast()->Kind), type);
    }
    if (TMaybeType<TFloatType>(type)) {
        return CopyTypeAttrs(std::make_shared<TFloatType>(), type);
    }
    if (TMaybeType<TBoolType>(type)) {
        return CopyTypeAttrs(std::make_shared<TBoolType>(), type);
    }
    if (TMaybeType<TStringType>(type)) {
        return CopyTypeAttrs(std::make_shared<TStringType>(), type);
    }
    if (TMaybeType<TSymbolType>(type)) {
        return CopyTypeAttrs(std::make_shared<TSymbolType>(), type);
    }
    if (TMaybeType<TVoidType>(type)) {
        return CopyTypeAttrs(std::make_shared<TVoidType>(), type);
    }
    if (auto t = TMaybeType<TFunctionType>(type)) {
        auto src = t.Cast();
        std::vector<TTypePtr> params;
        params.reserve(src->ParamTypes.size());
        for (const auto& param : src->ParamTypes) {
            params.push_back(CloneTypeWithGenericBindings(param, genericTypeParams, bindings, genericValueParams, valueBindings));
        }
        auto ret = CloneTypeWithGenericBindings(src->ReturnType, genericTypeParams, bindings, genericValueParams, valueBindings);
        return CopyTypeAttrs(std::make_shared<TFunctionType>(std::move(params), std::move(ret)), type);
    }
    if (auto t = TMaybeType<TFutureType>(type)) {
        return CopyTypeAttrs(
            std::make_shared<TFutureType>(CloneTypeWithGenericBindings(t.Cast()->ResultType, genericTypeParams, bindings, genericValueParams, valueBindings)),
            type);
    }
    if (auto t = TMaybeType<TArrayType>(type)) {
        auto src = t.Cast();
        return CopyTypeAttrs(
            std::make_shared<TArrayType>(
                CloneTypeWithGenericBindings(src->ElementType, genericTypeParams, bindings, genericValueParams, valueBindings),
                src->Arity),
            type);
    }
    if (auto t = TMaybeType<TVectorType>(type)) {
        auto result = std::make_shared<TVectorType>(*t.Cast());
        result->ElementType = CloneTypeWithGenericBindings(
            result->ElementType, genericTypeParams, bindings, genericValueParams, valueBindings);
        if (auto it = valueBindings.find(result->SizeParam); it != valueBindings.end()) {
            result->BindSize(it->second);
        }
        return result;
    }
    if (auto t = TMaybeType<TPointerType>(type)) {
        return CopyTypeAttrs(
            std::make_shared<TPointerType>(CloneTypeWithGenericBindings(t.Cast()->PointeeType, genericTypeParams, bindings, genericValueParams, valueBindings)),
            type);
    }
    if (auto t = TMaybeType<TReferenceType>(type)) {
        return CopyTypeAttrs(
            std::make_shared<TReferenceType>(CloneTypeWithGenericBindings(t.Cast()->ReferencedType, genericTypeParams, bindings, genericValueParams, valueBindings)),
            type);
    }
    if (auto t = TMaybeType<TNamedType>(type)) {
        auto src = t.Cast();
        if (src->TypeArgs.empty() && genericTypeParams.contains(src->Name)) {
            auto it = bindings.find(src->Name);
            if (it != bindings.end()) {
                return CopyTypeAttrs(CloneTypeWithGenericBindings(it->second, {}, {}, {}, {}), type);
            }
        }
        auto result = std::make_shared<TNamedType>(
            src->Name,
            CloneTypeWithGenericBindings(src->UnderlyingType, genericTypeParams, bindings, genericValueParams, valueBindings),
            CloneGenericArgsWithBindings(src->TypeArgs, genericTypeParams, bindings, genericValueParams, valueBindings));
        result->Reference = src->Reference;
        return CopyTypeAttrs(std::move(result), type);
    }
    if (auto t = TMaybeType<TStructType>(type)) {
        std::vector<std::pair<std::string, TTypePtr>> fields;
        fields.reserve(t.Cast()->Fields.size());
        for (const auto& [name, fieldType] : t.Cast()->Fields) {
            fields.emplace_back(name, CloneTypeWithGenericBindings(fieldType, genericTypeParams, bindings, genericValueParams, valueBindings));
        }
        return CopyTypeAttrs(std::make_shared<TStructType>(std::move(fields)), type);
    }
    return type;
}

bool IsGenericTypeParam(const std::string& name, TScopePtr typeScope) {
    while (typeScope) {
        if (typeScope->GenericTypeParams.contains(name)) {
            return true;
        }
        typeScope = typeScope->Parent;
    }
    return false;
}

bool IsGenericValueParam(const std::string& name, TScopePtr typeScope) {
    while (typeScope) {
        if (typeScope->GenericValueParams.contains(name)) {
            return true;
        }
        typeScope = typeScope->Parent;
    }
    return false;
}

std::optional<std::string> ValueArgFromTypeArg(const TGenericArg& arg, TScopePtr typeScope) {
    if (arg.Kind == TGenericArg::EKind::Value) {
        return arg.Value;
    }
    auto named = TMaybeType<TNamedType>(arg.Type);
    if (named && named.Cast()->TypeArgs.empty() && IsGenericValueParam(named.Cast()->Name, typeScope)) {
        return named.Cast()->Name;
    }
    return std::nullopt;
}

bool IsSupportedValueParamType(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TIntegerType>(type));
}

std::optional<TError> ValidateGenericParams(
    const std::vector<TGenericParam>& params,
    const TLocation& loc)
{
    std::unordered_set<std::string> names;
    for (const auto& param : params) {
        if (!names.insert(param.Name).second) {
            return TError(loc, "duplicate generic parameter: " + param.Name);
        }
        if (param.Kind == TGenericParam::EKind::Value && !IsSupportedValueParamType(param.ValueType)) {
            return TError(loc, "Only integer generic value parameters are supported: " + param.Name);
        }
    }
    return std::nullopt;
}

void AddGenericParamsToScope(const std::vector<TGenericParam>& params, const TScopePtr& scope) {
    for (const auto& param : params) {
        if (param.Kind == TGenericParam::EKind::Type) {
            scope->GenericTypeParams.insert(param.Name);
        } else {
            scope->GenericValueParams[param.Name] = param.ValueType;
        }
    }
}

struct TGenericParamAliases {
    std::unordered_map<std::string, std::string> Types;
    std::unordered_map<std::string, std::string> Values;
};

TGenericParamAliases GenericParamAliases(const TFunDecl& decl) {
    TGenericParamAliases aliases;
    int nextType = 0;
    int nextValue = 0;
    for (const auto& param : decl.GenericParams) {
        if (param.Kind == TGenericParam::EKind::Type && !aliases.Types.contains(param.Name)) {
            aliases.Types[param.Name] = "T" + std::to_string(nextType++);
        } else if (param.Kind == TGenericParam::EKind::Value && !aliases.Values.contains(param.Name)) {
            aliases.Values[param.Name] = "V" + std::to_string(nextValue++);
        }
    }
    return aliases;
}

std::string OverloadTypeKey(
    const TTypePtr& type,
    const TGenericParamAliases& genericAliases)
{
    if (!type) return "unknown";
    if (auto integer = TMaybeType<TIntegerType>(type)) return integer.Cast()->ToString();
    if (TMaybeType<TFloatType>(type)) return "f64";
    if (TMaybeType<TBoolType>(type)) return "bool";
    if (TMaybeType<TStringType>(type)) return "string";
    if (TMaybeType<TSymbolType>(type)) return "char";
    if (TMaybeType<TVoidType>(type)) return "void";
    if (auto named = TMaybeType<TNamedType>(type)) {
        auto src = named.Cast();
        auto alias = genericAliases.Types.find(src->Name);
        std::string value = alias != genericAliases.Types.end()
            ? alias->second
            : src->Name;
        for (const auto& arg : src->TypeArgs) {
            value += "_";
            if (arg.Kind == TGenericArg::EKind::Type) {
                value += OverloadTypeKey(arg.Type, genericAliases);
            } else {
                auto valueAlias = genericAliases.Values.find(arg.Value);
                value += valueAlias != genericAliases.Values.end()
                    ? valueAlias->second
                    : "Value_" + arg.Value;
            }
        }
        return value;
    }
    if (auto future = TMaybeType<TFutureType>(type)) {
        return std::string("Future_") + OverloadTypeKey(future.Cast()->ResultType, genericAliases);
    }
    if (auto array = TMaybeType<TArrayType>(type)) {
        return "Array" + std::to_string(array.Cast()->Arity) + "_"
            + OverloadTypeKey(array.Cast()->ElementType, genericAliases);
    }
    if (auto pointer = TMaybeType<TPointerType>(type)) {
        return std::string("Ptr_") + OverloadTypeKey(pointer.Cast()->PointeeType, genericAliases);
    }
    if (auto vector = TMaybeType<TVectorType>(type)) {
        auto src = vector.Cast();
        auto alias = genericAliases.Values.find(src->SizeParam);
        const std::string size = alias != genericAliases.Values.end()
            ? alias->second
            : src->SizeText();
        return "Vector_" + OverloadTypeKey(src->ElementType, genericAliases) + "_" + size;
    }
    if (auto reference = TMaybeType<TReferenceType>(type)) {
        return std::string("Ref_") + OverloadTypeKey(reference.Cast()->ReferencedType, genericAliases);
    }
    if (auto function = TMaybeType<TFunctionType>(type)) {
        std::string key = "Fun_";
        key += OverloadTypeKey(function.Cast()->ReturnType, genericAliases);
        key += "__";
        for (const auto& paramType : function.Cast()->ParamTypes) {
            key += "_" + OverloadTypeKey(paramType, genericAliases);
        }
        return key;
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::string key = "Struct";
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            key += "_" + OverloadTypeKey(fieldType, genericAliases);
        }
        return key;
    }
    return std::string(type->TypeName());
}

// Signature-based, order-independent symbol name; uniqueness matches ParamTypesSame.
std::string OverloadMangledName(const std::string& canonicalName, const TFunDecl& decl) {
    std::string name = "__overload_" + canonicalName + "$";
    auto aliases = GenericParamAliases(decl);
    name += OverloadTypeKey(decl.RetType, aliases);
    name += "__";
    for (const auto& param : decl.Params) {
        name += "$";
        name += OverloadTypeKey(param->Type, aliases);
    }
    return name;
}

} // namespace

TNameResolver::TNameResolver(const TNameResolverOptions& options)
    : Options(options)
{ }

void TNameResolver::ApplyPragmas(const std::vector<NAst::TPragma>& pragmas) {
    for (const auto& pragma : pragmas) {
        if (pragma.Group == "language") {
            for (const auto& value : pragma.Values) {
                if (value == "overloads") {
                    Options.AllowOverloads = true;
                }
            }
        }
    }
}

// Function declaration may be at the top level only
// They can be in any order, so we need to do two passes
TNameResolver::TTask TNameResolver::ResolveTopFuncDecl(NAst::TExprPtr node, TScopePtr scope)
{
    auto declFunc = [&](NAst::TExprPtr node) -> TTask {
        if (auto maybeFdecl = TMaybeNode<TFunDecl>(node)) {
            auto fdecl = maybeFdecl.Cast();
            if (fdecl->Name.empty()) {
                co_return TError(fdecl->Location, "function with empty name");
            }
            if (auto err = ValidateGenericParams(fdecl->GenericParams, fdecl->Location)) {
                co_return *err;
            }
            if (fdecl->Scope >= 0) {
                if (static_cast<size_t>(fdecl->Scope) >= Scopes.size()) {
                    co_return TError(fdecl->Location, "function has invalid scope id: " + std::to_string(fdecl->Scope));
                }
                co_return std::monostate{};
            }
            co_await Declare(fdecl->Name, node, scope, nullptr);
            auto newScope = NewScope(scope, nullptr);
            fdecl->Scope = newScope->Id.Id;
            AddGenericParamsToScope(fdecl->GenericParams, newScope);
            for (auto& param : fdecl->Params) {
                co_await Declare(param->Name, param, newScope, newScope);
            }
            // resolve body on second pass
        }
        co_return std::monostate{};
    };

    auto maybeBlock = TMaybeNode<TBlockExpr>(node);
    if (maybeBlock) {
        auto block = maybeBlock.Cast();
        for (const auto& stmt : block->Stmts) {
            co_await declFunc(stmt);
        }
    }

    co_return {};
}

std::optional<TError> TNameResolver::ResolveTypeRef(TTypePtr& type, const TLocation& loc, TScopePtr typeScope) {
    auto resolveTypeRef = [&](auto& self, NAst::TTypePtr& type, const TLocation& loc, TScopePtr typeScope) -> std::optional<TError> {
        if (!type) return {};
        if (auto maybeNamed = TMaybeType<TNamedType>(type)) {
            auto named = maybeNamed.Cast();
            if (IsGenericTypeParam(named->Name, typeScope)) {
                if (!named->TypeArgs.empty()) {
                    return TError(loc, "Generic type parameter '" + named->Name + "' does not accept parameters");
                }
                return {};
            }
            if (IsGenericValueParam(named->Name, typeScope)) {
                return TError(loc, "Generic value parameter '" + named->Name + "' cannot be used as a type");
            }
            auto it = RegisteredTypes.find(named->Name);
            if (it == RegisteredTypes.end()) {
                return TError(loc, "Неизвестный тип: " + named->Name);
            }
            const auto& typeDecl = it->second;
            if (auto n = TMaybeType<TNamedType>(typeDecl.UnderlyingType); n && n.Cast()->Name == named->Name) {
                throw std::runtime_error("resolveTypeRef: RegisteredTypes['" + named->Name
                    + "'] is a TNamedType with the same name — double wrap would result");
            }
            if (typeDecl.GenericParams.empty()) {
                if (!named->TypeArgs.empty()) {
                    return TError(loc, "Тип '" + named->Name + "' не принимает параметры");
                }
                named->UnderlyingType = typeDecl.UnderlyingType;
            } else {
                if (named->TypeArgs.size() != typeDecl.GenericParams.size()) {
                    return TError(loc, "Неверное число параметров типа '" + named->Name + "': ожидается "
                        + std::to_string(typeDecl.GenericParams.size()) + ", получено "
                        + std::to_string(named->TypeArgs.size()));
                }
                std::unordered_set<std::string> genericTypeParams;
                std::unordered_set<std::string> genericValueParams;
                std::map<std::string, TTypePtr> bindings;
                std::map<std::string, std::string> valueBindings;
                std::vector<TGenericArg> resolvedArgs;
                resolvedArgs.reserve(named->TypeArgs.size());
                for (size_t i = 0; i < typeDecl.GenericParams.size(); ++i) {
                    const auto& param = typeDecl.GenericParams[i];
                    auto arg = named->TypeArgs[i];
                    if (param.Kind == TGenericParam::EKind::Type) {
                        if (arg.Kind != TGenericArg::EKind::Type) {
                            return TError(loc, "Generic type parameter '" + param.Name + "' expects a type argument");
                        }
                        if (auto err = self(self, arg.Type, loc, typeScope)) {
                            return err;
                        }
                        genericTypeParams.insert(param.Name);
                        bindings[param.Name] = arg.Type;
                        resolvedArgs.push_back(std::move(arg));
                    } else {
                        if (!IsSupportedValueParamType(param.ValueType)) {
                            return TError(loc, "Only integer generic value parameters are supported: " + param.Name);
                        }
                        auto value = ValueArgFromTypeArg(arg, typeScope);
                        if (!value) {
                            return TError(loc, "Generic value parameter '" + param.Name + "' expects an integer value argument");
                        }
                        genericValueParams.insert(param.Name);
                        valueBindings[param.Name] = *value;
                        resolvedArgs.push_back(TGenericArg::ValueArg(std::move(*value)));
                    }
                }
                named->TypeArgs = std::move(resolvedArgs);
                named->UnderlyingType = CloneTypeWithGenericBindings(
                    typeDecl.UnderlyingType,
                    genericTypeParams,
                    bindings,
                    genericValueParams,
                    valueBindings);
            }
            auto modIt = ImportedModuleSymbols.find(named->Name);
            if (modIt != ImportedModuleSymbols.end()) {
                named->Reference = modIt->second; // marks type as imported from this module
            }
            return self(self, named->UnderlyingType, loc, typeScope);
        } else if (auto maybeRef = TMaybeType<TReferenceType>(type)) {
            return self(self, maybeRef.Cast()->ReferencedType, loc, typeScope);
        } else if (auto maybePtr = TMaybeType<TPointerType>(type)) {
            return self(self, maybePtr.Cast()->PointeeType, loc, typeScope);
        } else if (auto maybeArray = TMaybeType<TArrayType>(type)) {
            return self(self, maybeArray.Cast()->ElementType, loc, typeScope);
        } else if (auto vector = TMaybeType<TVectorType>(type)) {
            if (!vector.Cast()->SizeParam.empty() && !IsGenericValueParam(vector.Cast()->SizeParam, typeScope)) {
                return TError(loc, "Vector size '" + vector.Cast()->SizeParam + "' must be a generic value parameter");
            }
            return self(self, vector.Cast()->ElementType, loc, typeScope);
        } else if (auto maybeFun = TMaybeType<TFunctionType>(type)) {
            auto fun = maybeFun.Cast();
            if (auto err = self(self, fun->ReturnType, loc, typeScope)) {
                return err;
            }
            for (auto& param : fun->ParamTypes) {
                if (auto err = self(self, param, loc, typeScope)) {
                    return err;
                }
            }
        } else if (auto maybeFuture = TMaybeType<TFutureType>(type)) {
            return self(self, maybeFuture.Cast()->ResultType, loc, typeScope);
        } else if (auto maybeStruct = TMaybeType<TStructType>(type)) {
            for (auto& [_, fieldType] : maybeStruct.Cast()->Fields) {
                if (auto err = self(self, fieldType, loc, typeScope)) {
                    return err;
                }
            }
        }
        return {};
    };
    return resolveTypeRef(resolveTypeRef, type, loc, typeScope);
}

TNameResolver::TTask TNameResolver::Resolve(TExprPtr node, TScopePtr scope, TScopePtr funcScope) {
    std::list<TError> errors;

    auto suggestionMessage = [&](const std::string& name, int scopeId, bool includeFunctions) {
        auto suggestion = Suggest(name, TScopeId{scopeId}, includeFunctions);
        return suggestion ? suggestion->ToString() : "";
    };

    // TODO: ResolveTypeRef on every node is O(n*depth); consider resolving types
    // only at declaration sites (TVarStmt, TFunDecl params) rather than all nodes.
    if (node->Type && !TMaybeNode<TFunDecl>(node) && !TMaybeNode<TTypeDeclStmt>(node)) {
        if (auto err = ResolveTypeRef(node->Type, node->Location, scope)) {
            co_return *err;
        }
    }

    if (auto maybeFdecl = TMaybeNode<TFunDecl>(node)) {
        auto fdecl = maybeFdecl.Cast();
        auto scopeId = TScopeId{fdecl->Scope};
        if (scopeId.Id < 0 || static_cast<size_t>(scopeId.Id) >= Scopes.size())  {
            co_return TError(fdecl->Location, "function has invalid scope id: " + std::to_string(scopeId.Id));
        }
        auto newScope = Scopes[scopeId.Id];
        if (auto err = ResolveTypeRef(fdecl->Type, fdecl->Location, newScope)) {
            co_return *err;
        }
        if (auto err = ResolveTypeRef(fdecl->RetType, fdecl->Location, newScope)) {
            co_return *err;
        }
        for (auto& param : fdecl->Params) {
            if (auto err = ResolveTypeRef(param->Type, param->Location, newScope)) {
                co_return *err;
            }
        }
        co_await Resolve(fdecl->Body, newScope, newScope);
        if (fdecl->LastAssert) {
            auto bodyScope = Scopes[fdecl->Body->Scope];
            co_await Resolve(fdecl->LastAssert, bodyScope, newScope);
        }
        co_return std::monostate{};
    } else if (auto maybeBlock = TMaybeNode<TBlockExpr>(node)) {
        auto block = maybeBlock.Cast();
        if (block->Scope < 0) {
            scope = NewScope(scope, funcScope);
            block->Scope = scope->Id.Id;
        } else {
            if (static_cast<size_t>(block->Scope) >= Scopes.size()) {
                co_return TError(block->Location, "block has invalid scope id: " + std::to_string(block->Scope));
            }
            scope = Scopes[block->Scope];
        }
    } else if (auto call = TMaybeNode<TCallExpr>(node)) {
        auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee).Cast();
        if (callee && !Lookup(callee->Name, scope->Id) && !VectorIntrinsic(callee->Name).empty()) {
            // Pseudo-calls receive their concrete declarations during annotation.
            for (const auto& arg : call.Cast()->Args) {
                co_await Resolve(arg, scope, funcScope);
            }
            co_return {};
        }
    } else if (auto maybeIdent = TMaybeNode<TIdentExpr>(node)) {
        auto ident = maybeIdent.Cast();
        auto found = Lookup(ident->Name, scope->Id);
        if (!found) {
            if (IsGenericValueParam(ident->Name, scope)) {
                co_return {};
            }
            if (!LookupOverloads(ident->Name, scope->Id).empty()) {
                co_return {};
            }
            auto suggestionMsg = suggestionMessage(ident->Name, scope->Id.Id, /*includeFunctions=*/ true);
            co_return TError(ident->Location, TErrorString::Get<EErrorId::UNDEFINED_IDENTIFIER>(ident->Name) + suggestionMsg);
        }

        co_return {};
    } else if (auto maybeAsg = TMaybeNode<TAssignExpr>(node)) {
        auto asg = maybeAsg.Cast();
        auto found = Lookup(asg->Name, scope->Id);
        if (!found) {
            auto suggestionMsg = suggestionMessage(asg->Name, scope->Id.Id, /*includeFunctions=*/ false);
            errors.emplace_back(TError(asg->Location, TErrorString::Get<EErrorId::UNDEFINED_IDENTIFIER>(asg->Name) + suggestionMsg));
        }
    } else if (auto maybeFor = TMaybeNode<TForStmtExpr>(node)) {
        auto forExpr = maybeFor.Cast();
        auto found = Lookup(forExpr->VarName, scope->Id);
        if (!found) {
            auto suggestionMsg = suggestionMessage(forExpr->VarName, scope->Id.Id, /*includeFunctions=*/ false);
            errors.emplace_back(TError(forExpr->Location, TErrorString::Get<EErrorId::UNDEFINED_IDENTIFIER>(forExpr->VarName) + suggestionMsg));
        }
    } else if (auto maybeVarStmt = TMaybeNode<TVarStmt>(node)) {
        auto varStmt = maybeVarStmt.Cast();
        if (auto err = ResolveTypeRef(varStmt->Type, varStmt->Location, scope)) {
            co_return *err;
        }
        auto existing = scope->NameToSymbolId.find(varStmt->Name);
        if (existing != scope->NameToSymbolId.end()
            && GetSymbolNode(existing->second) == node)
        {
            if (varStmt->Init) {
                auto res = Resolve(varStmt->Init, scope, funcScope).result();
                if (!res) { co_return res.error(); }
            }
            co_return {};
        }
        auto res = Declare(varStmt->Name, node, scope, funcScope);
        if (!res) {
            co_return res.error();
        }
        if (varStmt->Init) {
            auto initRes = Resolve(varStmt->Init, scope, funcScope).result();
            if (!initRes) { co_return initRes.error(); }
        }
        co_return {};
    } else if (auto maybeTypeDecl = TMaybeNode<TTypeDeclStmt>(node)) {
        auto typeDecl = maybeTypeDecl.Cast();
        if (auto maybeNamed = TMaybeType<TNamedType>(typeDecl->Type)) {
            auto named = maybeNamed.Cast();
            if (auto err = ValidateGenericParams(typeDecl->GenericParams, typeDecl->Location)) {
                co_return *err;
            }
            auto typeScope = scope;
            if (!typeDecl->GenericParams.empty()) {
                typeScope = NewScope(scope, funcScope);
                AddGenericParamsToScope(typeDecl->GenericParams, typeScope);
            }
            if (auto err = ResolveTypeRef(named->UnderlyingType, typeDecl->Location, typeScope)) {
                co_return *err;
            }
        }
        co_return {};
    }

    for (const auto& child : node->Children()) {
        if (child == nullptr) continue; // LoopExpr may have null children
        auto res = Resolve(child, scope, funcScope).result();
        if (!res) {
            errors.emplace_back(res.error());
        }
    }

    if (errors.empty()) {
        co_return {};
    }
    co_return TError(node->Location, errors);
}

std::optional<TError> TNameResolver::Resolve(TExprPtr root) {
    auto scope = GetOrCreateRootScope();
    if (auto block = TMaybeNode<TBlockExpr>(root)) {
        block.Cast()->Scope = scope->Id.Id;
    }
    ImportUseStmts(root);
    if (auto err = RegisterTypeDecls(root)) {
        return err;
    }
    if (auto err = RegisterOperatorDecls(root)) {
        return err;
    }
    auto funcRes = ResolveTopFuncDecl(root, scope).result();
    if (!funcRes) {
        return funcRes.error();
    }
    auto res = Resolve(root, scope, nullptr).result();
    if (!res) {
        return res.error();
    }
    return {};
}

std::optional<TSymbolInfo> TNameResolver::Lookup(const std::string& name, TScopeId scopeId) const {
    TScopePtr scope = nullptr;
    if (scopeId.Id < 0 || static_cast<size_t>(scopeId.Id) >= Scopes.size()) {
        return std::nullopt;
    }
    scope = Scopes[scopeId.Id];
    while (scope) {
        auto it = scope->NameToSymbolId.find(name);
        if (it != scope->NameToSymbolId.end()) {
            auto& symbol = Symbols[it->second.Id];
            return TSymbolInfo {
                .Id = symbol.Id.Id,
                .DeclScopeId = symbol.ScopeId.Id,
                .ScopeLevelIdx = symbol.ScopeLevelIdx,
                .FunctionLevelIdx = symbol.FunctionLevelIdx,
                .FuncScopeId = symbol.FuncScopeId.Id,
            };
        }
        scope = scope->Parent;
    }
    return std::nullopt;
}

namespace {

std::vector<uint32_t> CodePoints(const std::string& s) {
    // parse utf-8 to code points
    std::vector<uint32_t>  cp;
    uint32_t codepoint = 0;
    for (char c : s) {
        if ((c & 0x80) == 0) {
            // 1-byte
            if (codepoint != 0) {
                cp.push_back(codepoint);
                codepoint = 0;
            }
            cp.push_back(c);
        } else if ((c & 0xC0) == 0x80) {
            // continuation byte
            codepoint = (codepoint << 6) | (c & 0x3F);
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte
            if (codepoint != 0) {
                cp.push_back(codepoint);
            }
            codepoint = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte
            if (codepoint != 0) {
                cp.push_back(codepoint);
            }
            codepoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte
            if (codepoint != 0) {
                cp.push_back(codepoint);
            }
            codepoint = c & 0x07;
        }
    }
    if (codepoint != 0) {
        cp.push_back(codepoint);
    }
    return cp;
}

uint32_t ToLower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20; // А-Я
    if (cp == 0x401) return 0x451;                     // Ё
    return cp;
}

// Short names get fewer edits, otherwise a single letter matches any two-letter name.
int MaxSuggestionDistance(size_t length) {
    return length <= 3 ? 1 : 2;
}

} // namespace

std::optional<TSuggestion> TNameResolver::Suggest(const std::string& name, TScopeId scopeId, bool includeFunctions) {
    TScopePtr scope = nullptr;
    if (scopeId.Id < 0 || static_cast<size_t>(scopeId.Id) >= Scopes.size()) {
        return std::nullopt;
    }
    scope = Scopes[scopeId.Id];

    TSuggestion bestSuggestion;
    bestSuggestion.Distance = INT32_MAX;
    bestSuggestion.OriginalName = name;
    std::unordered_set<std::string> checkedNames;

    auto nameCodePoints = CodePoints(name);
    const int maxDistance = MaxSuggestionDistance(nameCodePoints.size());

    // A keyword with a capital letter is lexed as part of an identifier,
    // e.g. "Использовать Черепаха".
    {
        auto space = name.find(' ');
        auto firstWord = CodePoints(name.substr(0, space));
        if (!firstWord.empty() && ToLower(firstWord[0]) != firstWord[0]) {
            firstWord[0] = ToLower(firstWord[0]);
            for (const auto& keyword : NAst::KeywordNames()) {
                if (CodePoints(keyword) == firstWord) {
                    bestSuggestion.Name = keyword + (space == std::string::npos ? "" : name.substr(space));
                    bestSuggestion.Distance = 1;
                    return bestSuggestion;
                }
            }
        }
    }

    // The exact name of a command from a module that is not imported beats any
    // similar local name: "вперед" with Робот is a Черепаха command, not "вверх".
    if (includeFunctions) {
        for (const auto& [moduleName, module] : Modules) {
            if (!module || ImportedModules.contains(moduleName)) {
                continue;
            }
            for (const auto& extFunc : module->ExternalFunctions()) {
                if (!extFunc.IsOp && extFunc.Name == name) {
                    bestSuggestion.Name = name;
                    bestSuggestion.Distance = 0;
                    bestSuggestion.RequiredModuleName = moduleName;
                    return bestSuggestion;
                }
            }
        }
    }

    while (scope) {
        for (auto symbolId : scope->Symbols) {
            auto& symbol = Symbols[symbolId];
            if (!includeFunctions) {
                if (TMaybeNode<TFunDecl>(symbol.Node)) {
                    continue;
                }
            }
            if (checkedNames.find(symbol.Name) != checkedNames.end()) {
                continue;
            }
            checkedNames.insert(symbol.Name);
            if (symbol.CodePoints.empty()) {
                symbol.CodePoints = CodePoints(symbol.Name);
            }
            if ( (int)symbol.CodePoints.size() - (int)nameCodePoints.size() > maxDistance) {
                continue;
            }
            int distance = EditDistanceCalculator.Calc(
                std::span<const uint32_t>(nameCodePoints.data(), nameCodePoints.size()),
                std::span<const uint32_t>(symbol.CodePoints.data(), symbol.CodePoints.size())
            );
            if (distance < bestSuggestion.Distance && distance <= maxDistance) {
                bestSuggestion.Name = symbol.Name;
                bestSuggestion.Distance = distance;
            }
        }
        scope = scope->Parent;
    }

    // search in modules
    if (includeFunctions && bestSuggestion.Distance == INT32_MAX) {
        for (const auto& [moduleName, module] : Modules) {
            if (!module) {
                continue; // skip null module pointer
            }
            for (const auto& extFunc : module->ExternalFunctions()) {
                const auto& symbolName = extFunc.Name;
                if (checkedNames.find(symbolName) != checkedNames.end()) {
                    continue;
                }
                checkedNames.insert(symbolName);
                auto& symbolCodePoints = extFunc.NameCodePoints;
                if (symbolCodePoints.empty()) {
                    symbolCodePoints = CodePoints(symbolName);
                }
                if ( (int)symbolCodePoints.size() - (int)nameCodePoints.size() > maxDistance) {
                    continue;
                }
                int distance = EditDistanceCalculator.Calc(
                    std::span<const uint32_t>(nameCodePoints.data(), nameCodePoints.size()),
                    std::span<const uint32_t>(symbolCodePoints.data(), symbolCodePoints.size())
                );
                if (distance < bestSuggestion.Distance && distance <= maxDistance) {
                    bestSuggestion.Name = symbolName;
                    bestSuggestion.Distance = distance;
                    bestSuggestion.RequiredModuleName = moduleName;
                }
            }
        }
    }

    if (bestSuggestion.Distance == INT32_MAX) {
        return std::nullopt;
    }
    return bestSuggestion;
}

std::expected<TSymbolId, TError> TNameResolver::Declare(const std::string& name, TExprPtr node, TScopePtr scope, TScopePtr funcScope) {
    auto maybeSymbolId = scope->NameToSymbolId.find(name);
    TSymbolId symbolId{-1};
    if (auto fd = NAst::TMaybeNode<NAst::TFunDecl>(node)) {
        if (Options.AllowOverloads) {
            auto ovIt = scope->OverloadSets.find(name);
            if (ovIt != scope->OverloadSets.end()) {
                return RegisterOverloadEntry(name, node, ovIt->second);
            }
        }
        // Independent of overloads: a sole cacheable decl gets a signature-stable
        // emit symbol (versioned by KernelLibVersion) while keeping canonical
        // Name lookup.
        if (fd.Cast()->Cacheable && !fd.Cast()->Mangled
            && maybeSymbolId == scope->NameToSymbolId.end()) {
            fd.Cast()->MangledName = OverloadMangledName(name, *fd.Cast());
            fd.Cast()->Mangled = true;
        }
    }

    if (maybeSymbolId != scope->NameToSymbolId.end()) {
        if (!scope->AllowsRedeclare) {
            auto& existingSymbol = Symbols[maybeSymbolId->second.Id];
            if (Options.AllowOverloads && NAst::TMaybeNode<NAst::TFunDecl>(existingSymbol.Node) && NAst::TMaybeNode<NAst::TFunDecl>(node)) {
                return StartOverloadSet(name, maybeSymbolId->second, node, scope);
            }
            std::ostringstream ss;
            ss << "Переопределение `" << existingSymbol.Name << "' уже объявлено в области видимости " << existingSymbol.ScopeId.Id;
            return std::unexpected(TError(node->Location, ss.str()));
        }
        symbolId = maybeSymbolId->second;
    }

    TSymbol& symbol = symbolId.Id != -1
        ? Symbols[symbolId.Id] // redeclare
        : Symbols.emplace_back(TSymbol {
            .Id = {(int32_t)Symbols.size()},
            .ScopeId = scope->Id,
            .ScopeLevelIdx = (int32_t)scope->Symbols.size(),
            .FunctionLevelIdx = funcScope ? (int32_t)funcScope->FuncSymbols.size() : -1,
            .FuncScopeId = funcScope ? funcScope->Id : TScopeId{-1},
            .Name = name,
            .Node = node,
        });

    if (funcScope) {
        funcScope->FuncSymbols.insert(symbol.Id.Id);
    }
    scope->Symbols.emplace(symbol.Id.Id);
    scope->NameToSymbolId[name] = symbol.Id;

    return symbol.Id;
}

TSymbolId TNameResolver::Declare(const std::string& name, NAst::TExprPtr node, TSymbolInfo parentSymbol)
{
    auto scope = Scopes[parentSymbol.DeclScopeId];
    auto funcScope = parentSymbol.FuncScopeId >= 0 ? Scopes[parentSymbol.FuncScopeId] : nullptr;
    auto res = Declare(name, node, scope, funcScope);
    if (res) {
        return res.value();
    }
    throw std::runtime_error(res.error().ToString());
}

TSymbolId TNameResolver::DeclareFunction(const std::string& name, TExprPtr node) {
    auto scope = GetOrCreateRootScope();
    auto res = Declare(name, node, scope, nullptr);
    if (!res) {
        throw std::runtime_error(std::string("failed to declare function: ") + res.error().what());
    }

    return res.value();
}

std::expected<TSymbolId, TError> TNameResolver::ResolveInstantiatedFunDecl(std::shared_ptr<TFunDecl> fdecl, bool publish) {
    auto rootScope = GetOrCreateRootScope();
    std::optional<TSymbolId> publishedSymId;
    if (publish) {
        auto declRes = Declare(fdecl->Name, fdecl, rootScope, nullptr);
        if (!declRes) {
            return std::unexpected(declRes.error());
        }
        publishedSymId = declRes.value();
    }

    auto funcScope = NewScope(rootScope, nullptr);
    fdecl->Scope = funcScope->Id.Id;
    AddGenericParamsToScope(fdecl->GenericParams, funcScope);
    if (auto err = ResolveTypeRef(fdecl->Type, fdecl->Location, funcScope)) {
        return std::unexpected(*err);
    }
    if (auto err = ResolveTypeRef(fdecl->RetType, fdecl->Location, funcScope)) {
        return std::unexpected(*err);
    }
    for (auto& param : fdecl->Params) {
        if (auto err = ResolveTypeRef(param->Type, param->Location, funcScope)) {
            return std::unexpected(*err);
        }
        auto paramRes = Declare(param->Name, param, funcScope, funcScope);
        if (!paramRes) {
            return std::unexpected(paramRes.error());
        }
    }

    auto bodyRes = Resolve(fdecl->Body, funcScope, funcScope).result();
    if (!bodyRes) {
        return std::unexpected(bodyRes.error());
    }

    if (publish) {
        GenericInstantiations.push_back(fdecl);
    }

    return publishedSymId.value_or(TSymbolId{-1});
}

std::vector<std::pair<int, std::shared_ptr<NAst::TFunDecl>>> TNameResolver::GetExternalFunctions() {
    auto scope = GetOrCreateRootScope();

    std::vector<std::pair<int, std::shared_ptr<NAst::TFunDecl>>> result;
    for (auto& symbolId : scope->Symbols) {
        auto& symbol = Symbols[symbolId];
        if (auto maybeFun = TMaybeNode<TFunDecl>(symbol.Node)) {
            auto fun = maybeFun.Cast();
            if (fun->Body == nullptr) {
                result.push_back({symbolId, fun});
            }
        }
    }
    return result;
}

bool TNameResolver::ParamTypesSame(const NAst::TFunDecl& a, const NAst::TFunDecl& b) {
    if (a.Params.size() != b.Params.size()) {
        return false;
    }
    auto aGenericAliases = GenericParamAliases(a);
    auto bGenericAliases = GenericParamAliases(b);
    for (size_t i = 0; i < a.Params.size(); ++i) {
        if (OverloadTypeKey(a.Params[i]->Type, aGenericAliases)
            != OverloadTypeKey(b.Params[i]->Type, bGenericAliases))
        {
            return false;
        }
    }
    return true;
}

std::expected<TSymbolId, TError> TNameResolver::RegisterOverloadEntry(
    const std::string& canonicalName,
    NAst::TExprPtr node,
    std::vector<TSymbolId>& overloads)
{
    auto newDecl = NAst::TMaybeNode<NAst::TFunDecl>(node).Cast();
    if (newDecl->OriginalName.empty()) {
        newDecl->OriginalName = canonicalName;
    }
    for (const auto& existingId : overloads) {
        auto existingDecl = NAst::TMaybeNode<NAst::TFunDecl>(GetSymbolNode(existingId)).Cast();
        if (existingDecl && ParamTypesSame(*newDecl, *existingDecl)) {
            // A source definition may replace an identically-typed function
            // from the host prelude. This lets a host module specialize its
            // runtime surface without dropping the rest of the prelude.
            if (existingDecl->IsExternal() && !newDecl->IsExternal()) {
                newDecl->Name = existingDecl->Name;
                newDecl->Mangled = true;
                Symbols[existingId.Id].Node = node;
                return existingId;
            }
            return std::unexpected(TError(
                newDecl->Location,
                "overload of '" + canonicalName + "' has identical parameter types"));
        }
    }
    std::string synthName = OverloadMangledName(canonicalName, *newDecl);
    newDecl->Name = synthName;
    newDecl->Mangled = true;
    auto symId = DeclareFunction(synthName, node);
    overloads.push_back(symId);
    return symId;
}

std::expected<TSymbolId, TError> TNameResolver::StartOverloadSet(
    const std::string& name,
    TSymbolId existingSymId,
    NAst::TExprPtr newNode,
    TScopePtr scope)
{
    auto& overloads = scope->OverloadSets[name];
    scope->NameToSymbolId.erase(name);
    auto first = RegisterOverloadEntry(name, Symbols[existingSymId.Id].Node, overloads);
    if (!first) {
        return first;
    }
    return RegisterOverloadEntry(name, newNode, overloads);
}

std::vector<TSymbolId> TNameResolver::LookupOverloads(const std::string& name, TScopeId scopeId) const {
    auto scope = Scopes[scopeId.Id];
    while (scope) {
        auto ovIt = scope->OverloadSets.find(name);
        if (ovIt != scope->OverloadSets.end()) {
            return ovIt->second;
        }
        auto nmIt = scope->NameToSymbolId.find(name);
        if (nmIt != scope->NameToSymbolId.end()) {
            return {nmIt->second};
        }
        scope = scope->Parent;
    }
    return {};
}

TScopePtr TNameResolver::NewScope(TScopePtr parent, TScopePtr funcScope) {
    auto newScope = std::make_shared<TScope>(TScope {
        .Id = {(int32_t)Scopes.size()},
        .Parent = parent,
        .FuncScope = funcScope,
    });
    Scopes.push_back(newScope);
    return newScope;
}

TScopePtr TNameResolver::GetOrCreateRootScope() {
    if (Scopes.empty()) {
        auto root = NewScope(nullptr, nullptr);
        root->RootLevel = true;
        return root;
    }
    return Scopes[0];
}

TExprPtr TNameResolver::GetSymbolNode(TSymbolId id) const {
    if (id.Id < 0 || static_cast<size_t>(id.Id) >= Symbols.size()) {
        return nullptr;
    }
    return Symbols[id.Id].Node;
}

std::optional<TNameResolver::TRegisteredOp> TNameResolver::GetBinaryOp(
    const std::string& op, const NAst::TTypePtr& left, const NAst::TTypePtr& right) const
{
    auto it = ImportedBinaryOps.find({op, TypeKey(left), TypeKey(right)});
    if (it != ImportedBinaryOps.end()) return it->second;
    return std::nullopt;
}

std::optional<TNameResolver::TRegisteredOp> TNameResolver::GetUnaryOp(
    const std::string& op, const NAst::TTypePtr& operand) const
{
    auto it = ImportedUnaryOps.find({op, TypeKey(operand)});
    if (it != ImportedUnaryOps.end()) return it->second;
    return std::nullopt;
}

void TNameResolver::RegisterUnaryOp(const std::string& op, const NAst::TTypePtr& operand, TRegisteredOp target) {
    ImportedUnaryOps[{op, TypeKey(operand)}] = std::move(target);
}

std::vector<std::shared_ptr<NAst::TFunDecl>> TNameResolver::LookupGenericOperatorDecls(
    const std::string& op,
    std::size_t arity) const
{
    std::vector<std::shared_ptr<NAst::TFunDecl>> result;
    for (const auto& decl : GenericOperatorDecls) {
        if (!decl->OperatorName || *decl->OperatorName != op || decl->Params.size() != arity) {
            continue;
        }
        result.push_back(decl);
    }
    return result;
}

NAst::TTypePtr TNameResolver::LookupType(const std::string& name) const {
    auto it = ImportedTypes.find(name);
    if (it != ImportedTypes.end()) return it->second;
    return nullptr;
}

void TNameResolver::RegisterType(const std::string& name, NAst::TTypePtr underlying) {
    RegisterTypeDecl(name, std::move(underlying), {});
}

void TNameResolver::RegisterTypeDecl(
    const std::string& name,
    NAst::TTypePtr underlying,
    std::vector<NAst::TGenericParam> genericParams)
{
    if (auto n = NAst::TMaybeType<NAst::TNamedType>(underlying); n && n.Cast()->Name == name) {
        throw std::runtime_error("RegisterType: underlying is a TNamedType with the same name '"
            + name + "' — see TExternalType::Type contract (must be unwrapped, not named)");
    }
    ImportedTypes[name] = underlying;
    RegisteredTypes[name] = TRegisteredTypeDecl{
        .UnderlyingType = std::move(underlying),
        .GenericParams = std::move(genericParams),
    };
}

std::optional<TError> TNameResolver::RegisterTypeDecls(const NAst::TExprPtr& root) {
    auto block = TMaybeNode<TBlockExpr>(root);
    if (!block) return {};
    std::unordered_set<std::string> localTypeNames;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto td = TMaybeNode<TTypeDeclStmt>(stmt);
        if (!td) continue;
        auto named = TMaybeType<TNamedType>(td.Cast()->Type);
        if (!named) continue;
        auto n = named.Cast();
        if (ImportedModuleSymbols.contains(n->Name) || !localTypeNames.insert(n->Name).second) {
            return TError(stmt->Location, "Тип уже объявлен или импортирован: " + n->Name);
        }
        RegisterTypeDecl(n->Name, n->UnderlyingType, td.Cast()->GenericParams);
    }
    return {};
}

std::optional<TError> TNameResolver::RegisterOperatorDecls(const NAst::TExprPtr& root) {
    auto block = TMaybeNode<TBlockExpr>(root);
    if (!block) return std::nullopt;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto fd = TMaybeNode<TFunDecl>(stmt);
        if (!fd) continue;
        auto fun = fd.Cast();
        if (!fun->OperatorName) continue;
        if (!fun->GenericParams.empty()) {
            for (const auto& existing : GenericOperatorDecls) {
                if (existing == fun) {
                    continue;
                }
                if (!existing->OperatorName
                    || *existing->OperatorName != *fun->OperatorName
                    || existing->Params.size() != fun->Params.size())
                {
                    continue;
                }
                if (ParamTypesSame(*fun, *existing)) {
                    return TError(
                        fun->Location,
                        "operator overload '" + *fun->OperatorName + "' has identical parameter types");
                }
            }
            if (std::find(GenericOperatorDecls.begin(), GenericOperatorDecls.end(), fun) == GenericOperatorDecls.end()) {
                GenericOperatorDecls.push_back(fun);
            }
            continue;
        }
        const std::string& op = *fun->OperatorName;
        std::vector<NAst::TTypePtr> argTypes;
        for (const auto& p : fun->Params) {
            argTypes.push_back(p->Type);
        }
        // Dispatch target is the function's own name (already declared at top
        // level), unlike runtime modules which use a synthetic name.
        if (op == "cast" && argTypes.size() == 1) {
            ImportedCasts[{TypeKey(argTypes[0]), TypeKey(fun->RetType)}] = fun->Name;
        } else if (argTypes.size() == 2) {
            ImportedBinaryOps[{op, TypeKey(argTypes[0]), TypeKey(argTypes[1])}]
                = {fun->Name, fun->RetType};
        } else if (argTypes.size() == 1) {
            ImportedUnaryOps[{op, TypeKey(argTypes[0])}]
                = {fun->Name, fun->RetType};
        }
    }
    return std::nullopt;
}

void TNameResolver::ImportUseStmts(const NAst::TExprPtr& root) {
    auto block = TMaybeNode<TBlockExpr>(root);
    if (!block) return;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto use = TMaybeNode<TUseExpr>(stmt);
        if (!use) continue;
        if (use.Cast()->Resolved) {
            continue;
        }
        ImportModule(use.Cast()->ModuleName);
    }
}

std::optional<std::string> TNameResolver::GetCast(const NAst::TTypePtr& from, const NAst::TTypePtr& to) const {
    auto it = ImportedCasts.find({TypeKey(from), TypeKey(to)});
    if (it == ImportedCasts.end()) return std::nullopt;
    return it->second;
}

void TNameResolver::PrintSymbols(std::ostream& os) const {
    for (const auto& symbol : Symbols) {
        os << "Symbol: " << symbol.Name << ", Scope: " << symbol.ScopeId.Id << "\n";
    }
}

void TNameResolver::RegisterModule(NRegistry::IModule* module) {
    if (!module) {
        throw std::runtime_error("Cannot register null module");
    }
    auto [it, flag] = Modules.insert({module->Name(), module});
    if (flag == false && it->second != module) {
        throw std::runtime_error("Module with conflicting name: " + module->Name());
    }
}

void TNameResolver::RegisterModuleAlias(const std::string& alias, const std::string& canonical) {
    if (!Modules.count(canonical)) {
        throw std::runtime_error(
            "Cannot register alias '" + alias + "': unknown module '" + canonical + "'");
    }
    auto [it, inserted] = ModuleAliases.insert({alias, canonical});
    if (!inserted && it->second != canonical) {
        throw std::runtime_error("Conflicting alias '" + alias + "'");
    }
}

std::optional<std::string> TNameResolver::ClosestModuleName(const std::string& name) {
    auto lower = [](std::vector<uint32_t> cps) {
        for (auto& cp : cps) cp = ToLower(cp);
        return cps;
    };
    auto target = lower(CodePoints(name));
    std::optional<std::string> best;
    int bestDistance = MaxSuggestionDistance(target.size()) + 1;
    auto consider = [&](const std::string& candidate) {
        auto cps = lower(CodePoints(candidate));
        int distance = EditDistanceCalculator.Calc(
            std::span<const uint32_t>(target.data(), target.size()),
            std::span<const uint32_t>(cps.data(), cps.size()));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    };
    for (const auto& [moduleName, module] : Modules) {
        if (module && !module->IsImplicit()) {
            consider(moduleName);
        }
    }
    for (const auto& [alias, canonical] : ModuleAliases) {
        consider(alias);
    }
    return best;
}

std::vector<std::string> TNameResolver::ModulesWithFunction(const std::string& name, size_t argCount) const {
    std::vector<std::string> result;
    for (const auto& [moduleName, module] : Modules) {
        if (!module || ImportedModules.contains(moduleName)) {
            continue;
        }
        for (const auto& extFunc : module->ExternalFunctions()) {
            if (!extFunc.IsOp && extFunc.Name == name && extFunc.ArgTypes.size() == argCount) {
                result.push_back(moduleName);
                break;
            }
        }
    }
    return result;
}

std::string TNameResolver::ModulesList() const
{
    // Sorted: the message must not depend on the hash map order of the standard library.
    std::vector<std::string> names;
    for (const auto& [name, module] : Modules) {
        if (module && !module->IsImplicit()) {
            names.push_back(name);
        }
    }
    for (const auto& [alias, canonical] : ModuleAliases) {
        names.push_back(alias);
    }
    std::sort(names.begin(), names.end());
    std::ostringstream oss;
    for (size_t i = 0; i < names.size(); ++i) {
        oss << (i ? "," : "") << names[i];
    }
    return oss.str();
}

std::vector<std::string> TNameResolver::GetAllImportedTypeNames() const {
    std::vector<std::string> names;
    names.reserve(ImportedTypes.size());
    for (const auto& [name, _] : ImportedTypes) {
        names.push_back(name);
    }
    return names;
}

std::vector<NRegistry::TLiteralSuffix> TNameResolver::GetAllImportedLiteralSuffixes() const {
    std::vector<NRegistry::TLiteralSuffix> result;
    for (const auto& modName : ImportedModules) {
        auto it = Modules.find(modName);
        if (it == Modules.end() || !it->second) continue;
        for (const auto& s : it->second->LiteralSuffixes()) {
            result.push_back(s);
        }
    }
    return result;
}

std::expected<bool, std::string> TNameResolver::ImportModule(const std::string& aliasOrName) {
    auto aliasIt = ModuleAliases.find(aliasOrName);
    const std::string& name = aliasIt != ModuleAliases.end() ? aliasIt->second : aliasOrName;
    if (ImportedModules.count(name)) {
        return false;
    }
    auto it = Modules.find(name);
    if (it == Modules.end()) {
        std::string message = "Неизвестный модуль: " + aliasOrName + ", доступные модули: " + ModulesList();
        if (auto closest = ClosestModuleName(aliasOrName)) {
            message += "\n Возможно вы имели в виду `" + *closest + "'.";
        }
        return std::unexpected(message);
    }
    auto* module = it->second;

    for (const auto& fn : module->ExternalFunctions()) {
        if (fn.IsOp) continue; // operators allow overloading - no conflict check
        auto conflict = ImportedModuleSymbols.find(fn.Name);
        if (conflict != ImportedModuleSymbols.end()) {
            return std::unexpected(
                "Конфликт имён при импорте модуля " + name + ": символ " + fn.Name +
                " уже импортирован из модуля " + conflict->second);
        }
    }
    for (const auto& type : module->ExternalTypes()) {
        auto conflict = ImportedModuleSymbols.find(type.Name);
        if (conflict != ImportedModuleSymbols.end()) {
            return std::unexpected(
                "Конфликт имён при импорте модуля " + name + ": тип " + type.Name +
                " уже импортирован из модуля " + conflict->second);
        }
    }

    ImportedModules.insert(name);
    for (const auto& dep : module->Dependencies()) {
        if (!ImportedModules.count(dep)) {
            auto res = ImportModule(dep);
            if (!res) {
                ImportedModules.erase(name); // rollback
                return std::unexpected("Не удалось импортировать зависимость " + dep + " модуля " + name + ": " + res.error());
            }
        }
    }

    for (const auto& fn : module->ExternalFunctions()) {
        auto funType = std::make_shared<NAst::TFunctionType>(fn.ArgTypes, fn.ReturnType);
        std::vector<NAst::TParam> params;
        for (size_t i = 0; i < fn.ArgTypes.size(); ++i) {
            params.push_back(std::make_shared<NAst::TVarStmt>(TLocation{}, "arg" + std::to_string(i), fn.ArgTypes[i]));
        }
        auto funDecl = std::make_shared<NAst::TFunDecl>(TLocation{}, fn.Name, std::vector<TGenericParam>{}, params, nullptr, fn.ReturnType);
        funDecl->MangledName = fn.MangledName;
        funDecl->Type = funType;
        funDecl->Packed = fn.Packed;
        funDecl->RequireArgsMaterialization = fn.RequireArgsMaterialization;
        funDecl->NeedsLocator = fn.NeedsLocator;
        funDecl->InlineFactory = fn.Inline;

        if (fn.IsOp) {
            ImportedOperators.push_back(funDecl);
            if (fn.Name == "cast" && fn.ArgTypes.size() == 1) {
                std::string synthName = "__cast_" + TypeKey(fn.ArgTypes[0]) + "_" + TypeKey(fn.ReturnType);
                DeclareFunction(synthName, funDecl);
                ImportedCasts[{TypeKey(fn.ArgTypes[0]), TypeKey(fn.ReturnType)}] = synthName;
            } else if (fn.ArgTypes.size() == 2) {
                std::string synthName = "__binop_" + fn.Name + "_"
                    + TypeKey(fn.ArgTypes[0]) + "_" + TypeKey(fn.ArgTypes[1]);
                DeclareFunction(synthName, funDecl);
                ImportedBinaryOps[{fn.Name, TypeKey(fn.ArgTypes[0]), TypeKey(fn.ArgTypes[1])}]
                    = {synthName, fn.ReturnType};
            } else if (fn.ArgTypes.size() == 1) {
                std::string synthName = "__unop_" + fn.Name + "_" + TypeKey(fn.ArgTypes[0]);
                DeclareFunction(synthName, funDecl);
                ImportedUnaryOps[{fn.Name, TypeKey(fn.ArgTypes[0])}]
                    = {synthName, fn.ReturnType};
            }
        } else {
            ImportedModuleSymbols[fn.Name] = name;
            DeclareFunction(fn.Name, funDecl);
        }
    }
    for (const auto& type : module->ExternalTypes()) {
        if (auto n = NAst::TMaybeType<NAst::TNamedType>(type.Type); n && n.Cast()->Name == type.Name) {
            throw std::runtime_error("ImportModule('" + name + "'): TExternalType{'" + type.Name
                + "'}.Type is a TNamedType with the same name — contract violation (must be unwrapped)");
        }
        ImportedModuleSymbols[type.Name] = name;
        RegisterType(type.Name, type.Type);
    }

    return true;
}


} // namespace NSemantics
} // namespace NQumir
