#include "pass.h"

#include <string_view>

namespace NQumir {
namespace NSemantics {
namespace {

using namespace NAst;

enum class EValueOwnership {
    NotApplicable,
    Borrowed,
    Owned,
    Literal,
};

TTypePtr ValueType(TTypePtr type) {
    while (true) {
        if (auto reference = TMaybeType<TReferenceType>(type)) {
            type = reference.Cast()->ReferencedType;
        } else if (auto named = TMaybeType<TNamedType>(type)) {
            type = named.Cast()->UnderlyingType;
        } else {
            return type;
        }
    }
}

bool IsString(const TTypePtr& type) {
    return static_cast<bool>(TMaybeType<TStringType>(ValueType(type)));
}

TMaybeType<TArrayType> AsArray(const TTypePtr& type) {
    return TMaybeType<TArrayType>(ValueType(type));
}

bool IsStringArray(const TTypePtr& type) {
    auto array = AsArray(type);
    return array && IsString(array.Cast()->ElementType);
}

TExprPtr Integer(const TLocation& location, int64_t value) {
    auto result = std::make_shared<TNumberExpr>(location, value);
    result->Type = std::make_shared<TIntegerType>();
    return result;
}

TExprPtr Binary(
    const TLocation& location,
    TOperator op,
    TExprPtr left,
    TExprPtr right)
{
    auto result = std::make_shared<TBinaryExpr>(
        location,
        op,
        std::move(left),
        std::move(right));
    result->Type = std::make_shared<TIntegerType>();
    return result;
}

// In elements, not bytes: the width of an element belongs to the target.
TExprPtr ArrayAllocationSize(const TVarStmt& variable) {
    TExprPtr elements = Integer(variable.Location, 1);
    for (const auto& [lower, upper] : variable.Bounds) {
        auto dimension = Binary(
            variable.Location,
            '+',
            Binary(variable.Location, '-', upper, lower),
            Integer(variable.Location, 1));
        elements = Binary(
            variable.Location,
            '*',
            std::move(elements),
            std::move(dimension));
    }
    return elements;
}

bool IsReusableArrayBound(const TExprPtr& expr) {
    return TMaybeNode<TNumberExpr>(expr) || TMaybeNode<TIdentExpr>(expr);
}

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

bool IsSynthetic(std::string_view name) {
    return name.starts_with("__lifetime_");
}

std::expected<EValueOwnership, TError> ClassifyOwnership(const TExprPtr& expr) {
    if (!expr || !IsString(expr->Type)) {
        return EValueOwnership::NotApplicable;
    }
    if (TMaybeNode<TStringLiteralExpr>(expr)) {
        return EValueOwnership::Literal;
    }
    if (TMaybeNode<TIdentExpr>(expr)
        || TMaybeNode<TIndexExpr>(expr)
        || TMaybeNode<TMultiIndexExpr>(expr)
        || TMaybeNode<TFieldAccessExpr>(expr)
        || TMaybeNode<TBorrowExpr>(expr))
    {
        return EValueOwnership::Borrowed;
    }
    if (TMaybeNode<TCallExpr>(expr)
        || TMaybeNode<TAwaitExpr>(expr)
        || TMaybeNode<TRetainExpr>(expr)
        || TMaybeNode<TOwnLiteralExpr>(expr)
        || TMaybeNode<TMoveExpr>(expr))
    {
        return EValueOwnership::Owned;
    }
    if (auto block = TMaybeNode<TBlockExpr>(expr)) {
        if (block.Cast()->Stmts.empty()) {
            return EValueOwnership::NotApplicable;
        }
        return ClassifyOwnership(block.Cast()->Stmts.back());
    }
    return std::unexpected(TError(
        expr->Location,
        "Cannot determine ownership of string expression '"
            + std::string(expr->NodeName()) + "'."));
}

std::expected<TExprPtr, TError> RequireOwned(TExprPtr value) {
    auto ownership = ClassifyOwnership(value);
    if (!ownership) {
        return std::unexpected(ownership.error());
    }
    auto loc = value->Location;
    switch (*ownership) {
    case EValueOwnership::Literal:
        return std::make_shared<TOwnLiteralExpr>(loc, std::move(value));
    case EValueOwnership::Borrowed:
        return std::make_shared<TRetainExpr>(
            loc,
            std::make_shared<TBorrowExpr>(loc, std::move(value)));
    case EValueOwnership::Owned:
        return std::make_shared<TMoveExpr>(loc, std::move(value));
    case EValueOwnership::NotApplicable:
        return std::unexpected(TError(
            loc,
            "Owned string value was required for a non-string expression."));
    }
    return std::unexpected(TError(loc, "Unknown string ownership."));
}

TExprPtr MakeNullString(const TLocation& location, const TTypePtr& type) {
    auto zero = std::make_shared<TNumberExpr>(location, int64_t{0});
    zero->Type = std::make_shared<TIntegerType>();
    return std::make_shared<TBitcastExpr>(location, std::move(zero), type);
}

bool IsNullString(const TExprPtr& expr) {
    if (auto number = TMaybeNode<TNumberExpr>(expr)) {
        return IsString(number.Cast()->Type)
            && !number.Cast()->IsFloat()
            && number.Cast()->IntValue == 0;
    }
    auto bitcast = TMaybeNode<TBitcastExpr>(expr);
    if (!bitcast || !IsString(bitcast.Cast()->Type)) {
        return false;
    }
    auto number = TMaybeNode<TNumberExpr>(bitcast.Cast()->Operand);
    return number && !number.Cast()->IsFloat() && number.Cast()->IntValue == 0;
}

class TLifetimeRewriter {
public:
    TLifetimeRewriter(
        TNameResolver& context,
        TSyntheticNameGenerator& syntheticNames)
        : Context_(context)
        , SyntheticNames_(syntheticNames)
    {}

