#include "validator.h"

#include "type_traits.h"

#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace NQumir {
namespace NSemantics {
namespace {

using namespace NAst;

enum class EManagedValueKind {
    None,
    Literal,
    Borrowed,
    Owned,
    Unknown,
};

bool IsLifetimeNode(const TExprPtr& expr) {
    return TMaybeNode<TRetainExpr>(expr)
        || TMaybeNode<TOwnLiteralExpr>(expr)
        || TMaybeNode<TMoveExpr>(expr)
        || TMaybeNode<TBorrowExpr>(expr)
        || TMaybeNode<TDestroyExpr>(expr)
        || TMaybeNode<TReplaceExpr>(expr)
        || TMaybeNode<TCleanupExitExpr>(expr)
        || TMaybeNode<TGlobalCleanupExpr>(expr);
}

bool ContainsLifetimeNode(const TExprPtr& expr) {
    if (IsLifetimeNode(expr)) {
        return true;
    }
    for (const auto& child : expr->Children()) {
        if (child && ContainsLifetimeNode(child)) {
            return true;
        }
    }
    return false;
}

// A reference to `type` or to a type it points to, so `type` must outlive it.
const TTypePtr& ValueType(const TTypePtr& type) {
    const TTypePtr* current = &type;
    while (true) {
        if (auto reference = TMaybeType<TReferenceType>(*current)) {
            current = &reference.Cast()->ReferencedType;
        } else if (auto named = TMaybeType<TNamedType>(*current)) {
            current = &named.Cast()->UnderlyingType();
        } else {
            return *current;
        }
    }
}

const TTypePtr& ValueType(TTypePtr&& type) = delete;

const TLifetimeTraits* TryTraits(const TTypePtr& type, TLifetimeTraits& storage) {
    if (!type) {
        return nullptr;
    }
    try {
        storage = GetLifetimeTraits(ValueType(type));
        return &storage;
    } catch (const std::invalid_argument&) {
        return nullptr;
    }
}

bool NeedsDestroy(const TExprPtr& expr) {
    TLifetimeTraits storage;
    const auto* traits = TryTraits(expr ? expr->Type : nullptr, storage);
    return traits && traits->NeedsDestroy;
}

EManagedValueKind ManagedValueKind(const TExprPtr& expr) {
    if (!expr || !NeedsDestroy(expr)) {
        return EManagedValueKind::None;
    }
    if (TMaybeNode<TStringLiteralExpr>(expr)) {
        return EManagedValueKind::Literal;
    }
    if (TMaybeNode<TRetainExpr>(expr)
        || TMaybeNode<TOwnLiteralExpr>(expr)
        || TMaybeNode<TMoveExpr>(expr)
        || TMaybeNode<TCallExpr>(expr)
        || TMaybeNode<TAwaitExpr>(expr))
    {
        return EManagedValueKind::Owned;
    }
    if (TMaybeNode<TBorrowExpr>(expr)
        || TMaybeNode<TIdentExpr>(expr)
        || TMaybeNode<TIndexExpr>(expr)
        || TMaybeNode<TMultiIndexExpr>(expr)
        || TMaybeNode<TFieldAccessExpr>(expr))
    {
        return EManagedValueKind::Borrowed;
    }
    if (auto block = TMaybeNode<TBlockExpr>(expr)) {
        if (block.Cast()->Stmts.empty()) {
            return EManagedValueKind::None;
        }
        return ManagedValueKind(block.Cast()->Stmts.back());
    }
    return EManagedValueKind::Unknown;
}

bool IsSynthetic(std::string_view name) {
    return name.starts_with("__lifetime_");
}

class TValidator {
public:
    explicit TValidator(TNameResolver& context)
        : Context_(context)
    {}

    std::expected<void, TError> Validate(const TExprPtr& root) {
        LifetimeMode_ = ContainsLifetimeNode(root);
        return ValidateNode(root, TScopeId{0}, false);
    }

private:
    std::expected<void, TError> Fail(const TExprPtr& expr, std::string message) {
        return std::unexpected(TError(expr->Location, std::move(message)));
    }

    std::expected<void, TError> ValidateSynthetic(
        const TExprPtr& expr,
        std::string_view name,
        TScopeId scopeId,
        bool declaration)
    {
        if (!IsSynthetic(name)) {
            return {};
        }
        auto symbol = Context_.Lookup(std::string(name), scopeId);
        if (!symbol) {
            return Fail(expr, "Unresolved synthetic lifetime variable '" + std::string(name) + "'.");
        }
        if (declaration
            && Context_.GetSymbolNode(TSymbolId{symbol->Id}) != expr)
        {
            return Fail(expr, "Synthetic lifetime variable resolves to a different declaration.");
        }
        return {};
    }

