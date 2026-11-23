#include "pass.h"

#include <qumir/semantics/return_analysis.h>

namespace NQumir {
namespace NSemantics {

namespace {

using namespace NAst;

bool IsVoidReturn(const TFunDecl& function) {
    const auto& futureResult = FutureResultType(function.RetType);
    const auto& returnType = futureResult ? futureResult : function.RetType;
    if (TMaybeType<TVoidType>(returnType)) {
        return true;
    }
    return false;
}

// Replaces one function's fallthrough with an explicit return.
bool NormalizeFunction(TFunDecl* function) {
    if (!function->BodyExpr() || function->IsExternal() || AlwaysReturns(function->BodyExpr())) {
        return false;
    }

    auto& statements = function->Body()->Stmts;
    if (IsVoidReturn(*function)) {
        statements.push_back(MakeIntrusive<TReturnExpr>(
            function->Body()->Location,
            nullptr));
        return true;
    }

    // Source annotation has already verified that a non-void fallthrough has
    // a value-producing final expression. Preserve that expression's type;
    // final annotation will validate it as the return value.
    if (statements.empty()) {
        return false;
    }
    auto value = std::move(statements.back());
    statements.back() = MakeIntrusive<TReturnExpr>(
        value->Location,
        std::move(value));
    return true;
}

// Walks the AST and normalizes every non-external function declaration.
bool NormalizeFunctions(const TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (auto maybeFunction = TMaybeNode<TFunDecl>(expr)) {
        // Nested declarations in the body are visited independently below.
        auto function = maybeFunction.Cast();
        bool changed = NormalizeFunction(function);
        TExprPtr body = function->BodyExpr();
        changed = NormalizeFunctions(body) || changed;
        function->BodyExpr() = TExprPtrOf<TBlockExpr>(TMaybeNode<TBlockExpr>(body).Cast());
        return changed;
    }

    bool changed = false;
    for (auto child : expr->Children()) {
        changed = NormalizeFunctions(child) || changed;
    }
    return changed;
}

} // namespace

std::expected<bool, TError> ReturnNormalizationPass(
    NAst::TExprPtr& expr,
    TNameResolver& context)
{
    (void)context;
    return NormalizeFunctions(expr);
}

} // namespace NSemantics
} // namespace NQumir