    std::expected<bool, TError> Rewrite(TExprPtr& root) {
        if (auto block = TMaybeNode<TBlockExpr>(root)) {
            size_t globalCleanupCount = 0;
            for (const auto& statement : block.Cast()->Stmts) {
                if (TMaybeNode<TGlobalCleanupExpr>(statement)) {
                    ++globalCleanupCount;
                }
            }
            if (globalCleanupCount > 1) {
                return std::unexpected(TError(
                    root->Location,
                    "Only one global cleanup node is allowed."));
            }
        }
        // Serialized core AST can re-enter the final pipeline. Any lifetime
        // node is a completion marker: the pass must never rewrite it twice.
        if (ContainsLifetimeNode(root)) {
            return false;
        }
        auto result = RewriteExpr(root, TScopeId{0}, false);
        if (!result) {
            return result;
        }
        if (GlobalLocals_.empty()) {
            return result;
        }
        auto block = TMaybeNode<TBlockExpr>(root);
        if (!block) {
            return std::unexpected(TError(
                root->Location,
                "Lifetime pass requires a root block for global cleanup."));
        }
        std::vector<TExprPtr> cleanups;
        cleanups.reserve(GlobalLocals_.size());
        for (auto local = GlobalLocals_.rbegin();
             local != GlobalLocals_.rend();
             ++local)
        {
            cleanups.push_back(MakeDestroy(*local));
        }
        block.Cast()->Stmts.push_back(std::make_shared<TGlobalCleanupExpr>(
            root->Location,
            std::move(cleanups)));
        return true;
    }

private:
    struct TLocal {
        TLocation Location;
        std::string Name;
        TTypePtr Type;
        std::optional<std::string> AuxName;
    };

    struct TLifetimeScope {
        std::vector<TLocal> Locals;
    };

    TExprPtr MakeDestroy(const TLocal& local) const {
        auto value = std::make_shared<TIdentExpr>(local.Location, local.Name);
        value->Type = local.Type;
        TExprPtr aux;
        if (local.AuxName) {
            aux = std::make_shared<TIdentExpr>(local.Location, *local.AuxName);
            aux->Type = std::make_shared<TIntegerType>();
        }
        return std::make_shared<TDestroyExpr>(
            local.Location,
            std::move(value),
            std::move(aux));
    }

    // Builds cleanup actions for active scopes in lexical LIFO order.
    std::vector<TExprPtr> MakeCleanups(size_t firstScope) const {
        std::vector<TExprPtr> cleanups;
        for (size_t scopeIndex = Scopes_.size(); scopeIndex-- > firstScope; ) {
            const auto& locals = Scopes_[scopeIndex].Locals;
            for (auto local = locals.rbegin(); local != locals.rend(); ++local) {
                cleanups.push_back(MakeDestroy(*local));
            }
        }
        return cleanups;
    }

