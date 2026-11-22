#pragma once

#include <qumir/parser/ast.h>
#include <qumir/error.h>
#include <qumir/optional.h>

#include "qumir/semantics/name_resolution/name_resolver.h"

namespace NQumir {
namespace NSemantics {

class TDefiniteAssignmentChecker {
public:
    explicit TDefiniteAssignmentChecker(TNameResolver& context);

    std::expected<void, TError> Check(const NAst::TExprPtr& root);

private:
    using TAssignedSet = std::unordered_set<int>; // assigned symbol ids

    std::expected<TAssignedSet, TError> CheckExpr(
        const NAst::TExprPtr& expr,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckBlock(
        const NAst::TBlockExpr& block,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckIfExpr(
        const NAst::TIfExpr& ifExpr,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);
    std::expected<TAssignedSet, TError> CheckWhile(
        const NAst::TWhileStmtExpr& loop,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);
    std::expected<TAssignedSet, TError> CheckRepeat(
        const NAst::TRepeatStmtExpr& loop,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);
    std::expected<TAssignedSet, TError> CheckFor(
        const NAst::TForStmtExpr& loop,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);
    std::expected<TAssignedSet, TError> CheckTimes(
        const NAst::TTimesStmtExpr& loop,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckAssign(
        const NAst::TAssignExpr& assign,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckVar(
        const NAst::TVarStmt& var,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckIdent(
        const NAst::TIdentExpr& ident,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckFunDecl(
        const NAst::TFunDecl& funDecl,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    std::expected<TAssignedSet, TError> CheckCall(
        const NAst::TCallExpr& call,
        TScopeId scopeId,
        const TAssignedSet& inAssigned);

    static TAssignedSet Intersect(const TAssignedSet& a, const TAssignedSet& b);
    static TAssignedSet Union(const TAssignedSet& a, const TAssignedSet& b);

    std::expected<int, TError> GetSymbolId(
        const std::string& name,
        TScopeId scopeId,
        const NAst::TExpr& where);
    std::expected<NAst::TExpr*, TError> GetSymbolNode(
        const std::string& name,
        TScopeId scopeId,
        const NAst::TExpr& where);

    TNameResolver& Context;
    TAssignedSet GlobalAssigned;
};

} // namespace NSemantics
} // namespace NQumir