    bool IsOwnedStorage(const TExprPtr& expr, TScopeId scopeId) {
        auto ident = TMaybeNode<TIdentExpr>(expr);
        if (!ident) {
            return false;
        }
        auto symbol = Context_.Lookup(ident.Cast()->Name, scopeId);
        if (!symbol) {
            return false;
        }
        auto declaration = Context_.GetSymbolNode(TSymbolId{symbol->Id});
        if (!TMaybeNode<TVarStmt>(declaration)) {
            return false;
        }
        return !Parameters_.contains(declaration);
    }

    std::expected<void, TError> ValidateNode(
        const TExprPtr& expr,
        TScopeId scopeId,
        bool consumesOwned)
    {
        if (!expr) {
            return {};
        }
        if (auto block = TMaybeNode<TBlockExpr>(expr)) {
            const auto blockScope = block.Cast()->Scope >= 0
                ? TScopeId{block.Cast()->Scope}
                : scopeId;
            for (size_t i = 0; i < block.Cast()->Stmts.size(); ++i) {
                const bool consumesStatement = consumesOwned
                    && i + 1 == block.Cast()->Stmts.size();
                if (auto result = ValidateNode(
                    block.Cast()->Stmts[i],
                    blockScope,
                    consumesStatement); !result)
                {
                    return result;
                }
            }
            return {};
        }
        if (auto function = TMaybeNode<TFunDecl>(expr)) {
            for (const auto& parameter : function.Cast()->Params) {
                Parameters_.insert(parameter.Get());
            }
            auto result = ValidateNode(
                function.Cast()->Body,
                TScopeId{function.Cast()->Scope},
                false);
            for (const auto& parameter : function.Cast()->Params) {
                Parameters_.erase(parameter.Get());
            }
            return result;
        }
        if (auto returnExpr = TMaybeNode<TReturnExpr>(expr)) {
            return ValidateNode(returnExpr.Cast()->Value, scopeId, true);
        }
        if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
            if (auto result = ValidateSynthetic(
                expr,
                ident.Cast()->Name,
                scopeId,
                false); !result)
            {
                return result;
            }
        }
        if (auto variable = TMaybeNode<TVarStmt>(expr)) {
            if (auto result = ValidateSynthetic(
                expr,
                variable.Cast()->Name,
                scopeId,
                true); !result)
            {
                return result;
            }
            if (variable.Cast()->Init) {
                if (LifetimeMode_
                    && NeedsDestroy(expr)
                    && TMaybeNode<TStringLiteralExpr>(variable.Cast()->Init))
                {
                    return Fail(expr,
                        "Raw string literal requires own-literal before entering owned storage.");
                }
                TLifetimeTraits traitsStorage;
                const auto* traits = TryTraits(variable.Cast()->Type, traitsStorage);
                if (LifetimeMode_
                    && traits
                    && !traits->CanCopy
                    && ManagedValueKind(variable.Cast()->Init) == EManagedValueKind::Borrowed)
                {
                    return Fail(expr, "Unique value cannot be copied.");
                }
                if (auto result = ValidateNode(
                    variable.Cast()->Init,
                    scopeId,
                    NeedsDestroy(expr)); !result)
                {
                    return result;
                }
            }
            for (const auto& bound : variable.Cast()->Bounds) {
                if (auto result = ValidateNode(bound.first, scopeId, false); !result) {
                    return result;
                }
                if (auto result = ValidateNode(bound.second, scopeId, false); !result) {
                    return result;
                }
            }
            return {};
        }