    // Detects exits after which the current lexical block cannot fall through.
    bool AlwaysExits(const TExprPtr& expr) const {
        if (TMaybeNode<TCleanupExitExpr>(expr)) {
            return true;
        }
        if (auto block = TMaybeNode<TBlockExpr>(expr)) {
            for (const auto& statement : block.Cast()->Stmts) {
                if (AlwaysExits(statement)) {
                    return true;
                }
            }
            return false;
        }
        if (auto branch = TMaybeNode<TIfExpr>(expr)) {
            return branch.Cast()->Else
                && AlwaysExits(branch.Cast()->Then)
                && AlwaysExits(branch.Cast()->Else);
        }
        return false;
    }

    // Converts a source return into a value-preserving structured cleanup exit.
    std::expected<bool, TError> RewriteReturn(
        TExprPtr& expr,
        const std::shared_ptr<TReturnExpr>& returnExpr,
        TScopeId scopeId)
    {
        if (!FunctionBoundary_) {
            return std::unexpected(TError(returnExpr->Location, "`return` outside function."));
        }
        if (returnExpr->Value) {
            auto rewritten = RewriteExpr(returnExpr->Value, scopeId, true);
            if (!rewritten) {
                return std::unexpected(rewritten.error());
            }
        }

        auto cleanups = MakeCleanups(*FunctionBoundary_);
        TExprPtr value = std::move(returnExpr->Value);
        if (value && IsString(value->Type)) {
            auto owned = RequireOwned(std::move(value));
            if (!owned) {
                return std::unexpected(owned.error());
            }
            const auto resultName = SyntheticNames_.Next();
            auto result = std::make_shared<TVarStmt>(
                returnExpr->Location,
                resultName,
                std::make_shared<TStringType>());
            result->Init = std::move(owned.value());
            auto resultValue = std::make_shared<TIdentExpr>(
                returnExpr->Location,
                resultName);
            resultValue->Type = result->Type;
            auto exit = std::make_shared<TCleanupExitExpr>(
                returnExpr->Location,
                ECleanupExitKind::Return,
                std::make_shared<TMoveExpr>(
                    returnExpr->Location,
                    std::move(resultValue)),
                std::move(cleanups));
            auto wrapper = std::make_shared<TBlockExpr>(
                returnExpr->Location,
                std::vector<TExprPtr>{std::move(result), std::move(exit)});
            wrapper->Type = std::make_shared<TVoidType>();
            expr = std::move(wrapper);
            return true;
        }

        expr = std::make_shared<TCleanupExitExpr>(
            returnExpr->Location,
            ECleanupExitKind::Return,
            std::move(value),
            std::move(cleanups));
        return true;
    }

    // Converts break/continue into an exit carrying only iteration-local cleanup.
    std::expected<bool, TError> RewriteLoopExit(
        TExprPtr& expr,
        ECleanupExitKind kind)
    {
        if (LoopBoundaries_.empty()) {
            return std::unexpected(TError(expr->Location, "Loop exit outside loop."));
        }
        expr = std::make_shared<TCleanupExitExpr>(
            expr->Location,
            kind,
            nullptr,
            MakeCleanups(LoopBoundaries_.back()));
        return true;
    }

    // Rewrites loop operands normally and gives its body an iteration boundary.
    std::expected<bool, TError> RewriteLoop(
        std::vector<TExprPtr*> operands,
        TExprPtr& body,
        TScopeId scopeId)
    {
        bool changed = false;
        for (auto* operand : operands) {
            auto result = RewriteExpr(*operand, scopeId, true);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = result.value() || changed;
        }
        LoopBoundaries_.push_back(Scopes_.size());
        auto result = RewriteExpr(body, scopeId, false);
        LoopBoundaries_.pop_back();
        if (!result) {
            return std::unexpected(result.error());
        }
        return result.value() || changed;
    }

