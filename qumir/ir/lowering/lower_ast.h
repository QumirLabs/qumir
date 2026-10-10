#pragma once

#include <qumir/ir/builder.h>
#include <qumir/ir/lowering/debug_point_emitter.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/parser.h>
#include <qumir/semantics/name_resolution/name_resolver.h>

#include <qumir/optional.h>

#include <string_view>
#include <unordered_map>

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

    std::expected<std::monostate, TError> LowerTop(const NAst::TExprPtr& expr);

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

    TExpectedTask<TValueWithBlock, TError, TLocation> Lower(const NAst::TExprPtr& expr, TBlockScope scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerVector(const NAst::TVectorExpr& vector, TBlockScope scope);
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

    TExpectedTask<TValueWithBlock, TError, TLocation> LowerWhile(std::shared_ptr<NAst::TWhileStmtExpr> loop, TBlockScope scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerFor(std::shared_ptr<NAst::TForStmtExpr> loop, TBlockScope scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerRepeat(std::shared_ptr<NAst::TRepeatStmtExpr> loop, TBlockScope scope);
    TExpectedTask<TValueWithBlock, TError, TLocation> LowerTimes(std::shared_ptr<NAst::TTimesStmtExpr> loop, TBlockScope scope);
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
