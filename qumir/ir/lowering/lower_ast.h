#pragma once

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/debug_point_emitter.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <qumir/optional.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

class TAstLowerer {
public:
    TAstLowerer(TModule& module, TBuilder& builder, NSemantics::TNameResolver& ctx, TDebugOptions options = {})
        : Module(module)
        , Builder(builder)
        , Context(ctx)
        , DebugPoints_(module, builder)
    {
        Module.DebugOptions = options;
    }

    // With entries, only functions reachable from them or from `used` functions are lowered.
    std::expected<std::monostate, TError> LowerTop(
        const NAst::TExprPtr& expr, const std::unordered_set<std::string>* entries = nullptr);

private:
    TInstrDebugInfo InstDebugInfo(const TLocation& location, int32_t scopeId) const;
    TLocalVarDebugInfo LocalDebugInfo(
        std::string_view name,
        const TLocation& location,
        int32_t scopeId,
        const NAst::TTypePtr& astType = {}) const;

    struct TBlockScope {
        int64_t FuncIdx;
        NSemantics::TScopeId Id;
        std::optional<TLabel> BreakLabel;
        std::optional<TLabel> ContinueLabel;
        std::optional<TLabel> ReturnLabel; // function's exit block, set for every scope inside a function body
        std::optional<TLocal> RetLocal; // holds the return value; unset for void-returning functions
        std::optional<int> RetTypeId; // type of RetLocal when a function returns a value
        NAst::TExprPtr LastAssert; // function epilogue executed after evaluating the return value and before cleanup
    };

    struct TValueWithBlock {
        std::optional<TOperand> Value; // absent => no value
        TLabel ProducingLabel; // label of block that produced Value (or current block if no value)
    };

    struct TArrayLayout {
        std::vector<TOperand> LBounds;
        std::vector<TOperand> DimSizes;
        std::vector<TOperand> Strides;
        TOperand TotalElements;
    };

    TExpectedTask<TValueWithBlock, TError, TLocation> Lower(const NAst::TExprPtr& expr, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerVector(const NAst::TVectorExpr& vector, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerRetain(const NAst::TRetainExpr& retain, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerOwnLiteral(const NAst::TOwnLiteralExpr& literal, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerDestroy(const NAst::TDestroyExpr& destroy, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerReplace(const NAst::TReplaceExpr& replace, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerCleanupExit(const NAst::TCleanupExitExpr& exit, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerGlobalCleanup(const NAst::TGlobalCleanupExpr& cleanup, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerBitcast(const NAst::TBitcastExpr& bitcast, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerCast(const NAst::TCastExpr& cast, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerNumber(const NAst::TNumberExpr& num);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerStringLiteral(const NAst::TStringLiteralExpr& str);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerBlock(const NAst::TBlockExpr& block, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerUnary(const NAst::TUnaryExpr& unary, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerBinary(const NAst::TBinaryExpr& binary, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerIf(const NAst::TIfExpr& ife, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerArrayAssign(const NAst::TArrayAssignExpr& asg, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerIndex(const NAst::TIndexExpr& index, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerStructConstruct(const NAst::TStructConstructExpr& sc, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerMultiIndex(const NAst::TMultiIndexExpr& multiIndex, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerAssign(const NAst::TAssignExpr& asg, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerIdent(const NAst::TIdentExpr& ident, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerVar(const NAst::TVarStmt& var, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerFunction(const NAst::TFunDecl& fun, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerAwait(const NAst::TAwaitExpr& awaitExpr, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerCall(const NAst::TCallExpr& call, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerField(const NAst::TExprPtr& expr, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerUnsupported(const NAst::TExprPtr& expr);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerVectorIntrinsic(
        const NAst::TCallExpr& call, std::string_view op, TBlockScope scope);
    TTmp MaterializeValue(
        TOperand value,
        int typeId,
        const std::string& name,
        const TLocation& loc,
        int32_t scopeId);
    TExpectedTask<TOperand, TError, TLocation> EnsureStructAddress(
        TOperand value,
        int structTypeId,
        const std::string& name,
        const TLocation& loc,
        int32_t scopeId);

    TExpectedTask<TValueWithBlock, TError, TLocation> LowerWhile(std::shared_ptr<NAst::TWhileStmtExpr> loop, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerFor(std::shared_ptr<NAst::TForStmtExpr> loop, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerRepeat(std::shared_ptr<NAst::TRepeatStmtExpr> loop, const TBlockScope& scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerTimes(std::shared_ptr<NAst::TTimesStmtExpr> loop, const TBlockScope& scope);
    TExpectedTask<TArrayLayout, TError, TLocation> LowerArrayLayout(NSemantics::TSymbolInfo symbol, const std::vector<std::pair<NAst::TExprPtr, NAst::TExprPtr>>& bounds, TBlockScope scope, const std::string& name, const TLocation& loc);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerIndices(NSemantics::TSymbolInfo symbol, const std::vector<NAst::TExprPtr>& indices, TBlockScope scope, int elemSize = 8);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerLValueAddress(const NAst::TExprPtr& expr, TBlockScope scope);
    TExpectedTask<std::monostate, TError, TLocation> EmitLifetimeDestroy(
        TOperand value,
        const NAst::TTypePtr& type,
        std::optional<TOperand> aux,
        const TLocation& loc,
        int32_t scopeId);
    TExpectedTask<TTmp, TError, TLocation> LoadVar(const std::string& name, TBlockScope scope, const TLocation& loc, bool ref = false);
    TTmp LoadLayoutOperand(TOperand operand, const TLocation& loc, int32_t scopeId);
    TOperand AllocLayoutStorage(NSemantics::TSymbolInfo symbol, int typeId, const std::string& name, const TLocation& loc);

    void SetLocalType(TLocal local, int typeId, const TLocalVarDebugInfo& info, bool declare = false);
    void Statement(const NAst::TExprPtr& expr, int scopeId);

    void ImportExternalFunction(int symbolId, const NAst::TFunDecl& funcDecl);
    void ImportExternalFunctions();
    TExpectedTask<int, TError, TLocation> GlobalSymbolId(const std::string& name);

    TModule& Module;
    TBuilder& Builder;
    NSemantics::TNameResolver& Context;
    TDebugPointEmitter DebugPoints_;

    std::unordered_map<int32_t, TArrayLayout> ArrayLayouts;
    int32_t NextHiddenGlobalSlot = -1;

    int64_t NextReplChunk = 0;
    int64_t NextLambdaChunk = 0;
};

} // namespace NIR
} // namespace NQumir