    std::expected<bool, TError> RewriteBlock(
        const std::shared_ptr<TBlockExpr>& block,
        TScopeId parentScope,
        bool resultUsed)
    {
        auto scopeId = block->Scope >= 0
            ? TScopeId{block->Scope}
            : parentScope;
        Scopes_.push_back({});
        bool changed = false;
        std::vector<TExprPtr> statements;
        statements.reserve(block->Stmts.size());
        for (size_t statementIndex = 0;
             statementIndex < block->Stmts.size();
             ++statementIndex)
        {
            auto& statement = block->Stmts[statementIndex];
            const bool statementResultUsed = resultUsed
                && statementIndex + 1 == block->Stmts.size();
            if (auto variable = TMaybeNode<TVarStmt>(statement);
                variable && IsSynthetic(variable.Cast()->Name))
            {
                if (variable.Cast()->Init) {
                    auto result = RewriteExpr(
                        variable.Cast()->Init,
                        scopeId,
                        true);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    changed = result.value() || changed;
                }
                statements.push_back(statement);
                continue;
            }
            if (auto variable = TMaybeNode<TVarStmt>(statement);
                variable && AsArray(variable.Cast()->Type))
            {
                for (auto& [lower, upper] : variable.Cast()->Bounds) {
                    for (auto* bound : {&lower, &upper}) {
                        auto result = RewriteExpr(*bound, scopeId, true);
                        if (!result) {
                            return std::unexpected(result.error());
                        }
                        changed = result.value() || changed;
                        if (IsStringArray(variable.Cast()->Type)
                            && !IsReusableArrayBound(*bound))
                        {
                            const auto boundName = SyntheticNames_.Next();
                            const auto boundLocation = (*bound)->Location;
                            auto boundValue = std::make_shared<TVarStmt>(
                                boundLocation,
                                boundName,
                                std::make_shared<TIntegerType>());
                            if (FunctionBoundary_) {
                                boundValue->Init = std::move(*bound);
                            }
                            statements.push_back(std::move(boundValue));
                            if (!FunctionBoundary_) {
                                statements.push_back(std::make_shared<TAssignExpr>(
                                    boundLocation,
                                    boundName,
                                    std::move(*bound)));
                            }
                            auto savedBound = std::make_shared<TIdentExpr>(
                                variable.Cast()->Location,
                                boundName);
                            savedBound->Type = std::make_shared<TIntegerType>();
                            *bound = std::move(savedBound);
                            changed = true;
                        }
                    }
                }
                statements.push_back(statement);

                std::optional<std::string> auxName;
                if (IsStringArray(variable.Cast()->Type)) {
                    auxName = SyntheticNames_.Next();
                    auto allocationSize = std::make_shared<TVarStmt>(
                        variable.Cast()->Location,
                        *auxName,
                        std::make_shared<TIntegerType>());
                    if (FunctionBoundary_) {
                        allocationSize->Init = ArrayAllocationSize(*variable.Cast());
                    }
                    statements.push_back(std::move(allocationSize));
                    if (!FunctionBoundary_) {
                        statements.push_back(std::make_shared<TAssignExpr>(
                            variable.Cast()->Location,
                            *auxName,
                            ArrayAllocationSize(*variable.Cast())));
                    }
                    changed = true;
                }
                if (FunctionBoundary_) {
                    Scopes_.back().Locals.push_back({
                        variable.Cast()->Location,
                        variable.Cast()->Name,
                        variable.Cast()->Type,
                        std::move(auxName),
                    });
                } else {
                    GlobalLocals_.push_back({
                        variable.Cast()->Location,
                        variable.Cast()->Name,
                        variable.Cast()->Type,
                        std::move(auxName),
                    });
                }
                continue;
            }
            if (auto variable = TMaybeNode<TVarStmt>(statement);
                variable
                && IsString(variable.Cast()->Type))
            {
                if (IsNullString(variable.Cast()->Init)) {
                    statements.push_back(statement);
                    if (FunctionBoundary_) {
                        Scopes_.back().Locals.push_back({
                            variable.Cast()->Location,
                            variable.Cast()->Name,
                            variable.Cast()->Type,
                        });
                    } else {
                        GlobalLocals_.push_back({
                            variable.Cast()->Location,
                            variable.Cast()->Name,
                            variable.Cast()->Type,
                        });
                    }
                    continue;
                }
                auto init = std::move(variable.Cast()->Init);
                variable.Cast()->Init = MakeNullString(
                    variable.Cast()->Location,
                    variable.Cast()->Type);
                statements.push_back(statement);
                changed = true;
                if (init) {
                    if (auto result = RewriteExpr(init, scopeId, true); !result) {
                        return std::unexpected(result.error());
                    } else {
                        changed = result.value() || changed;
                    }
                    auto owned = RequireOwned(std::move(init));
                    if (!owned) {
                        return std::unexpected(owned.error());
                    }
                    auto target = std::make_shared<TIdentExpr>(
                        variable.Cast()->Location,
                        variable.Cast()->Name);
                    target->Type = variable.Cast()->Type;
                    statements.push_back(std::make_shared<TReplaceExpr>(
                        variable.Cast()->Location,
                        std::move(target),
                        std::move(owned.value())));
                }
                if (FunctionBoundary_) {
                    Scopes_.back().Locals.push_back({
                        variable.Cast()->Location,
                        variable.Cast()->Name,
                        variable.Cast()->Type,
                    });
                } else {
                    GlobalLocals_.push_back({
                        variable.Cast()->Location,
                        variable.Cast()->Name,
                        variable.Cast()->Type,
                    });
                }
                continue;
            }

            if (auto result = RewriteExpr(
                statement,
                scopeId,
                statementResultUsed); !result)
            {
                return std::unexpected(result.error());
            } else {
                changed = result.value() || changed;
            }
            if (!statementResultUsed) {
                auto ownership = ClassifyOwnership(statement);
                if (!ownership) {
                    return std::unexpected(ownership.error());
                }
                if (*ownership == EValueOwnership::Owned) {
                    auto loc = statement->Location;
                    statement = std::make_shared<TDestroyExpr>(
                        loc,
                        std::make_shared<TMoveExpr>(
                            loc,
                            std::move(statement)));
                    changed = true;
                }
            }
            statements.push_back(statement);
        }
        const bool fallsThrough = !AlwaysExits(
            std::make_shared<TBlockExpr>(block->Location, statements));
        if (FunctionBoundary_ && fallsThrough) {
            auto cleanups = MakeCleanups(Scopes_.size() - 1);
            if (!cleanups.empty()) {
                statements.insert(
                    statements.end(),
                    std::make_move_iterator(cleanups.begin()),
                    std::make_move_iterator(cleanups.end()));
                changed = true;
            }
        }
        block->Stmts = std::move(statements);
        Scopes_.pop_back();
        return changed;
    }

