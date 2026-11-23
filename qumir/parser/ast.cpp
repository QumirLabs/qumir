#include "ast.h"

#include <stdexcept>

namespace NQumir {
namespace NAst {

TExprPtr ShallowCloneNode(const TExprPtr& node) {
    if (auto n = TMaybeNode<TIdentExpr>(node)) {
        return MakeIntrusive<TIdentExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TAssignExpr>(node)) {
        return MakeIntrusive<TAssignExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TArrayAssignExpr>(node)) {
        return MakeIntrusive<TArrayAssignExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TNumberExpr>(node)) {
        return MakeIntrusive<TNumberExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TStringLiteralExpr>(node)) {
        return MakeIntrusive<TStringLiteralExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TUnaryExpr>(node)) {
        return MakeIntrusive<TUnaryExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TBinaryExpr>(node)) {
        return MakeIntrusive<TBinaryExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TBlockExpr>(node)) {
        return MakeIntrusive<TBlockExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TIfExpr>(node)) {
        return MakeIntrusive<TIfExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TWhileStmtExpr>(node)) {
        return MakeIntrusive<TWhileStmtExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TRepeatStmtExpr>(node)) {
        return MakeIntrusive<TRepeatStmtExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TForStmtExpr>(node)) {
        return MakeIntrusive<TForStmtExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TTimesStmtExpr>(node)) {
        return MakeIntrusive<TTimesStmtExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TBreakStmt>(node)) {
        return MakeIntrusive<TBreakStmt>(*n.Cast());
    }
    if (auto n = TMaybeNode<TContinueStmt>(node)) {
        return MakeIntrusive<TContinueStmt>(*n.Cast());
    }
    if (auto n = TMaybeNode<TReturnExpr>(node)) {
        return MakeIntrusive<TReturnExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TVarStmt>(node)) {
        return MakeIntrusive<TVarStmt>(*n.Cast());
    }
    if (auto n = TMaybeNode<TVarsBlockExpr>(node)) {
        return MakeIntrusive<TVarsBlockExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TFunDecl>(node)) {
        return MakeIntrusive<TFunDecl>(*n.Cast());
    }
    if (auto n = TMaybeNode<TCallExpr>(node)) {
        return MakeIntrusive<TCallExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TAwaitExpr>(node)) {
        return MakeIntrusive<TAwaitExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TInputExpr>(node)) {
        return MakeIntrusive<TInputExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TOutputExpr>(node)) {
        return MakeIntrusive<TOutputExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TCastExpr>(node)) {
        return MakeIntrusive<TCastExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TBitcastExpr>(node)) {
        return MakeIntrusive<TBitcastExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TIndexExpr>(node)) {
        return MakeIntrusive<TIndexExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TMultiIndexExpr>(node)) {
        return MakeIntrusive<TMultiIndexExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TSliceExpr>(node)) {
        return MakeIntrusive<TSliceExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TUseExpr>(node)) {
        return MakeIntrusive<TUseExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TAssertStmt>(node)) {
        return MakeIntrusive<TAssertStmt>(*n.Cast());
    }
    if (auto n = TMaybeNode<TTypeDeclStmt>(node)) {
        return MakeIntrusive<TTypeDeclStmt>(*n.Cast());
    }
    if (auto n = TMaybeNode<TFieldAccessExpr>(node)) {
        return MakeIntrusive<TFieldAccessExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TStructConstructExpr>(node)) {
        return MakeIntrusive<TStructConstructExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TFieldAssignExpr>(node)) {
        return MakeIntrusive<TFieldAssignExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TRetainExpr>(node)) {
        return MakeIntrusive<TRetainExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TOwnLiteralExpr>(node)) {
        return MakeIntrusive<TOwnLiteralExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TMoveExpr>(node)) {
        return MakeIntrusive<TMoveExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TBorrowExpr>(node)) {
        return MakeIntrusive<TBorrowExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TDestroyExpr>(node)) {
        return MakeIntrusive<TDestroyExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TReplaceExpr>(node)) {
        return MakeIntrusive<TReplaceExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TCleanupExitExpr>(node)) {
        return MakeIntrusive<TCleanupExitExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TGlobalCleanupExpr>(node)) {
        return MakeIntrusive<TGlobalCleanupExpr>(*n.Cast());
    }
    if (auto n = TMaybeNode<TVectorExpr>(node)) {
        return MakeIntrusive<TVectorExpr>(*n.Cast());
    }
    return node;
}

TExprPtr DeepCloneExpr(const TExprPtr& node) {
    if (!node) {
        return nullptr;
    }
    auto clone = ShallowCloneNode(node);
    if (clone == node) {
        throw std::logic_error(
            "DeepCloneExpr does not support AST node "
            + std::string(node->NodeName()));
    }
    if (auto block = TMaybeNode<TBlockExpr>(clone)) {
        block.Cast()->Scope = -1;
    } else if (auto function = TMaybeNode<TFunDecl>(clone)) {
        function.Cast()->Scope = -1;
        for (auto& param : function.Cast()->Params) {
            param = StaticPointerCast<TVarStmt>(DeepCloneExpr(param));
        }
    } else if (auto vars = TMaybeNode<TVarsBlockExpr>(clone)) {
        for (auto& var : vars.Cast()->Vars) {
            var = DeepCloneExpr(var);
        }
    } else if (auto use = TMaybeNode<TUseExpr>(clone)) {
        use.Cast()->Resolved = false;
    }
    for (auto& child : clone->MutableChildren()) {
        child = DeepCloneExpr(child);
    }
    return clone;
}

void TIdentExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TAssignExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TArrayAssignExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TNumberExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TStringLiteralExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TUnaryExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TBinaryExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TBlockExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TIfExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TWhileStmtExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TRepeatStmtExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TForStmtExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TTimesStmtExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TBreakStmt::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TContinueStmt::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TReturnExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TVarStmt::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TVarsBlockExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TFunDecl::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TCallExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TAwaitExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TInputExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TOutputExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TCastExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TBitcastExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TIndexExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TMultiIndexExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TSliceExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TUseExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TAssertStmt::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TTypeDeclStmt::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TFieldAccessExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TStructConstructExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TFieldAssignExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TRetainExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TOwnLiteralExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TMoveExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TBorrowExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TDestroyExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TReplaceExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TCleanupExitExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TGlobalCleanupExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }
void TVectorExpr::Accept(IVisitor& visitor) { visitor.Visit(*this); }

} // namespace NAst
} // namespace NQumir
