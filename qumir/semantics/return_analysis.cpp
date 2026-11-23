#include <qumir/semantics/return_analysis.h>

namespace NQumir {
namespace NSemantics {

bool AlwaysReturns(const NAst::TExprPtr& expr) {
    if (NAst::TMaybeNode<NAst::TReturnExpr>(expr)) {
        return true;
    }
    if (auto exit = NAst::TMaybeNode<NAst::TCleanupExitExpr>(expr)) {
        return exit.Cast()->Kind == NAst::ECleanupExitKind::Return;
    }
    if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(expr)) {
        for (const auto& stmt : block.Cast()->Stmts) {
            if (AlwaysReturns(stmt)) {
                return true;
            }
        }
        return false;
    }
    if (auto ifExpr = NAst::TMaybeNode<NAst::TIfExpr>(expr)) {
        return ifExpr.Cast()->Else()
            && AlwaysReturns(ifExpr.Cast()->Then())
            && AlwaysReturns(ifExpr.Cast()->Else());
    }
    return false;
}

} // namespace NSemantics
} // namespace NQumir