    std::expected<bool, TError> RewriteAssign(
        TExprPtr& expr,
        const std::shared_ptr<TAssignExpr>& assign,
        TScopeId scopeId)
    {
        bool changed = false;
        if (auto result = RewriteExpr(assign->Value, scopeId, true); !result) {
            return std::unexpected(result.error());
        } else {
            changed = result.value();
        }
        auto symbol = Context_.Lookup(assign->Name, scopeId);
        if (!symbol) {
            return std::unexpected(TError(
                assign->Location,
                "Cannot resolve assignment target '" + assign->Name + "'."));
        }
        auto declaration = Context_.GetSymbolNode(TSymbolId{symbol->Id});
        if (!declaration || !IsString(declaration->Type)) {
            return changed;
        }
        auto owned = RequireOwned(std::move(assign->Value));
        if (!owned) {
            return std::unexpected(owned.error());
        }
        auto target = std::make_shared<TIdentExpr>(assign->Location, assign->Name);
        target->Type = declaration->Type;
        expr = std::make_shared<TReplaceExpr>(
            assign->Location,
            std::move(target),
            std::move(owned.value()));
        return true;
    }

    std::expected<bool, TError> RewriteArrayAssign(
        TExprPtr& expr,
        const std::shared_ptr<TArrayAssignExpr>& assign,
        TScopeId scopeId)
    {
        bool changed = false;
        for (auto& index : assign->Indices) {
            auto result = RewriteExpr(index, scopeId, true);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = result.value() || changed;
        }
        if (auto result = RewriteExpr(assign->Value, scopeId, true); !result) {
            return std::unexpected(result.error());
        } else {
            changed = result.value() || changed;
        }

        auto symbol = Context_.Lookup(assign->Name, scopeId);
        if (!symbol) {
            return std::unexpected(TError(
                assign->Location,
                "Cannot resolve indexed assignment target '" + assign->Name + "'."));
        }
        auto declaration = Context_.GetSymbolNode(TSymbolId{symbol->Id});
        auto collectionType = declaration
            ? ValueType(declaration->Type)
            : nullptr;
        TTypePtr elementType;
        if (auto array = TMaybeType<TArrayType>(collectionType)) {
            elementType = array.Cast()->ElementType;
        } else if (auto pointer = TMaybeType<TPointerType>(collectionType)) {
            elementType = pointer.Cast()->PointeeType;
        }
        if (!IsString(elementType)) {
            return changed;
        }

        auto collection = std::make_shared<TIdentExpr>(assign->Location, assign->Name);
        collection->Type = declaration->Type;
        TExprPtr target;
        if (assign->Indices.size() == 1) {
            target = std::make_shared<TIndexExpr>(
                assign->Location,
                std::move(collection),
                std::move(assign->Indices.front()));
        } else {
            target = std::make_shared<TMultiIndexExpr>(
                assign->Location,
                std::move(collection),
                std::move(assign->Indices));
        }
        target->Type = elementType;
        auto owned = RequireOwned(std::move(assign->Value));
        if (!owned) {
            return std::unexpected(owned.error());
        }
        expr = std::make_shared<TReplaceExpr>(
            assign->Location,
            std::move(target),
            std::move(owned.value()));
        return true;
    }