        const bool explicitOwnedValue = TMaybeNode<TRetainExpr>(expr)
            || TMaybeNode<TOwnLiteralExpr>(expr)
            || TMaybeNode<TMoveExpr>(expr);
        if (LifetimeMode_
            && explicitOwnedValue
            && ManagedValueKind(expr) == EManagedValueKind::Owned
            && !consumesOwned)
        {
            return Fail(expr, "Owned result has no move/destroy consumer.");
        }
        if (auto retain = TMaybeNode<TRetainExpr>(expr)) {
            TLifetimeTraits storage;
            const auto* traits = TryTraits(retain.Cast()->Value->Type, storage);
            if (!traits || traits->Kind != ELifetimeKind::RefCounted) {
                return Fail(expr, "retain requires a ref-counted value.");
            }
            if (ManagedValueKind(retain.Cast()->Value) != EManagedValueKind::Borrowed) {
                return Fail(expr, "retain requires a borrowed value.");
            }
            return ValidateNode(retain.Cast()->Value, scopeId, false);
        }
        if (auto literal = TMaybeNode<TOwnLiteralExpr>(expr)) {
            if (!TMaybeNode<TStringLiteralExpr>(literal.Cast()->Value)) {
                return Fail(expr, "own-literal requires a supported literal value.");
            }
            return ValidateNode(literal.Cast()->Value, scopeId, false);
        }
        if (auto move = TMaybeNode<TMoveExpr>(expr)) {
            if (ManagedValueKind(move.Cast()->Value) != EManagedValueKind::Owned
                && !IsOwnedStorage(move.Cast()->Value, scopeId))
            {
                return Fail(expr, "move requires an owned value.");
            }
            return ValidateNode(move.Cast()->Value, scopeId, true);
        }
        if (auto borrow = TMaybeNode<TBorrowExpr>(expr)) {
            if (ManagedValueKind(borrow.Cast()->Value) != EManagedValueKind::Borrowed) {
                return Fail(expr, "borrow requires a borrowed managed value.");
            }
            return ValidateNode(borrow.Cast()->Value, scopeId, false);
        }
        if (auto destroy = TMaybeNode<TDestroyExpr>(expr)) {
            TLifetimeTraits storage;
            const auto* traits = TryTraits(destroy.Cast()->Value->Type, storage);
            if (!traits || !traits->NeedsDestroy) {
                return Fail(expr, "destroy requires a managed value.");
            }
            if (ManagedValueKind(destroy.Cast()->Value) != EManagedValueKind::Owned
                && !IsOwnedStorage(destroy.Cast()->Value, scopeId))
            {
                return Fail(expr, "destroy cannot consume a borrowed value.");
            }
            if (auto result = ValidateNode(destroy.Cast()->Value, scopeId, true); !result) {
                return result;
            }
            return ValidateNode(destroy.Cast()->Aux, scopeId, false);
        }
        if (auto replace = TMaybeNode<TReplaceExpr>(expr)) {
            TLifetimeTraits storage;
            const auto* traits = TryTraits(replace.Cast()->Target->Type, storage);
            if (!traits || !traits->NeedsDestroy) {
                return Fail(expr, "replace requires managed storage.");
            }
            if (ManagedValueKind(replace.Cast()->Value) != EManagedValueKind::Owned) {
                if (!traits->CanCopy) {
                    return Fail(expr, "Unique value cannot be copied.");
                }
                if (TMaybeNode<TStringLiteralExpr>(replace.Cast()->Value)) {
                    return Fail(expr,
                        "Raw string literal requires own-literal before entering owned storage.");
                }
                return Fail(expr, "replace requires an owned value.");
            }
            if (auto result = ValidateNode(replace.Cast()->Target, scopeId, false); !result) {
                return result;
            }
            return ValidateNode(replace.Cast()->Value, scopeId, true);
        }
        if (auto exit = TMaybeNode<TCleanupExitExpr>(expr)) {
            if (exit.Cast()->Value
                && NeedsDestroy(exit.Cast()->Value)
                && ManagedValueKind(exit.Cast()->Value) != EManagedValueKind::Owned)
            {
                return Fail(expr, "Managed return value must be owned.");
            }
            if (auto result = ValidateNode(exit.Cast()->Value, scopeId, true); !result) {
                return result;
            }
            for (const auto& cleanup : exit.Cast()->Cleanups) {
                if (auto result = ValidateNode(cleanup, scopeId, false); !result) {
                    return result;
                }
            }
            return {};
        }
        if (auto global = TMaybeNode<TGlobalCleanupExpr>(expr)) {
            for (const auto& cleanup : global.Cast()->Cleanups) {
                if (auto result = ValidateNode(cleanup, scopeId, false); !result) {
                    return result;
                }
            }
            return {};
        }
        if (auto assign = TMaybeNode<TAssignExpr>(expr)) {
            if (auto result = ValidateSynthetic(
                expr,
                assign.Cast()->Name,
                scopeId,
                false); !result)
            {
                return result;
            }
        }

        for (const auto& child : expr->Children()) {
            if (auto result = ValidateNode(child, scopeId, false); !result) {
                return result;
            }
        }
        return {};
    }

    TNameResolver& Context_;
    bool LifetimeMode_ = false;
    std::unordered_set<const TExpr*> Parameters_;
};

} // namespace

TLifetimeValidator::TLifetimeValidator(TNameResolver& context)
    : Context_(context)
{}

std::expected<void, TError> TLifetimeValidator::Validate(const NAst::TExprPtr& root) {
    return TValidator(Context_).Validate(root);
}

} // namespace NSemantics
} // namespace NQumir