    std::expected<bool, TError> RewriteFieldAssign(
        TExprPtr& expr,
        const std::shared_ptr<TFieldAssignExpr>& assign,
        TScopeId scopeId)
    {
        bool changed = false;
        if (auto result = RewriteExpr(assign->Object, scopeId, true); !result) {
            return std::unexpected(result.error());
        } else {
            changed = result.value() || changed;
        }
        if (auto result = RewriteExpr(assign->Value, scopeId, true); !result) {
            return std::unexpected(result.error());
        } else {
            changed = result.value() || changed;
        }

        auto objectType = ValueType(assign->Object->Type);
        auto structType = TMaybeType<TStructType>(objectType);
        if (!structType
            || assign->FieldIndex < 0
            || static_cast<size_t>(assign->FieldIndex) >= structType.Cast()->Fields.size())
        {
            return changed;
        }
        auto fieldType = structType.Cast()->Fields[assign->FieldIndex].second;
        if (!IsString(fieldType)) {
            return changed;
        }
        auto target = std::make_shared<TFieldAccessExpr>(
            assign->Location,
            std::move(assign->Object),
            assign->FieldName);
        target->FieldIndex = assign->FieldIndex;
        target->Type = fieldType;
        auto owned = RequireOwned(std::move(assign->Value));
        if (!owned) {
            return std::unexpected(owned.error());
        }
        expr = std::make_shared<TReplaceExpr>(
            assign->Location,
            std::move(target),
            std::move(owned.value()));
        return true;
    }

    // `valueNode' is the node whose value the rewritten block yields, and whose
    // type the block takes. It is the call itself everywhere except for a
    // suspending call reached through `await': there the awaited result, not
    // the future, is the value of the block - see RewriteAwaitedCall.
    std::expected<bool, TError> RewriteCall(
        TExprPtr& expr,
        const std::shared_ptr<TCallExpr>& call,
        TScopeId scopeId,
        TExprPtr valueNode = nullptr)
    {
        if (!valueNode) {
            valueNode = call;
        }
        bool changed = false;
        for (auto& argument : call->Args) {
            auto result = RewriteExpr(argument, scopeId, true);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = result.value() || changed;
        }

        auto callee = TMaybeNode<TIdentExpr>(call->Callee);
        if (!callee) {
            return std::unexpected(TError(
                call->Location,
                "Lifetime call rewrite requires an identifier callee."));
        }
        auto symbol = Context_.Lookup(callee.Cast()->Name, scopeId);
        auto declaration = symbol
            ? Context_.GetSymbolNode(TSymbolId{symbol->Id})
            : nullptr;
        auto function = TMaybeNode<TFunDecl>(declaration);
        if (!function) {
            return std::unexpected(TError(
                call->Location,
                "Cannot resolve call target '" + callee.Cast()->Name + "'."));
        }
        if (call->Args.size() != function.Cast()->Params.size()) {
            return std::unexpected(TError(
                call->Location,
                "Call argument count changed before lifetime rewrite."));
        }

        std::vector<TExprPtr> prefix;
        std::vector<TExprPtr> cleanups;
        for (size_t i = 0; i < call->Args.size(); ++i) {
            auto& argument = call->Args[i];
            const auto& parameterType = function.Cast()->Params[i]->Type;
            if (TMaybeType<TReferenceType>(parameterType)
                || !IsString(parameterType))
            {
                continue;
            }

            auto ownership = ClassifyOwnership(argument);
            if (!ownership) {
                return std::unexpected(ownership.error());
            }
            if (*ownership == EValueOwnership::Borrowed) {
                if (!TMaybeNode<TBorrowExpr>(argument)) {
                    auto loc = argument->Location;
                    argument = std::make_shared<TBorrowExpr>(
                        loc,
                        std::move(argument));
                    changed = true;
                }
                continue;
            }
            if (*ownership == EValueOwnership::Literal
                && function.Cast()->IsExternal()
                && !function.Cast()->RequireArgsMaterialization)
            {
                continue;
            }
            if (*ownership != EValueOwnership::Owned
                && *ownership != EValueOwnership::Literal)
            {
                continue;
            }

            auto owned = RequireOwned(std::move(argument));
            if (!owned) {
                return std::unexpected(owned.error());
            }
            const auto name = SyntheticNames_.Next();
            auto temporary = std::make_shared<TVarStmt>(
                call->Location,
                name,
                std::make_shared<TStringType>());
            temporary->Init = std::move(owned.value());
            prefix.push_back(temporary);

            auto borrowedStorage = std::make_shared<TIdentExpr>(
                call->Location,
                name);
            borrowedStorage->Type = parameterType;
            argument = std::make_shared<TBorrowExpr>(
                call->Location,
                std::move(borrowedStorage));

            auto ownedStorage = std::make_shared<TIdentExpr>(
                call->Location,
                name);
            ownedStorage->Type = parameterType;
            cleanups.push_back(std::make_shared<TDestroyExpr>(
                call->Location,
                std::move(ownedStorage)));
            changed = true;
        }

        if (cleanups.empty()) {
            return changed;
        }

        std::vector<TExprPtr> statements = std::move(prefix);
        const bool returnsValue = !TMaybeType<TVoidType>(valueNode->Type);
        if (returnsValue) {
            const auto resultName = SyntheticNames_.Next();
            auto result = std::make_shared<TVarStmt>(
                call->Location,
                resultName,
                valueNode->Type);
            result->Init = valueNode;
            statements.push_back(std::move(result));
            for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
                statements.push_back(*it);
            }
            auto resultStorage = std::make_shared<TIdentExpr>(
                call->Location,
                resultName);
            resultStorage->Type = valueNode->Type;
            if (IsString(valueNode->Type)) {
                statements.push_back(std::make_shared<TMoveExpr>(
                    call->Location,
                    std::move(resultStorage)));
            } else {
                statements.push_back(std::move(resultStorage));
            }
        } else {
            statements.push_back(valueNode);
            for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
                statements.push_back(*it);
            }
        }

        auto wrapper = std::make_shared<TBlockExpr>(
            call->Location,
            std::move(statements));
        wrapper->Type = valueNode->Type;
        expr = std::move(wrapper);
        return true;
    }

    // `await' over a call whose string arguments have to be materialized.
    //
    // Rewriting the call alone would leave the await outside the block the
    // rewrite builds, so the block would have to yield the future through a
    // local of type Future<T> - and IR lowering has no such type, it throws
    // "AST Future<T> cannot be lowered as a regular IR type". Rewrite the await
    // instead: the block then yields the awaited result and the temporaries are
    // destroyed after the operation has actually completed, which is also the
    // safer order.
    //
    //   (await (block (var s = "...") (var r = (call f (borrow s))) (destroy s) r))
    //   ->  (block (var s = "...") (await (call f (borrow s))) (destroy s))
    //
    // The only such function today is `написать' of the Чертёжник module: it is
    // the one external declaration that combines RequireArgsMaterialization
    // with a Future return type.
    std::expected<bool, TError> RewriteAwaitedCall(
        TExprPtr& expr,
        const std::shared_ptr<TAwaitExpr>& await,
        TScopeId scopeId)
    {
        auto call = TMaybeNode<TCallExpr>(await->Operand);
        if (!call) {
            return RewriteChildren(expr, scopeId);
        }
        TExprPtr valueNode = expr;
        return RewriteCall(expr, call.Cast(), scopeId, std::move(valueNode));
    }

    std::expected<bool, TError> RewriteChildren(TExprPtr& expr, TScopeId scopeId)
    {
        bool changed = false;
        for (auto* child : expr->MutableChildren()) {
            if (!*child) {
                continue;
            }
            auto result = RewriteExpr(*child, scopeId, true);
            if (!result) {
                return std::unexpected(result.error());
            }
            changed = result.value() || changed;
        }
        return changed;
    }

    std::expected<bool, TError> RewriteExpr(
        TExprPtr& expr,
        TScopeId scopeId,
        bool resultUsed)
    {
        if (!expr) {
            return false;
        }
        if (auto block = TMaybeNode<TBlockExpr>(expr)) {
            return RewriteBlock(block.Cast(), scopeId, resultUsed);
        }
        if (auto function = TMaybeNode<TFunDecl>(expr)) {
            const auto previousFunctionBoundary = FunctionBoundary_;
            FunctionBoundary_ = Scopes_.size();
            TExprPtr body = function.Cast()->Body;
            auto result = RewriteExpr(
                body,
                TScopeId{function.Cast()->Scope},
                !TMaybeType<TVoidType>(function.Cast()->RetType));
            if (!result) {
                FunctionBoundary_ = previousFunctionBoundary;
                return result;
            }
            function.Cast()->Body = std::static_pointer_cast<TBlockExpr>(body);
            bool changed = result.value();
            if (function.Cast()->LastAssert) {
                auto assertResult = RewriteExpr(
                    function.Cast()->LastAssert,
                    TScopeId{function.Cast()->Scope},
                    false);
                if (!assertResult) {
                    FunctionBoundary_ = previousFunctionBoundary;
                    return assertResult;
                }
                changed = assertResult.value() || changed;
            }
            FunctionBoundary_ = previousFunctionBoundary;
            return changed;
        }
        if (auto returnExpr = TMaybeNode<TReturnExpr>(expr)) {
            return RewriteReturn(expr, returnExpr.Cast(), scopeId);
        }
        if (TMaybeNode<TBreakStmt>(expr)) {
            return RewriteLoopExit(expr, ECleanupExitKind::Break);
        }
        if (TMaybeNode<TContinueStmt>(expr)) {
            return RewriteLoopExit(expr, ECleanupExitKind::Continue);
        }
        if (auto loop = TMaybeNode<TWhileStmtExpr>(expr)) {
            return RewriteLoop({&loop.Cast()->Cond}, loop.Cast()->Body, scopeId);
        }
        if (auto loop = TMaybeNode<TRepeatStmtExpr>(expr)) {
            return RewriteLoop({&loop.Cast()->Cond}, loop.Cast()->Body, scopeId);
        }
        if (auto loop = TMaybeNode<TForStmtExpr>(expr)) {
            return RewriteLoop(
                {&loop.Cast()->From, &loop.Cast()->To, &loop.Cast()->Step},
                loop.Cast()->Body,
                scopeId);
        }
        if (auto loop = TMaybeNode<TTimesStmtExpr>(expr)) {
            return RewriteLoop({&loop.Cast()->Count}, loop.Cast()->Body, scopeId);
        }
        if (auto assign = TMaybeNode<TAssignExpr>(expr)) {
            return RewriteAssign(expr, assign.Cast(), scopeId);
        }
        if (auto assign = TMaybeNode<TArrayAssignExpr>(expr)) {
            return RewriteArrayAssign(expr, assign.Cast(), scopeId);
        }
        if (auto assign = TMaybeNode<TFieldAssignExpr>(expr)) {
            return RewriteFieldAssign(expr, assign.Cast(), scopeId);
        }
        if (auto awaitExpr = TMaybeNode<TAwaitExpr>(expr)) {
            return RewriteAwaitedCall(expr, awaitExpr.Cast(), scopeId);
        }
        if (auto call = TMaybeNode<TCallExpr>(expr)) {
            return RewriteCall(expr, call.Cast(), scopeId);
        }

        return RewriteChildren(expr, scopeId);
    }

    TNameResolver& Context_;
    TSyntheticNameGenerator& SyntheticNames_;
    std::vector<TLifetimeScope> Scopes_;
    std::vector<TLocal> GlobalLocals_;
    std::vector<size_t> LoopBoundaries_;
    std::optional<size_t> FunctionBoundary_;
};

} // namespace

std::expected<bool, TError> LifetimePass(
    NAst::TExprPtr& expr,
    TNameResolver& context,
    TSyntheticNameGenerator& syntheticNames)
{
    return TLifetimeRewriter(context, syntheticNames).Rewrite(expr);
}

} // namespace NSemantics
} // namespace NQumir
