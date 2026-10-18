#include <qumir/ir/passes/transforms/debug_async.h>
#include "lower_ast.h"
#include <qumir/semantics/vector_intrinsics.h>
#include "qumir/ir/builder.h"
#include "qumir/parser/parser.h"
#include "qumir/parser/core/printer.h"
#include "qumir/parser/type.h"
#include "qumir/semantics/lifetime/validator.h"
#include "qumir/error.h"

#include <iostream>
#include <sstream>
#include <cassert>
#include <functional>
#include <algorithm>
#include <unordered_set>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

namespace {

struct TInstrEmitter {
    TBuilder& Builder;
    TInstrDebugInfo DebugInfo;
    TDebugPointEmitter& Points;
    bool EmitInfo;

    TTmp Emit1(TOp op, std::initializer_list<TOperand> operands) {
        return Emit1(op, std::span<const TOperand>(operands));
    }

    TTmp Emit1(TOp op, std::span<const TOperand> operands) {
        Points.Before(op, DebugInfo);
        auto result = Builder.Emit1(op, operands, EmitInfo ? DebugInfo : TInstrDebugInfo{});
        Points.After(op, DebugInfo);
        return result;
    }

    void Emit0(TOp op, std::initializer_list<TOperand> operands) {
        Points.Before(op, DebugInfo);
        Builder.Emit0(op, operands, EmitInfo ? DebugInfo : TInstrDebugInfo{});
        Points.After(op, DebugInfo);
    }
};

NAst::TTypePtr PhysicalCallResultType(NAst::TTypePtr type)
{
    if (auto futureResult = NAst::FutureResultType(type)) {
        return futureResult;
    }
    return type;
}

bool IsRawArraySymbol(const NAst::TExprPtr& node)
{
    auto var = NAst::TMaybeNode<NAst::TVarStmt>(node);
    if (!var) {
        return false;
    }
    auto type = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(var.Cast()->Type));
    return NAst::TMaybeType<NAst::TArrayType>(type) && var.Cast()->Bounds.empty();
}

std::string RootVariableName(const NAst::TExprPtr& node)
{
    if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(node)) {
        return ident.Cast()->Name;
    }
    if (auto field = NAst::TMaybeNode<NAst::TFieldAccessExpr>(node)) {
        return RootVariableName(field.Cast()->Object);
    }
    if (auto index = NAst::TMaybeNode<NAst::TIndexExpr>(node)) {
        return RootVariableName(index.Cast()->Collection);
    }
    if (auto index = NAst::TMaybeNode<NAst::TMultiIndexExpr>(node)) {
        return RootVariableName(index.Cast()->Collection);
    }
    if (auto cast = NAst::TMaybeNode<NAst::TCastExpr>(node)) {
        return RootVariableName(cast.Cast()->Operand);
    }
    return {};
}

} // namespace

TInstrDebugInfo TAstLowerer::InstDebugInfo(const TLocation& location, int32_t scopeId) const {
    if (!Module.DebugOptions.CollectMetadata()) {
        return {};
    }
    return TInstrDebugInfo{location, scopeId};
}

TLocalVarDebugInfo TAstLowerer::LocalDebugInfo(
    std::string_view name,
    const TLocation& location,
    int32_t scopeId,
    const NAst::TTypePtr& astType) const
{
    if (!Module.DebugOptions.CollectMetadata()) {
        return {};
    }
    return TLocalVarDebugInfo{std::string(name), location, scopeId, astType};
}

void TAstLowerer::SetLocalType(TLocal local, int typeId, const TLocalVarDebugInfo& info, bool declare) {
    Builder.SetType(local, typeId, info);
    if (declare) {
        DebugPoints_.Declare(local, info);
    }
}

void TAstLowerer::Statement(const NAst::TExprPtr& expr, int scopeId) {
    if (!Module.DebugOptions.EmitDebugPoints || expr->Location.Line == 0
        || NAst::TMaybeNode<NAst::TFunDecl>(expr) || NAst::TMaybeNode<NAst::TDestroyExpr>(expr)
        || NAst::TMaybeNode<NAst::TGlobalCleanupExpr>(expr) || NAst::TMaybeNode<NAst::TBlockExpr>(expr))
    {
        return;
    }
    if (auto var = NAst::TMaybeNode<NAst::TVarStmt>(expr); var && (var.Cast()->Name.starts_with('$') || var.Cast()->Name == "знач")) {
        return;
    }
    DebugPoints_.Point("statement", InstDebugInfo(expr->Location, scopeId));
}

TOperand TAstLowerer::AllocLayoutStorage(NSemantics::TSymbolInfo symbol, int typeId, const std::string& name, const TLocation& loc)
{
    if (symbol.FunctionLevelIdx >= 0) {
        return TOperand{Builder.AllocLocal(typeId, LocalDebugInfo(name, loc, symbol.DeclScopeId))};
    }

    if (NextHiddenGlobalSlot < 0) {
        NextHiddenGlobalSlot = static_cast<int32_t>(Context.GetSymbols().size());
    }

    TSlot slot{NextHiddenGlobalSlot++};
    if (Module.GlobalTypes.size() <= static_cast<size_t>(slot.Idx)) {
        Module.GlobalTypes.resize(slot.Idx + 1);
        Module.GlobalValues.resize(slot.Idx + 1);
    }
    Module.GlobalTypes[slot.Idx] = typeId;
    return TOperand{slot};
}

TTmp TAstLowerer::MaterializeValue(
    TOperand value,
    int typeId,
    const std::string& name,
    const TLocation& loc,
    int32_t scopeId)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scopeId), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto local = Builder.AllocLocal(typeId, LocalDebugInfo(name, loc, scopeId));
    emitter.Emit0("stre"_op, {local, value});
    auto address = emitter.Emit1("lea"_op, {local});
    Builder.SetType(address, Module.Types.Ptr(typeId));
    return address;
}

TExpectedTask<TOperand, TError, TLocation> TAstLowerer::EnsureStructAddress(
    TOperand value,
    int structTypeId,
    const std::string& name,
    const TLocation& loc,
    int32_t scopeId)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scopeId), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    if (Module.Types.GetKind(structTypeId) != EKind::Struct) {
        co_return TError(loc, "Внутренняя ошибка: ожидается структурный тип.");
    }
    if (value.Type != TOperand::EType::Tmp) {
        co_return TError(loc, "Внутренняя ошибка: значение структуры должно быть временным значением.");
    }

    int ptrTypeId = Module.Types.Ptr(structTypeId);
    int valueTypeId = Builder.GetType(value.Tmp);
    if (valueTypeId == ptrTypeId) {
        co_return value;
    }
    if (Module.Types.GetKind(valueTypeId) == EKind::Ptr) {
        auto typedPtr = emitter.Emit1("mov"_op, {value});
        Builder.SetType(typedPtr, ptrTypeId);
        co_return TOperand{typedPtr};
    }
    if (valueTypeId != structTypeId) {
        co_return TError(loc, "Внутренняя ошибка: значение структуры имеет несовместимый IR-тип.");
    }

    co_return TOperand{MaterializeValue(value, structTypeId, name, loc, scopeId)};
}

TTmp TAstLowerer::LoadLayoutOperand(TOperand operand, const TLocation& loc, int32_t scopeId)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scopeId), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto tmp = emitter.Emit1("load"_op, {operand});
    Builder.SetType(tmp, Module.Types.I(EKind::I64));
    return tmp;
}

TExpectedTask<TAstLowerer::TArrayLayout, TError, TLocation> TAstLowerer::LowerArrayLayout(
    NSemantics::TSymbolInfo symbol,
    const std::vector<std::pair<NAst::TExprPtr, NAst::TExprPtr>>& bounds,
    TBlockScope scope,
    const std::string& name,
    const TLocation& loc)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    if (bounds.empty()) {
        co_return TError(loc, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
    }

    auto i64 = Module.Types.I(EKind::I64);
    TArrayLayout layout;
    layout.LBounds.resize(bounds.size());
    layout.DimSizes.resize(bounds.size());
    layout.Strides.resize(bounds.size());

    TOperand prevStride = TImm{1, i64};
    for (int i = static_cast<int>(bounds.size()) - 1; i >= 0; --i) {
        const auto& [lboundExpr, rboundExpr] = bounds[i];
        auto lboundValue = co_await Lower(lboundExpr, scope);
        if (!lboundValue.Value) {
            co_return TError(lboundExpr->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
        }
        auto rboundValue = co_await Lower(rboundExpr, scope);
        if (!rboundValue.Value) {
            co_return TError(rboundExpr->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
        }

        auto lboundStorage = AllocLayoutStorage(symbol, i64, "$lbound" + std::to_string(i) + "_" + name, loc);
        auto dimSizeStorage = AllocLayoutStorage(symbol, i64, "$dimSize" + std::to_string(i) + "_" + name, loc);
        auto strideStorage = AllocLayoutStorage(symbol, i64, "$stride" + std::to_string(i) + "_" + name, loc);

        emitter.Emit0("stre"_op, {lboundStorage, *lboundValue.Value});

        auto dimDiff = emitter.Emit1("-"_op, {*rboundValue.Value, *lboundValue.Value});
        Builder.SetType(dimDiff, i64);
        auto dimSize = emitter.Emit1("+"_op, {dimDiff, TImm{1, i64}});
        Builder.SetType(dimSize, i64);
        emitter.Emit0("stre"_op, {dimSizeStorage, dimSize});

        auto stride = emitter.Emit1("*"_op, {prevStride, dimSize});
        Builder.SetType(stride, i64);
        emitter.Emit0("stre"_op, {strideStorage, stride});

        layout.LBounds[i] = lboundStorage;
        layout.DimSizes[i] = dimSizeStorage;
        layout.Strides[i] = strideStorage;
        prevStride = stride;
    }

    layout.TotalElements = layout.Strides[0];
    ArrayLayouts[symbol.Id] = layout;
    co_return layout;
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerWhile(std::shared_ptr<NAst::TWhileStmtExpr> loop, TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loop->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto entryId = Builder.CurrentBlockIdx();
    auto [condLabel, condId] = Builder.NewBlock();
    auto [bodyLabel, bodyId] = Builder.NewBlock();
    auto endLabel = Builder.NewLabel();

    Builder.SetCurrentBlock(entryId);
    emitter.Emit0("jmp"_op, {condLabel});

    Builder.SetCurrentBlock(condId);
    Statement(loop, scope.Id.Id);
    auto cond = co_await Lower(loop->Cond, scope);
    if (!cond.Value) co_return TError(loop->Cond->Location, TErrorString::Get<EErrorId::WHILE_CONDITION_NOT_NUMBER>());
    emitter.Emit0("cmp"_op, {*cond.Value, bodyLabel, endLabel});

    Builder.SetCurrentBlock(bodyId);
    co_await Lower(loop->Body, TBlockScope {
        .FuncIdx = scope.FuncIdx,
        .Id = scope.Id,
        .BreakLabel = endLabel,
        .ContinueLabel = condLabel,
        .ReturnLabel = scope.ReturnLabel,
        .RetLocal = scope.RetLocal,
        .RetTypeId = scope.RetTypeId,
        .LastAssert = scope.LastAssert
    });
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {condLabel});
    }

    Builder.NewBlock(endLabel);
    co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerRepeat(std::shared_ptr<NAst::TRepeatStmtExpr> loop, TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loop->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto entryId = Builder.CurrentBlockIdx();
    auto [bodyLabel, bodyId] = Builder.NewBlock();
    auto [condLabel, condId] = Builder.NewBlock();
    auto endLabel = Builder.NewLabel();

    Builder.SetCurrentBlock(entryId);
    emitter.Emit0("jmp"_op, {bodyLabel});

    Builder.SetCurrentBlock(bodyId);
    co_await Lower(loop->Body, TBlockScope {
        .FuncIdx = scope.FuncIdx,
        .Id = scope.Id,
        .BreakLabel = endLabel,
        .ContinueLabel = condLabel,
        .ReturnLabel = scope.ReturnLabel,
        .RetLocal = scope.RetLocal,
        .RetTypeId = scope.RetTypeId,
        .LastAssert = scope.LastAssert
    });
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {condLabel});
    }

    Builder.SetCurrentBlock(condId);
    Statement(loop, scope.Id.Id);
    auto cond = co_await Lower(loop->Cond, scope);
    if (!cond.Value) co_return TError(loop->Cond->Location, TErrorString::Get<EErrorId::REPEAT_CONDITION_NOT_NUMBER>());
    emitter.Emit0("cmp"_op, {*cond.Value, bodyLabel, endLabel});

    Builder.NewBlock(endLabel);
    co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerFor(std::shared_ptr<NAst::TForStmtExpr> loop, TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loop->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    const auto i64 = Module.Types.I(EKind::I64);

    auto sidOpt = Context.Lookup(loop->VarName, scope.Id);
    if (!sidOpt) {
        co_return TError(loop->Location, TErrorString::Get<EErrorId::ASSIGNMENT_TO_UNDEFINED>());
    }
    TOperand varOperand = sidOpt->FunctionLevelIdx >= 0
        ? TOperand{TLocal{sidOpt->FunctionLevelIdx}}
        : TOperand{TSlot{sidOpt->Id}};
    if (sidOpt->FunctionLevelIdx >= 0) {
        auto declaration = Context.GetSymbolNode(NSemantics::TSymbolId{sidOpt->Id});
        SetLocalType(TLocal{sidOpt->FunctionLevelIdx}, i64,
            LocalDebugInfo(loop->VarName, declaration->Location, sidOpt->DeclScopeId, declaration->Type), true);
    }

    auto toLocal = Builder.AllocLocal(i64, LocalDebugInfo("$to", loop->Location, scope.Id.Id));
    auto stepLocal = Builder.AllocLocal(i64, LocalDebugInfo("$step", loop->Location, scope.Id.Id));
    auto nextLocal = Builder.AllocLocal(i64, LocalDebugInfo("$next", loop->Location, scope.Id.Id));

    auto storeLocal = [&](TLocal local, TOperand value) {
        emitter.Emit0("stre"_op, {TOperand{local}, value});
    };
    auto loadLocal = [&](TLocal local) {
        auto tmp = emitter.Emit1("load"_op, {TOperand{local}});
        Builder.SetType(tmp, i64);
        return tmp;
    };

    auto from = co_await Lower(loop->From, scope);
    if (!from.Value) co_return TError(loop->From->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
    emitter.Emit0("stre"_op, {varOperand, *from.Value});

    if (loop->Step) {
        auto step = co_await Lower(loop->Step, scope);
        if (!step.Value) co_return TError(loop->Step->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
        storeLocal(stepLocal, *step.Value);
    } else {
        storeLocal(stepLocal, TImm{1, i64});
    }

    auto initialNext = emitter.Emit1("load"_op, {varOperand});
    Builder.SetType(initialNext, i64);
    storeLocal(nextLocal, initialNext);

    auto to = co_await Lower(loop->To, scope);
    if (!to.Value) co_return TError(loop->To->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
    storeLocal(toLocal, *to.Value);

    auto entryId = Builder.CurrentBlockIdx();
    auto [condLabel, condId] = Builder.NewBlock();
    auto [bodyLabel, bodyId] = Builder.NewBlock();
    auto [postLabel, postId] = Builder.NewBlock();
    auto endLabel = Builder.NewLabel();

    Builder.SetCurrentBlock(entryId);
    emitter.Emit0("jmp"_op, {condLabel});

    Builder.SetCurrentBlock(condId);
    Statement(loop, scope.Id.Id);
    auto toValue = loadLocal(toLocal);
    auto nextValue = loadLocal(nextLocal);
    auto stepValue = loadLocal(stepLocal);
    auto diff = emitter.Emit1("-"_op, {toValue, nextValue});
    Builder.SetType(diff, i64);
    auto product = emitter.Emit1("*"_op, {diff, stepValue});
    Builder.SetType(product, i64);
    auto condValue = emitter.Emit1(">="_op, {product, TImm{0, i64}});
    Builder.SetType(condValue, Module.Types.I(EKind::I1));
    emitter.Emit0("cmp"_op, {condValue, bodyLabel, endLabel});

    Builder.SetCurrentBlock(bodyId);
    auto current = loadLocal(nextLocal);
    emitter.Emit0("stre"_op, {varOperand, current});
    co_await Lower(loop->Body, TBlockScope{
        .FuncIdx = scope.FuncIdx,
        .Id = scope.Id,
        .BreakLabel = endLabel,
        .ContinueLabel = postLabel,
        .ReturnLabel = scope.ReturnLabel,
        .RetLocal = scope.RetLocal,
        .RetTypeId = scope.RetTypeId,
        .LastAssert = scope.LastAssert
    });
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {postLabel});
    }

    Builder.SetCurrentBlock(postId);
    auto postNext = loadLocal(nextLocal);
    auto postStep = loadLocal(stepLocal);
    auto updated = emitter.Emit1("+"_op, {postNext, postStep});
    Builder.SetType(updated, i64);
    storeLocal(nextLocal, updated);
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {condLabel});
    }

    Builder.NewBlock(endLabel);
    co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerTimes(std::shared_ptr<NAst::TTimesStmtExpr> loop, TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loop->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    const auto i64 = Module.Types.I(EKind::I64);

    auto toLocal = Builder.AllocLocal(i64, LocalDebugInfo("$to", loop->Location, scope.Id.Id));
    auto nextLocal = Builder.AllocLocal(i64, LocalDebugInfo("$next", loop->Location, scope.Id.Id));

    auto count = co_await Lower(loop->Count, scope);
    if (!count.Value) co_return TError(loop->Count->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
    emitter.Emit0("stre"_op, {TOperand{nextLocal}, TImm{1, i64}});
    auto toValue = emitter.Emit1("+"_op, {*count.Value, TImm{1, i64}});
    Builder.SetType(toValue, i64);
    emitter.Emit0("stre"_op, {TOperand{toLocal}, toValue});

    auto loadLocal = [&](TLocal local) {
        auto tmp = emitter.Emit1("load"_op, {TOperand{local}});
        Builder.SetType(tmp, i64);
        return tmp;
    };

    auto entryId = Builder.CurrentBlockIdx();
    auto [condLabel, condId] = Builder.NewBlock();
    auto [bodyLabel, bodyId] = Builder.NewBlock();
    auto [postLabel, postId] = Builder.NewBlock();
    auto endLabel = Builder.NewLabel();

    Builder.SetCurrentBlock(entryId);
    emitter.Emit0("jmp"_op, {condLabel});

    Builder.SetCurrentBlock(condId);
    Statement(loop, scope.Id.Id);
    auto nextValue = loadLocal(nextLocal);
    auto limitValue = loadLocal(toLocal);
    auto condValue = emitter.Emit1("!="_op, {nextValue, limitValue});
    Builder.SetType(condValue, Module.Types.I(EKind::I1));
    emitter.Emit0("cmp"_op, {condValue, bodyLabel, endLabel});

    Builder.SetCurrentBlock(bodyId);
    co_await Lower(loop->Body, TBlockScope{
        .FuncIdx = scope.FuncIdx,
        .Id = scope.Id,
        .BreakLabel = endLabel,
        .ContinueLabel = postLabel,
        .ReturnLabel = scope.ReturnLabel,
        .RetLocal = scope.RetLocal,
        .RetTypeId = scope.RetTypeId,
        .LastAssert = scope.LastAssert
    });
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {postLabel});
    }

    Builder.SetCurrentBlock(postId);
    auto postNext = loadLocal(nextLocal);
    auto updated = emitter.Emit1("+"_op, {postNext, TImm{1, i64}});
    Builder.SetType(updated, i64);
    emitter.Emit0("stre"_op, {TOperand{nextLocal}, updated});
    if (!Builder.IsCurrentBlockTerminated()) {
        emitter.Emit0("jmp"_op, {condLabel});
    }

    Builder.NewBlock(endLabel);
    co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
}

TExpectedTask<TTmp, TError, TLocation> TAstLowerer::LoadVar(const std::string& name, TBlockScope scope, const TLocation& loc, bool takeRefOfNotRef)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto var = Context.Lookup(name, scope.Id);
    if (!var) {
        co_return TError(loc, TErrorString::Get<EErrorId::UNDEFINED_VARIABLE>(name));
    }

    auto node = Context.GetSymbolNode(NSemantics::TSymbolId{var->Id});
    if (!node) {
        co_return TError(loc, TErrorString::Get<EErrorId::UNDEFINED_VARIABLE>(name));
    }
    if (NAst::TMaybeType<NAst::TReferenceType>(node->Type) && takeRefOfNotRef) {
        takeRefOfNotRef = false;
    }

    TOperand op = (var->FunctionLevelIdx >= 0)
        ? TOperand{ TLocal{ var->FunctionLevelIdx } }
        : TOperand{ TSlot{ var->Id } };

    DebugPoints_.Bind(loc, var->FunctionLevelIdx, name);
    int valueTypeId = FromAstType(node->Type, Module.Types);
    bool loadStructByAddress =
        !takeRefOfNotRef
        && NAst::TMaybeType<NAst::TStructType>(NAst::UnwrapNamedType(node->Type));

    // Struct rvalues are address-backed in IR; scalar values are loaded.
    TOp opcode = (takeRefOfNotRef || loadStructByAddress) ? "lea"_op : "load"_op;
    auto tmp = emitter.Emit1(opcode, { op });
    Builder.SetType(tmp,
        (takeRefOfNotRef || loadStructByAddress) ? Module.Types.Ptr(valueTypeId) : valueTypeId);
    co_return tmp;
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerIndices(NSemantics::TSymbolInfo symbol, const std::vector<NAst::TExprPtr>& indices, TBlockScope scope, int elemSize)
{
    TInstrEmitter emitter{
        Builder,
        InstDebugInfo(indices.empty() ? TLocation{} : indices.front()->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    int n = indices.size() - 1;
    int i = n;
    auto i64 = Module.Types.I(EKind::I64);

    auto layoutIt = ArrayLayouts.find(symbol.Id);
    if (layoutIt == ArrayLayouts.end()) {
        co_return TError(indices.empty() ? TLocation{} : indices.front()->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
    }
    const auto& layout = layoutIt->second;

    std::optional<TTmp> prev;

    for (; i >= 0; --i) {
        auto indexRes = co_await Lower(indices[i], scope);
        if (!indexRes.Value) {
            co_return TError(indices[i]->Location, TErrorString::Get<EErrorId::ARRAY_INDEX_NOT_NUMBER>());
        }
        // totalIndex = sum (index - lowerBound) * stride_i
        // stride_n = 1
        // stride_{n-1} = mulAccName_{n}

        emitter.DebugInfo = InstDebugInfo(indices[i]->Location, scope.Id.Id);
        auto tmp = LoadLayoutOperand(layout.LBounds[i], indices[i]->Location, scope.Id.Id);
        tmp = emitter.Emit1("-"_op, {*indexRes.Value, tmp});
        Builder.SetType(tmp, i64);
        if (i != n) {
            auto stride = LoadLayoutOperand(layout.Strides[i + 1], indices[i]->Location, scope.Id.Id);
            tmp = emitter.Emit1("*"_op, {tmp, stride});
            Builder.SetType(tmp, i64);
        }

        if (prev) {
            tmp = emitter.Emit1("+"_op, {tmp, *prev});
            Builder.SetType(tmp, i64);
        }

        prev = tmp;
    }
    *prev = emitter.Emit1("*"_op, {*prev, TImm{elemSize, i64}}); // byte offset
    Builder.SetType(*prev, i64);
    co_return TValueWithBlock{ *prev, Builder.CurrentBlockLabel() };
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerLValueAddress(const NAst::TExprPtr& expr, TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(expr->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        auto addr = co_await LoadVar(maybeIdent.Cast()->Name, scope, expr->Location, true /*address*/);
        co_return TValueWithBlock{addr, Builder.CurrentBlockLabel()};
    }

    if (auto maybeIndex = NAst::TMaybeNode<NAst::TIndexExpr>(expr)) {
        auto index = maybeIndex.Cast();
        auto indexValue = co_await Lower(index->Index, scope);
        if (!indexValue.Value) {
            co_return TError(index->Index->Location, TErrorString::Get<EErrorId::ARRAY_INDEX_NOT_NUMBER>());
        }

        auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(index->Collection);
        if (!maybeIdent) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }
        auto arraySymbol = Context.Lookup(maybeIdent.Cast()->Name, scope.Id);
        if (!arraySymbol) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }
        auto symbolNode = Context.GetSymbolNode(NSemantics::TSymbolId{arraySymbol->Id});

        auto value = co_await Lower(index->Collection, scope);
        if (!value.Value) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::FAILED_LOWER_COLLECTION>());
        }
        auto arrayPtr = *value.Value;
        if (arrayPtr.Type != TOperand::EType::Tmp) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }

        auto arrayType = Builder.GetType(arrayPtr.Tmp);
        int elemTypeId = FromAstType(expr->Type, Module.Types);
        int elemByteSize = Module.Types.SizeInBytes(elemTypeId);
        auto i64 = Module.Types.I(EKind::I64);

        TOperand byteOffset;
        auto collectionType = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(index->Collection->Type));
        if (NAst::TMaybeType<NAst::TPointerType>(collectionType) || IsRawArraySymbol(symbolNode)) {
            auto offset = emitter.Emit1("*"_op, {*indexValue.Value, TImm{elemByteSize, i64}});
            Builder.SetType(offset, i64);
            byteOffset = offset;
        } else {
            auto layoutIt = ArrayLayouts.find(arraySymbol->Id);
            if (layoutIt == ArrayLayouts.end()) {
                co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
            }
            auto lbound0 = LoadLayoutOperand(layoutIt->second.LBounds[0], expr->Location, scope.Id.Id);
            auto zeroBasedIndex = emitter.Emit1("-"_op, {*indexValue.Value, lbound0});
            Builder.SetType(zeroBasedIndex, i64);
            auto offset = emitter.Emit1("*"_op, {zeroBasedIndex, TImm{elemByteSize, i64}});
            Builder.SetType(offset, i64);
            byteOffset = offset;
        }

        auto destPtr = emitter.Emit1("+"_op, {arrayPtr, byteOffset});
        Builder.SetType(destPtr, arrayType);
        co_return TValueWithBlock{destPtr, Builder.CurrentBlockLabel()};
    }

    if (auto maybeMultiIndex = NAst::TMaybeNode<NAst::TMultiIndexExpr>(expr)) {
        auto multiIndex = maybeMultiIndex.Cast();
        auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(multiIndex->Collection);
        if (!maybeIdent) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::MULTI_INDEX_COLLECTION_MUST_BE_IDENTIFIER>());
        }
        auto arraySymbol = Context.Lookup(maybeIdent.Cast()->Name, scope.Id);
        if (!arraySymbol) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }
        auto value = co_await Lower(multiIndex->Collection, scope);
        if (!value.Value) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::FAILED_LOWER_COLLECTION>());
        }
        auto arrayPtr = *value.Value;
        if (arrayPtr.Type != TOperand::EType::Tmp) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }
        auto arrayType = Builder.GetType(arrayPtr.Tmp);
        int elemTypeId = FromAstType(expr->Type, Module.Types);
        auto indices = co_await LowerIndices(*arraySymbol, multiIndex->Indices, scope, Module.Types.SizeInBytes(elemTypeId));
        if (!indices.Value) {
            co_return TError(multiIndex->Location, TErrorString::Get<EErrorId::FAILED_LOWER_ARRAY_INDICES>());
        }
        auto destPtr = emitter.Emit1("+"_op, {arrayPtr, *indices.Value});
        Builder.SetType(destPtr, arrayType);
        co_return TValueWithBlock{destPtr, Builder.CurrentBlockLabel()};
    }

    if (auto maybeField = NAst::TMaybeNode<NAst::TFieldAccessExpr>(expr)) {
        auto field = maybeField.Cast();
        auto object = field->Object;
        TOperand objAddr;
        if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(object)) {
            objAddr = co_await LoadVar(maybeIdent.Cast()->Name, scope, object->Location, true /*address*/);
        } else {
            auto res = co_await LowerLValueAddress(object, scope);
            if (!res.Value) {
                co_return TError(object->Location, "Не удалось вычислить адрес объекта при обращении к полю '" + field->FieldName + "'.");
            }
            objAddr = *res.Value;
        }

        auto objAstType = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(object->Type));
        int structTypeId = FromAstType(objAstType, Module.Types);
        int64_t fieldByteOffset = Module.Types.FieldOffset(structTypeId, field->FieldIndex);

        auto i64 = Module.Types.I(EKind::I64);
        int fieldTypeId = FromAstType(field->Type, Module.Types);
        int ptrTypeId = Module.Types.Ptr(fieldTypeId);
        auto fieldPtr = emitter.Emit1("+"_op, {objAddr, TImm{fieldByteOffset, i64}});
        Builder.SetType(fieldPtr, ptrTypeId);
        co_return TValueWithBlock{fieldPtr, Builder.CurrentBlockLabel()};
    }

    co_return TError(expr->Location, TErrorString::Get<EErrorId::ARG_REF_MUST_BE_IDENTIFIER>());
}

static bool IsAddressableFieldBase(const NAst::TExprPtr& expr) {
    if (!expr) {
        return false;
    }
    if (NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        return true;
    }
    if (NAst::TMaybeNode<NAst::TIndexExpr>(expr) || NAst::TMaybeNode<NAst::TMultiIndexExpr>(expr)) {
        return true;
    }
    if (NAst::TMaybeNode<NAst::TFieldAccessExpr>(expr)) {
        return true;
    }
    if (auto maybeCast = NAst::TMaybeNode<NAst::TCastExpr>(expr)) {
        return IsAddressableFieldBase(maybeCast.Cast()->Operand);
    }
    return false;
}

TExpectedTask<std::monostate, TError, TLocation> TAstLowerer::EmitLifetimeDestroy(
    TOperand value,
    const NAst::TTypePtr& inputType,
    std::optional<TOperand> aux,
    const TLocation& loc,
    int32_t scopeId)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(loc, scopeId), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    auto type = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(inputType));
    std::string destructorName;
    bool usesAux = false;
    if (NAst::TMaybeType<NAst::TStringType>(type)) {
        destructorName = "str_release";
    } else if (auto array = NAst::TMaybeType<NAst::TArrayType>(type)) {
        auto elementType = NAst::UnwrapNamedType(array.Cast()->ElementType);
        if (NAst::TMaybeType<NAst::TStringType>(elementType)) {
            if (!aux) {
                co_return TError(loc, "array<string> destroy requires an allocation-size operand");
            }
            destructorName = "array_str_destroy";
            usesAux = true;
        } else {
            destructorName = "array_destroy";
        }
    } else {
        co_return TError(loc, "lifetime destroy does not support type '"
            + NAst::TypeDiagnosticName(type) + "'");
    }

    auto destructorId = co_await GlobalSymbolId(destructorName);
    emitter.Emit0("arg"_op, {value});
    if (usesAux) {
        emitter.Emit0("arg"_op, {*aux});
    }
    emitter.Emit0("call"_op, {TImm{destructorId}});
    co_return std::monostate{};
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerVector(
    const NAst::TVectorExpr& vector,
    TBlockScope scope)
{
    TInstrEmitter emitter{Builder, InstDebugInfo(vector.Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    const int vectorType = FromAstType(vector.Type, Module.Types);
    const int elementType = Module.Types.UnderlyingType(vectorType);
    const int elementSize = Module.Types.SizeInBytes(elementType);
    const int pointerType = Module.Types.Ptr(elementType);
    const int offsetType = Module.Types.I(EKind::I64);
    const auto storage = Builder.AllocLocal(vectorType, LocalDebugInfo("$vector", vector.Location, scope.Id.Id));
    const auto base = emitter.Emit1("lea"_op, {storage});
    Builder.SetType(base, pointerType);
    for (size_t i = 0; i < vector.Elements.size(); ++i) {
        auto element = co_await Lower(vector.Elements[i], scope);
        if (!element.Value) {
            co_return TError(vector.Elements[i]->Location, "vector element must produce a value");
        }
        auto address = base;
        if (i != 0) {
            address = emitter.Emit1("+"_op, {base, TImm{static_cast<int64_t>(i * elementSize), offsetType}});
            Builder.SetType(address, pointerType);
        }
        emitter.Emit0("ste"_op, {address, *element.Value});
    }
    const auto result = emitter.Emit1("load"_op, {storage});
    Builder.SetType(result, vectorType);
    co_return TValueWithBlock{result, Builder.CurrentBlockLabel()};
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::LowerVectorIntrinsic(
    const NAst::TCallExpr& call, std::string_view op, TBlockScope scope)
{
    std::vector<int> mask;
    if (op == "shuffle") {
        if (auto literal = NAst::TMaybeNode<NAst::TVectorExpr>(call.Args[1]).Cast()) {
            for (const auto& element : literal->Elements) {
                auto number = NAst::TMaybeNode<NAst::TNumberExpr>(element).Cast();
                if (!number) {
                    mask.clear();
                    break;
                }
                mask.push_back(static_cast<int>(number->IntValue));
            }
        }
    }
    std::vector<TOperand> args;
    for (size_t i = 0; i < call.Args.size(); ++i) {
        if (i == 1 && !mask.empty()) {
            const auto id = static_cast<int64_t>(Module.VectorShuffleMasks.size());
            Module.VectorShuffleMasks.push_back(std::move(mask));
            args.push_back(TImm{id, Module.Types.I(EKind::I64)});
        } else {
            auto value = co_await Lower(call.Args[i], scope);
            if (!value.Value) {
                co_return TError(call.Location, "Vector intrinsic argument must produce a value");
            }
            args.push_back(*value.Value);
        }
    }
    TInstrEmitter emitter{Builder, InstDebugInfo(call.Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    const TOp opcode{std::string(op).c_str()};
    if (op == "prefetch") {
        emitter.Emit0(opcode, {args[0]});
        co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
    }
    const auto tmp = emitter.Emit1(opcode, args);
    Builder.SetType(tmp, FromAstType(call.Type, Module.Types));
    co_return TValueWithBlock{tmp, Builder.CurrentBlockLabel()};
}

TExpectedTask<TAstLowerer::TValueWithBlock, TError, TLocation> TAstLowerer::Lower(const NAst::TExprPtr& inputExpr, TBlockScope scope) {
    NAst::TExprPtr expr = inputExpr;
    TInstrEmitter emitter{Builder, InstDebugInfo(expr->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    if (Module.DebugOptions.CollectMetadata()) {
        auto scopePtr = Context.GetScope(scope.Id);
        auto parentScopeId = scopePtr && scopePtr->Parent ? scopePtr->Parent->Id.Id : -1;
        Builder.SetScopeDebugInfo(scope.Id.Id, parentScopeId);
    }

    if (auto vector = NAst::TMaybeNode<NAst::TVectorExpr>(expr)) {
        co_return co_await LowerVector(*vector.Cast(), scope);
    } else if (auto maybeRetain = NAst::TMaybeNode<NAst::TRetainExpr>(expr)) {
        auto retain = maybeRetain.Cast();
        auto value = co_await Lower(retain->Value, scope);
        if (!value.Value) {
            co_return TError(retain->Location, "retain operand must produce a value");
        }
        auto retainId = co_await GlobalSymbolId("str_retain");
        emitter.Emit0("arg"_op, {*value.Value});
        emitter.Emit0("call"_op, {TImm{retainId}});
        co_return value;
    } else if (auto maybeLiteral = NAst::TMaybeNode<NAst::TOwnLiteralExpr>(expr)) {
        auto literal = maybeLiteral.Cast();
        auto value = co_await Lower(literal->Value, scope);
        if (!value.Value) {
            co_return TError(literal->Location, "own-literal operand must produce a value");
        }
        auto constructorId = co_await GlobalSymbolId("str_from_lit");
        emitter.Emit0("arg"_op, {*value.Value});
        auto owned = emitter.Emit1("call"_op, {TImm{constructorId}});
        Builder.SetType(owned, FromAstType(literal->Type, Module.Types));
        co_return TValueWithBlock{owned, Builder.CurrentBlockLabel()};
    } else if (auto maybeMove = NAst::TMaybeNode<NAst::TMoveExpr>(expr)) {
        auto value = co_await Lower(maybeMove.Cast()->Value, scope);
        co_return value;
    } else if (auto maybeBorrow = NAst::TMaybeNode<NAst::TBorrowExpr>(expr)) {
        auto value = co_await Lower(maybeBorrow.Cast()->Value, scope);
        co_return value;
    } else if (auto maybeDestroy = NAst::TMaybeNode<NAst::TDestroyExpr>(expr)) {
        auto destroy = maybeDestroy.Cast();
        auto value = co_await Lower(destroy->Value, scope);
        if (!value.Value) {
            co_return TError(destroy->Location, "destroy operand must produce a value");
        }
        std::optional<TOperand> aux;
        if (destroy->Aux) {
            auto loweredAux = co_await Lower(destroy->Aux, scope);
            if (!loweredAux.Value) {
                co_return TError(destroy->Aux->Location,
                    "destroy auxiliary operand must produce a value");
            }
            aux = *loweredAux.Value;
        }
        co_await EmitLifetimeDestroy(
            *value.Value,
            destroy->Value->Type,
            aux,
            destroy->Location,
            scope.Id.Id);
        co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
    } else if (auto maybeReplace = NAst::TMaybeNode<NAst::TReplaceExpr>(expr)) {
        auto replace = maybeReplace.Cast();
        auto value = co_await Lower(replace->Value, scope);
        if (!value.Value) {
            co_return TError(replace->Value->Location,
                "replace value must produce a value");
        }
        auto address = co_await LowerLValueAddress(replace->Target, scope);
        if (!address.Value) {
            co_return TError(replace->Target->Location,
                "replace target must be addressable");
        }
        auto targetType = NAst::UnwrapReferenceType(
            NAst::UnwrapNamedType(replace->Target->Type));
        auto targetTypeId = FromAstType(targetType, Module.Types);
        auto oldValue = emitter.Emit1("lde"_op, {*address.Value});
        Builder.SetType(oldValue, targetTypeId);
        co_await EmitLifetimeDestroy(
            oldValue,
            targetType,
            std::nullopt,
            replace->Location,
            scope.Id.Id);
        emitter.Emit0("ste"_op, {*address.Value, *value.Value});
        co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
    } else if (auto maybeExit = NAst::TMaybeNode<NAst::TCleanupExitExpr>(expr)) {
        auto exit = maybeExit.Cast();
        if (exit->Kind == NAst::ECleanupExitKind::Return) {
            if (!scope.ReturnLabel) {
                co_return TError(expr->Location, TErrorString::Get<EErrorId::RETURN_OUTSIDE_FUNCTION>());
            }
            if (exit->Value) {
                auto value = co_await Lower(exit->Value, scope);
                if (!value.Value || !scope.RetLocal) {
                    co_return TError(expr->Location, "cleanup return value cannot be stored");
                }
                auto returnValue = *value.Value;
                if (scope.RetTypeId && Module.Types.GetKind(*scope.RetTypeId) == EKind::Struct) {
                    returnValue = co_await EnsureStructAddress(
                        returnValue,
                        *scope.RetTypeId,
                        "$return_value",
                        exit->Value->Location,
                        scope.Id.Id);
                }
                emitter.Emit0("stre"_op, {TOperand{*scope.RetLocal}, returnValue});
            }
            if (scope.LastAssert) {
                co_await Lower(scope.LastAssert, scope);
            }
            for (const auto& cleanup : exit->Cleanups) {
                co_await Lower(cleanup, scope);
            }
            emitter.Emit0("jmp"_op, {*scope.ReturnLabel});
            co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
        }

        const auto target = exit->Kind == NAst::ECleanupExitKind::Break
            ? scope.BreakLabel
            : scope.ContinueLabel;
        if (!target) {
            co_return TError(expr->Location,
                exit->Kind == NAst::ECleanupExitKind::Break
                    ? TErrorString::Get<EErrorId::BREAK_NOT_IN_LOOP>()
                    : TErrorString::Get<EErrorId::CONTINUE_NOT_IN_LOOP>());
        }
        for (const auto& cleanup : exit->Cleanups) {
            co_await Lower(cleanup, scope);
        }
        emitter.Emit0("jmp"_op, {*target});
        co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
    } else if (auto maybeGlobal = NAst::TMaybeNode<NAst::TGlobalCleanupExpr>(expr)) {
        for (const auto& cleanup : maybeGlobal.Cast()->Cleanups) {
            co_await Lower(cleanup, scope);
        }
        co_return TValueWithBlock{std::nullopt, Builder.CurrentBlockLabel()};
    } else if (auto maybeBitcast = NAst::TMaybeNode<NAst::TBitcastExpr>(expr)) {
        auto bitcast = maybeBitcast.Cast();
        auto operand = co_await Lower(bitcast->Operand, scope);
        if (!operand.Value) {
            co_return TError(bitcast->Operand->Location, "bitcast operand must be a value");
        }
        const auto& types = Module.Types;
        auto sourceType = NAst::UnwrapNamedType(NAst::UnwrapReferenceType(bitcast->Operand->Type));
        auto targetType = NAst::UnwrapNamedType(bitcast->Type);
        int sourceTypeId = FromAstType(sourceType, Module.Types);
        int targetTypeId = FromAstType(targetType, Module.Types);
        // An address travels as a 64-bit integer whatever the pointer width, and
        // codegen converts it with inttoptr/ptrtoint (see "bitcast"_op in
        // llvm_codegen.cpp), so such a pair is exempt from the same-size rule.
        auto isAddress = [](const NAst::TTypePtr& type) {
            auto integer = NAst::TMaybeType<NAst::TIntegerType>(type);
            return NAst::TMaybeType<NAst::TPointerType>(type) || NAst::TMaybeType<NAst::TStringType>(type)
                || (integer && integer.Cast()->BitWidth() == 64);
        };
        // Plain data is an integer or a float, alone or as vector lanes. A boolean
        // must stay 0 or 1: its lane, a byte, can be read, but nothing becomes one.
        auto isPlain = [&](int typeId, bool readOnly) {
            int lane = types.IsVector(typeId) ? types.UnderlyingType(typeId) : typeId;
            if (!types.IsInteger(lane)) {
                return types.IsFloat(lane);
            }
            return types.GetKind(lane) != EKind::I1 || (readOnly && types.IsVector(typeId));
        };
        bool plain = isPlain(sourceTypeId, true) && isPlain(targetTypeId, false);
        if (!isAddress(sourceType) || !isAddress(targetType)) {
            if (!plain) {
                co_return TError(bitcast->Location, TErrorString::Get<EErrorId::UNSUPPORTED_CAST_TYPES>(
                    NAst::TypeDiagnosticName(bitcast->Operand->Type),
                    NAst::TypeDiagnosticName(bitcast->Type)));
            }
            if (types.SizeInBytes(sourceTypeId) != types.SizeInBytes(targetTypeId)) {
                co_return TError(bitcast->Location, "bitcast types must have the same size");
            }
        }
        if (plain && operand.Value->Type == TOperand::EType::Imm && !types.IsVector(targetTypeId)) {
            // An immediate already holds raw bits, so a scalar one only changes its type.
            co_return TValueWithBlock{
                TImm{.Value = operand.Value->Imm.Value, .TypeId = targetTypeId},
                Builder.CurrentBlockLabel()};
        }
        auto tmp = emitter.Emit1("bitcast"_op, {*operand.Value});
        Builder.SetType(tmp, targetTypeId);
        co_return TValueWithBlock{tmp, Builder.CurrentBlockLabel()};
    } else if (auto maybeCast = NAst::TMaybeNode<NAst::TCastExpr>(expr)) {
        auto cast = maybeCast.Cast();
        auto operand = co_await Lower(cast->Operand, scope);
        if (!operand.Value) co_return TError(cast->Operand->Location, TErrorString::Get<EErrorId::OPERAND_OF_CAST_NOT_VALUE>());
        auto sourceVector = NAst::TMaybeType<NAst::TVectorType>(
            NAst::UnwrapNamedType(NAst::UnwrapReferenceType(cast->Operand->Type))).Cast();
        auto targetVector = NAst::TMaybeType<NAst::TVectorType>(NAst::UnwrapNamedType(expr->Type)).Cast();
        if (sourceVector && targetVector) {
            const int sourceType = FromAstType(sourceVector->ElementType, Module.Types);
            const int targetType = FromAstType(targetVector->ElementType, Module.Types);
            TOp op = "mov"_op;
            if (Module.Types.GetKind(targetType) == EKind::I1) {
                op = Module.Types.IsFloat(sourceType) ? "f2b"_op : "i2b"_op;
            } else if (Module.Types.IsFloat(sourceType) != Module.Types.IsFloat(targetType)) {
                op = Module.Types.IsFloat(sourceType) ? "f2i"_op : "i2f"_op;
            }
            auto tmp = emitter.Emit1(op, {*operand.Value});
            Builder.SetType(tmp, FromAstType(expr->Type, Module.Types));
            co_return TValueWithBlock{tmp, Builder.CurrentBlockLabel()};
        }
        std::optional<TOperand> tmp;
        auto sourceArrayForPtrCast = NAst::TMaybeType<NAst::TArrayType>(
            NAst::UnwrapNamedType(cast->Operand->Type));
        auto targetPtrForArrayCast = NAst::TMaybeType<NAst::TPointerType>(
            NAst::UnwrapNamedType(expr->Type));
        bool arrayToPtrCast = sourceArrayForPtrCast && targetPtrForArrayCast
            && FromAstType(NAst::UnwrapNamedType(sourceArrayForPtrCast.Cast()->ElementType), Module.Types)
                == FromAstType(NAst::UnwrapNamedType(targetPtrForArrayCast.Cast()->PointeeType), Module.Types);
        if (NAst::TMaybeType<NAst::TIntegerType>(expr->Type) && NAst::TMaybeType<NAst::TBoolType>(cast->Operand->Type)) {
            // bool to int cast
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TFloatType>(expr->Type) && NAst::TMaybeType<NAst::TBoolType>(cast->Operand->Type)) {
            // bool to float cast
            tmp = emitter.Emit1("i2f"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TIntegerType>(expr->Type) && NAst::TMaybeType<NAst::TFloatType>(cast->Operand->Type)) {
            // float to int cast
            tmp = emitter.Emit1("f2i"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TFloatType>(expr->Type) && NAst::TMaybeType<NAst::TIntegerType>(cast->Operand->Type)) {
            // int to float cast
            tmp = emitter.Emit1("i2f"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TBoolType>(expr->Type) && NAst::TMaybeType<NAst::TIntegerType>(cast->Operand->Type)) {
            tmp = emitter.Emit1("i2b"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TBoolType>(expr->Type) && NAst::TMaybeType<NAst::TFloatType>(cast->Operand->Type)) {
            tmp = emitter.Emit1("f2b"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TSymbolType>(expr->Type) && NAst::TMaybeType<NAst::TIntegerType>(cast->Operand->Type)) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TIntegerType>(expr->Type) && NAst::TMaybeType<NAst::TSymbolType>(cast->Operand->Type)) {
            // oposite of above: int to symbol
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TIntegerType>(NAst::UnwrapNamedType(expr->Type))
            && NAst::TMaybeType<NAst::TIntegerType>(NAst::UnwrapNamedType(cast->Operand->Type))) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (arrayToPtrCast) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (FromAstType(NAst::UnwrapNamedType(expr->Type), Module.Types)
            == FromAstType(NAst::UnwrapNamedType(cast->Operand->Type), Module.Types)) {
            if (NAst::TMaybeType<NAst::TStructType>(NAst::UnwrapNamedType(expr->Type))) {
                co_return TValueWithBlock{operand.Value, Builder.CurrentBlockLabel()};
            }
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TPointerType>(NAst::UnwrapNamedType(expr->Type))
            && NAst::TMaybeType<NAst::TPointerType>(NAst::UnwrapNamedType(cast->Operand->Type))) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TIntegerType>(NAst::UnwrapNamedType(expr->Type))
            && NAst::TMaybeType<NAst::TPointerType>(NAst::UnwrapNamedType(cast->Operand->Type))) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else if (NAst::TMaybeType<NAst::TPointerType>(NAst::UnwrapNamedType(expr->Type))
            && NAst::TMaybeType<NAst::TIntegerType>(NAst::UnwrapNamedType(cast->Operand->Type))) {
            tmp = emitter.Emit1("mov"_op, {*operand.Value});
        } else {
            co_return TError(cast->Location, TErrorString::Get<EErrorId::UNSUPPORTED_CAST_TYPES>(
                NAst::TypeDiagnosticName(cast->Operand->Type),
                NAst::TypeDiagnosticName(expr->Type)));
        }
        Builder.SetType(tmp->Tmp, FromAstType(expr->Type, Module.Types));
        co_return TValueWithBlock{ tmp, Builder.CurrentBlockLabel() };
    } else if (auto maybeNum = NAst::TMaybeNode<NAst::TNumberExpr>(expr)) {
        auto num = maybeNum.Cast();
        if (num->IsFloat()) {
            co_return TValueWithBlock{ TImm{.Value = std::bit_cast<int64_t>(num->FloatValue), .TypeId = Module.Types.I(EKind::F64)}, Builder.CurrentBlockLabel() };
        } else {
            int typeId = num->Type && (
                NAst::TMaybeType<NAst::TIntegerType>(num->Type)
                || NAst::TMaybeType<NAst::TBoolType>(num->Type)
                || NAst::TMaybeType<NAst::TStringType>(num->Type))
                ? FromAstType(num->Type, Module.Types)
                : Module.Types.I(EKind::I64);
            co_return TValueWithBlock{ TImm{.Value = num->IntValue, .TypeId = typeId}, Builder.CurrentBlockLabel() };
        }
    } else if (auto maybeStringLiteral = NAst::TMaybeNode<NAst::TStringLiteralExpr>(expr)) {
        auto str = maybeStringLiteral.Cast();
        auto id = Builder.StringLiteral(str->Value);
        // TODO: type is 'pointer to char'
        co_return TValueWithBlock{ TImm{.Value = id, .TypeId = Module.Types.Ptr(Module.Types.I(EKind::I8))}, Builder.CurrentBlockLabel() };
    } else if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(expr)) {
        // Evaluate a block: value is the value of the last statement (or void if none)
        std::optional<TOperand> last;
        auto block = maybeBlock.Cast();
        auto newScope = scope;
        newScope.Id = NSemantics::TScopeId{block->Scope};

        // Frontend I/O expansion groups several runtime calls into one source statement.
        const bool grouped = !block->Stmts.empty() && std::all_of(
            block->Stmts.begin(), block->Stmts.end(), [&](const auto& stmt) {
                return (stmt->Location <=> block->Location) == 0;
            });
        if (grouped) {
            DebugPoints_.Point("statement", InstDebugInfo(block->Location, newScope.Id.Id));
        }
        for (size_t stmtIdx = 0; stmtIdx < block->Stmts.size(); ++stmtIdx) {
            auto& s = block->Stmts[stmtIdx];

            if (!grouped && !NAst::TMaybeNode<NAst::TWhileStmtExpr>(s) && !NAst::TMaybeNode<NAst::TForStmtExpr>(s)
                && !NAst::TMaybeNode<NAst::TTimesStmtExpr>(s))
            {
                Statement(s, newScope.Id.Id);
            }
            auto r = co_await Lower(s, newScope);
            last = r.Value;

            // Stop lowering subsequent statements if current block has a terminating instruction (e.g. break -> jmp)
            if (Builder.IsCurrentBlockTerminated()) {
                break;
            }
        }
        // TODO: return only if function block and last is 'return'
        co_return TValueWithBlock{ last, Builder.CurrentBlockLabel() };
    } else if (auto maybeUnary = NAst::TMaybeNode<NAst::TUnaryExpr>(expr)) {
        auto unary = maybeUnary.Cast();
        auto operand = co_await Lower(unary->Operand, scope);
        if (!operand.Value) {
            co_return TError(unary->Operand->Location, TErrorString::Get<EErrorId::OPERAND_OF_UNARY_NOT_NUMBER>());
        }
        if (unary->Operator == '-' || unary->Operator == '!'_op || unary->Operator == '~'_op) {
            auto op = unary->Operator == '-'
                ? "neg"_op
                : TOp(unary->Operator.Value);
            auto tmp = emitter.Emit1(op, {*operand.Value});
            Builder.SetType(tmp, FromAstType(expr->Type, Module.Types));
            co_return TValueWithBlock{ tmp, Builder.CurrentBlockLabel() };
        }
        co_return TValueWithBlock{ operand.Value, operand.ProducingLabel };
    } else if (auto maybeBinary = NAst::TMaybeNode<NAst::TBinaryExpr>(expr)) {
        auto binary = maybeBinary.Cast();
        const bool isVector = NAst::TMaybeType<NAst::TVectorType>(NAst::UnwrapNamedType(expr->Type));
        const bool logical = binary->Operator == "&&"_op || binary->Operator == "||"_op;
        const bool isLazy = !isVector && logical;
        auto leftRes = co_await Lower(binary->Left, scope);
        if (!leftRes.Value) {
            co_return TError(binary->Left->Location, TErrorString::Get<EErrorId::BINARY_OPERANDS_NOT_NUMBERS>());
        }

        if (!isLazy) {
            auto rightRes = co_await Lower(binary->Right, scope);
            if (!rightRes.Value) {
                co_return TError(binary->Right->Location, TErrorString::Get<EErrorId::BINARY_OPERANDS_NOT_NUMBERS>());
            }
            // IR '/' is type-directed, so integer '//' uses the same opcode.
            auto irOp = binary->Operator == "//"_op
                ? '/'_op
                : TOp(binary->Operator.Value);
            auto tmp = emitter.Emit1(irOp, {*leftRes.Value, *rightRes.Value});
            Builder.SetType(tmp, FromAstType(expr->Type, Module.Types));
            co_return TValueWithBlock{tmp, Builder.CurrentBlockLabel()};
        }

        const bool isAnd = binary->Operator == "&&"_op;
        auto [rhsLabel, rhsId] = Builder.NewBlock();
        auto endLabel = Builder.NewLabel();
        Builder.SetCurrentBlock(leftRes.ProducingLabel);
        emitter.Emit0("cmp"_op, {*leftRes.Value, isAnd ? rhsLabel : endLabel, isAnd ? endLabel : rhsLabel});
        auto leftEdgeLabel = Builder.CurrentBlockLabel();

        Builder.SetCurrentBlock(rhsId);
        auto rightRes = co_await Lower(binary->Right, scope);
        if (!rightRes.Value) {
            co_return TError(binary->Right->Location, TErrorString::Get<EErrorId::BINARY_OPERANDS_NOT_NUMBERS>());
        }
        emitter.Emit0("jmp"_op, {endLabel});
        auto rightEdgeLabel = Builder.CurrentBlockLabel();

        // Nested expressions can change the RHS block; use the actual merge predecessors.
        Builder.NewBlock(endLabel);
        auto result = emitter.Emit1("phi"_op, {*leftRes.Value, leftEdgeLabel, *rightRes.Value, rightEdgeLabel});
        Builder.SetType(result, FromAstType(expr->Type, Module.Types));
        co_return TValueWithBlock{result, Builder.CurrentBlockLabel()};
    } else if (auto maybeIfExpr = NAst::TMaybeNode<NAst::TIfExpr>(expr)) {
        auto ife = maybeIfExpr.Cast();
        auto cond = co_await Lower(ife->Cond, scope);
        if (!cond.Value) co_return TError(ife->Cond->Location, TErrorString::Get<EErrorId::IF_CONDITION_NOT_NUMBER>());

        bool isVoid = NAst::TMaybeType<NAst::TVoidType>(ife->Type);

        auto entryId = Builder.CurrentBlockIdx();
        auto [thenLabel, thenId] = Builder.NewBlock();
        auto [elseLabel, elseId] = Builder.NewBlock();

        Builder.SetCurrentBlock(entryId);
        emitter.Emit0("cmp"_op, {*cond.Value, thenLabel, elseLabel});

        Builder.SetCurrentBlock(thenId);
        auto thenRes = co_await Lower(ife->Then, scope);
        Builder.SetCurrentBlock(thenRes.ProducingLabel);
        bool thenTerminated = Builder.IsCurrentBlockTerminated();
        auto thenEdgeLabel = thenRes.ProducingLabel;

        Builder.SetCurrentBlock(elseId);
        if (ife->Else) {
            auto elseRes = co_await Lower(ife->Else, scope);
            Builder.SetCurrentBlock(elseRes.ProducingLabel);
            bool elseTerminated = Builder.IsCurrentBlockTerminated();

            if (thenTerminated && elseTerminated) {
                co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
            }

            auto endLabel = Builder.NewLabel();
            if (isVoid && !thenTerminated) {
                Builder.SetCurrentBlock(thenEdgeLabel);
                emitter.Emit0("jmp"_op, {endLabel});
            }

            if (!isVoid) {
                // Expression if: emit phi to merge values
                if (!thenRes.Value) {
                    co_return TError(ife->Then->Location, "Ветвь `то' в `если'-выражении не возвращает значение");
                }
                if (!elseRes.Value) {
                    co_return TError(ife->Else->Location, "Ветвь `иначе' в `если'-выражении не возвращает значение");
                }
                int resultTypeId = FromAstType(expr->Type, Module.Types);
                bool isStructResult = NAst::TMaybeType<NAst::TStructType>(NAst::UnwrapNamedType(expr->Type));
                if (isStructResult) {
                    if (thenTerminated || elseTerminated) {
                        co_return TError(ife->Location, "Внутренняя ошибка: структурное if-выражение с завершающейся ветвью не поддержано.");
                    }
                    Builder.SetCurrentBlock(thenEdgeLabel);
                    auto thenAddress = co_await EnsureStructAddress(
                        *thenRes.Value,
                        resultTypeId,
                        "$if_then",
                        ife->Then->Location,
                        scope.Id.Id);
                    thenEdgeLabel = Builder.CurrentBlockLabel();
                    emitter.Emit0("jmp"_op, {endLabel});

                    auto elseEdgeLabel = elseRes.ProducingLabel;
                    Builder.SetCurrentBlock(elseEdgeLabel);
                    auto elseAddress = co_await EnsureStructAddress(
                        *elseRes.Value,
                        resultTypeId,
                        "$if_else",
                        ife->Else->Location,
                        scope.Id.Id);
                    elseEdgeLabel = Builder.CurrentBlockLabel();
                    emitter.Emit0("jmp"_op, {endLabel});

                    Builder.NewBlock(endLabel);
                    auto res = emitter.Emit1("phi"_op, {thenAddress, thenEdgeLabel, elseAddress, elseEdgeLabel});
                    Builder.SetType(res, Module.Types.Ptr(resultTypeId));
                    co_return TValueWithBlock{ res, Builder.CurrentBlockLabel() };
                }

                if (!thenTerminated) {
                    Builder.SetCurrentBlock(thenEdgeLabel);
                    emitter.Emit0("jmp"_op, {endLabel});
                }
                auto elseEdgeLabel = elseRes.ProducingLabel;
                Builder.SetCurrentBlock(elseEdgeLabel);
                emitter.Emit0("jmp"_op, {endLabel});
                Builder.NewBlock(endLabel);
                auto res = emitter.Emit1("phi"_op, {*thenRes.Value, thenEdgeLabel, *elseRes.Value, elseEdgeLabel});
                Builder.SetType(res, resultTypeId);
                if (thenRes.Value->Type == TOperand::EType::Tmp) {
                    Builder.UnifyTypes(res, thenRes.Value->Tmp);
                }
                if (elseRes.Value->Type == TOperand::EType::Tmp) {
                    Builder.UnifyTypes(res, elseRes.Value->Tmp);
                }
                co_return TValueWithBlock{ res, Builder.CurrentBlockLabel() };
            }

            if (!elseTerminated) {
                Builder.SetCurrentBlock(elseRes.ProducingLabel);
                emitter.Emit0("jmp"_op, {endLabel});
            }
            Builder.NewBlock(endLabel);
            co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
        } else {
            auto endLabel = Builder.NewLabel();
            if (!thenTerminated) {
                Builder.SetCurrentBlock(thenEdgeLabel);
                emitter.Emit0("jmp"_op, {endLabel});
            }
            Builder.SetCurrentBlock(elseId);
            emitter.Emit0("jmp"_op, {endLabel});
            Builder.NewBlock(endLabel);
            co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
        }

    } else if (auto maybeLoop = NAst::TMaybeNode<NAst::TWhileStmtExpr>(expr)) {
        co_return co_await LowerWhile(maybeLoop.Cast(), scope);
    } else if (auto maybeLoop = NAst::TMaybeNode<NAst::TRepeatStmtExpr>(expr)) {
        co_return co_await LowerRepeat(maybeLoop.Cast(), scope);
    } else if (auto maybeLoop = NAst::TMaybeNode<NAst::TForStmtExpr>(expr)) {
        co_return co_await LowerFor(maybeLoop.Cast(), scope);
    } else if (auto maybeLoop = NAst::TMaybeNode<NAst::TTimesStmtExpr>(expr)) {
        co_return co_await LowerTimes(maybeLoop.Cast(), scope);
    } else if (NAst::TMaybeNode<NAst::TBreakStmt>(expr)) {
        co_return TError(expr->Location, "break must be normalized before lowering");
    } else if (NAst::TMaybeNode<NAst::TContinueStmt>(expr)) {
        co_return TError(expr->Location, "continue must be normalized before lowering");
    } else if (NAst::TMaybeNode<NAst::TReturnExpr>(expr)) {
        co_return TError(expr->Location, "return must be normalized before lowering");
    } else if (auto maybeAsg = NAst::TMaybeNode<NAst::TArrayAssignExpr>(expr)) {
        auto asg = maybeAsg.Cast();

        auto arrayPtr = co_await LoadVar(asg->Name, scope, asg->Location);
        auto arraySymbol = Context.Lookup(asg->Name, scope.Id);
        if (!arraySymbol) {
            co_return TError(asg->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }
        auto arrayType = Builder.GetType(arrayPtr);
        int arrayElemTypeId = Module.Types.UnderlyingType(arrayType);
        assert(arrayElemTypeId >= 0);
        int elemByteSize = Module.Types.SizeInBytes(arrayElemTypeId);
        TOperand totalIndex;
        auto symbolNode = Context.GetSymbolNode(NSemantics::TSymbolId{arraySymbol->Id});
        auto symbolType = symbolNode ? NAst::UnwrapReferenceType(NAst::UnwrapNamedType(symbolNode->Type)) : nullptr;
        if (NAst::TMaybeType<NAst::TPointerType>(symbolType) || IsRawArraySymbol(symbolNode)) {
            if (asg->Indices.size() != 1) {
                co_return TError(asg->Location, TErrorString::Get<EErrorId::FAILED_LOWER_ARRAY_INDICES>());
            }
            auto indexValue = co_await Lower(asg->Indices[0], scope);
            if (!indexValue.Value) {
                co_return TError(asg->Indices[0]->Location, TErrorString::Get<EErrorId::ARRAY_INDEX_NOT_NUMBER>());
            }
            auto i64 = Module.Types.I(EKind::I64);
            auto offset = emitter.Emit1("*"_op, {*indexValue.Value, TImm{elemByteSize, i64}});
            Builder.SetType(offset, i64);
            totalIndex = offset;
        } else {
            auto indices = co_await LowerIndices(*arraySymbol, asg->Indices, scope, elemByteSize);
            if (!indices.Value) {
                co_return TError(asg->Location, TErrorString::Get<EErrorId::FAILED_LOWER_ARRAY_INDICES>());
            }
            totalIndex = *indices.Value;
        }
        auto destPtr = emitter.Emit1("+"_op, {arrayPtr, totalIndex});
        Builder.SetType(destPtr, arrayType);

        auto rhs = co_await Lower(asg->Value, scope);
        if (!rhs.Value) co_return TError(asg->Value->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());

        if (Module.Types.GetKind(arrayElemTypeId) == EKind::Struct) {
            auto src = co_await EnsureStructAddress(
                *rhs.Value,
                arrayElemTypeId,
                "$" + asg->Name + "_assign",
                asg->Value->Location,
                scope.Id.Id);
            emitter.Emit0("copy"_op, {destPtr, src, TImm{(int64_t)elemByteSize}});
        } else {
            emitter.Emit0("ste"_op, {destPtr, *rhs.Value});
        }
        co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
    } else if (auto maybeIndex = NAst::TMaybeNode<NAst::TIndexExpr>(expr)) {
        auto index = maybeIndex.Cast();
        auto indexValue = co_await Lower(index->Index, scope);
        if (!indexValue.Value) {
            co_return TError(index->Index->Location, TErrorString::Get<EErrorId::ARRAY_INDEX_NOT_NUMBER>());
        }
        auto value = co_await Lower(index->Collection, scope);
        if (!value.Value) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::FAILED_LOWER_COLLECTION>());
        }
        auto arrayPtr = *value.Value;
        if (arrayPtr.Type != TOperand::EType::Tmp) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }

        if (NAst::TMaybeType<NAst::TVectorType>(NAst::UnwrapNamedType(NAst::UnwrapReferenceType(index->Collection->Type)))) {
            auto element = emitter.Emit1("index"_op, {*value.Value, *indexValue.Value});
            Builder.SetType(element, FromAstType(expr->Type, Module.Types));
            co_return TValueWithBlock{element, Builder.CurrentBlockLabel()};
        }

        auto maybeIndexIdent = NAst::TMaybeNode<NAst::TIdentExpr>(index->Collection);
        if (!maybeIndexIdent) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }
        auto indexSymbol = Context.Lookup(maybeIndexIdent.Cast()->Name, scope.Id);
        if (!indexSymbol) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }
        auto symbolNode = Context.GetSymbolNode(NSemantics::TSymbolId{indexSymbol->Id});
        auto symbolType = symbolNode ? NAst::UnwrapReferenceType(NAst::UnwrapNamedType(symbolNode->Type)) : nullptr;
        if (NAst::TMaybeType<NAst::TPointerType>(symbolType) || IsRawArraySymbol(symbolNode)) {
            auto arrayType = Builder.GetType(arrayPtr.Tmp);
            int elemTypeId = FromAstType(expr->Type, Module.Types);
            int elemByteSize = Module.Types.SizeInBytes(elemTypeId);
            auto i64 = Module.Types.I(EKind::I64);
            auto offset = emitter.Emit1("*"_op, {*indexValue.Value, TImm{elemByteSize, i64}});
            Builder.SetType(offset, i64);
            auto destPtr = emitter.Emit1("+"_op, {arrayPtr, offset});
            Builder.SetType(destPtr, arrayType);
            if (Module.Types.GetKind(elemTypeId) == EKind::Struct) {
                Builder.SetType(destPtr, Module.Types.Ptr(elemTypeId));
                co_return TValueWithBlock{ destPtr, Builder.CurrentBlockLabel() };
            }
            auto loaded = emitter.Emit1("lde"_op, { destPtr });
            Builder.SetType(loaded, elemTypeId);
            co_return TValueWithBlock{ loaded, Builder.CurrentBlockLabel() };
        }
        auto layoutIt = ArrayLayouts.find(indexSymbol->Id);
        if (layoutIt == ArrayLayouts.end()) {
            co_return TError(index->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }

        // Adjust index by lower bound: index0 = index - lbound0
            auto lbound0 = LoadLayoutOperand(layoutIt->second.LBounds[0], expr->Location, scope.Id.Id);
        auto i64 = Module.Types.I(EKind::I64);
        auto zeroBasedIndex = emitter.Emit1("-"_op, {*indexValue.Value, lbound0});
        Builder.SetType(zeroBasedIndex, i64);

        auto arrayType = Builder.GetType(arrayPtr.Tmp);
        int elemTypeId = FromAstType(expr->Type, Module.Types);
        int elemByteSize = Module.Types.SizeInBytes(elemTypeId);
        auto offset = emitter.Emit1("*"_op, {zeroBasedIndex, TImm{elemByteSize, i64}}); // byte offset
        Builder.SetType(offset, i64);
        auto destPtr = emitter.Emit1("+"_op, {arrayPtr, offset});
        Builder.SetType(destPtr, arrayType);
        // For struct elements: return the pointer directly (caller uses memcpy/lea semantics)
        bool isStructElem = NAst::TMaybeType<NAst::TStructType>(NAst::UnwrapNamedType(expr->Type));
        if (isStructElem) {
            Builder.SetType(destPtr, Module.Types.Ptr(elemTypeId));
            co_return TValueWithBlock{ destPtr, Builder.CurrentBlockLabel() };
        }
        auto loaded = emitter.Emit1("lde"_op, { destPtr });
        Builder.SetType(loaded, elemTypeId);
        co_return TValueWithBlock{ loaded, Builder.CurrentBlockLabel() };
    } else if (auto maybeConstruct = NAst::TMaybeNode<NAst::TStructConstructExpr>(expr)) {
        auto sc = maybeConstruct.Cast();
        auto objAstType = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(sc->Type));
        int structTypeId = FromAstType(objAstType, Module.Types);
        int64_t totalSize = Module.Types.SizeInBytes(structTypeId);
        int ptrTypeId = Module.Types.Ptr(structTypeId);
        auto i64 = Module.Types.I(EKind::I64);

        auto ptr = emitter.Emit1("salloc"_op, {TImm{totalSize, i64}});
        Builder.SetType(ptr, ptrTypeId);

        const auto& fieldTypes = Module.Types.GetStructFields(structTypeId);
        auto astStructType = NAst::TMaybeType<NAst::TStructType>(objAstType).Cast();
        for (size_t i = 0; i < sc->Fields.size(); ++i) {
            auto fieldVal = co_await Lower(sc->Fields[i], scope);
            if (!fieldVal.Value) {
                co_return TError(sc->Fields[i]->Location, "Не удалось вычислить поле при конструировании структуры.");
            }
            int fieldIndex = static_cast<int>(i);
            if (i < sc->FieldNames.size()) {
                fieldIndex = -1;
                if (astStructType) {
                    for (size_t j = 0; j < astStructType->Fields.size(); ++j) {
                        if (astStructType->Fields[j].first == sc->FieldNames[i]) {
                            fieldIndex = static_cast<int>(j);
                            break;
                        }
                    }
                }
                if (fieldIndex < 0) {
                    co_return TError(sc->Fields[i]->Location,
                        "Поле '" + sc->FieldNames[i] + "' не найдено при конструировании структуры.");
                }
            }
            int64_t offset = Module.Types.FieldOffset(structTypeId, fieldIndex);
            int fieldTypeId = fieldIndex < static_cast<int>(fieldTypes.size()) ? fieldTypes[fieldIndex] : -1;
            auto fieldPtr = emitter.Emit1("+"_op, {ptr, TImm{offset, i64}});
            Builder.SetType(fieldPtr, Module.Types.Ptr(fieldTypeId));
            if (Module.Types.GetKind(fieldTypeId) == EKind::Struct) {
                int fieldSize = Module.Types.SizeInBytes(fieldTypeId);
                auto src = co_await EnsureStructAddress(
                    *fieldVal.Value,
                    fieldTypeId,
                    "$field_value",
                    sc->Fields[i]->Location,
                    scope.Id.Id);
                emitter.Emit0("copy"_op, {fieldPtr, src, TImm{(int64_t)fieldSize}});
            } else {
                emitter.Emit0("ste"_op, {fieldPtr, *fieldVal.Value});
            }
        }
        co_return TValueWithBlock{ ptr, Builder.CurrentBlockLabel() };
    } else if (NAst::TMaybeNode<NAst::TFieldAccessExpr>(expr) || NAst::TMaybeNode<NAst::TFieldAssignExpr>(expr)) {
        auto maybeRead  = NAst::TMaybeNode<NAst::TFieldAccessExpr>(expr);
        auto maybeWrite = NAst::TMaybeNode<NAst::TFieldAssignExpr>(expr);
        NAst::TExprPtr& object = maybeRead ? maybeRead.Cast()->Object : maybeWrite.Cast()->Object;
        int fieldIndex = maybeRead ? maybeRead.Cast()->FieldIndex : maybeWrite.Cast()->FieldIndex;
        const std::string& fieldName = maybeRead ? maybeRead.Cast()->FieldName : maybeWrite.Cast()->FieldName;

        TOperand objAddr;
        if (maybeRead) {
            if (IsAddressableFieldBase(object)) {
                auto res = co_await LowerLValueAddress(object, scope);
                if (!res.Value) {
                    co_return TError(object->Location, "Не удалось вычислить объект при обращении к полю '" + fieldName + "'.");
                }
                objAddr = *res.Value;
            } else {
                auto res = co_await Lower(object, scope);
                if (!res.Value) {
                    co_return TError(object->Location, "Не удалось вычислить объект при обращении к полю '" + fieldName + "'.");
                }
                objAddr = *res.Value;
            }
        } else {
            auto res = co_await LowerLValueAddress(object, scope);
            if (!res.Value) {
                co_return TError(object->Location, "Не удалось вычислить объект при обращении к полю '" + fieldName + "'.");
            }
            objAddr = *res.Value;
        }

        auto objAstType = NAst::UnwrapReferenceType(NAst::UnwrapNamedType(object->Type));
        int structTypeId = FromAstType(objAstType, Module.Types);
        int64_t fieldByteOffset = Module.Types.FieldOffset(structTypeId, fieldIndex);

        // Dispatch: read or write
        NAst::TTypePtr fieldAstType = maybeRead ? expr->Type : maybeWrite.Cast()->Value->Type;
        int fieldTypeId = FromAstType(fieldAstType, Module.Types);

        auto i64      = Module.Types.I(EKind::I64);
        auto fieldPtr  = emitter.Emit1("+"_op, {objAddr, TImm{fieldByteOffset, i64}});
        Builder.SetType(fieldPtr, Module.Types.Ptr(fieldTypeId));

        if (maybeRead) {
            if (Module.Types.GetKind(fieldTypeId) == EKind::Struct) {
                co_return TValueWithBlock{ fieldPtr, Builder.CurrentBlockLabel() };
            }
            auto loaded = emitter.Emit1("lde"_op, { fieldPtr });
            Builder.SetType(loaded, fieldTypeId);
            co_return TValueWithBlock{ loaded, Builder.CurrentBlockLabel() };
        } else {
            auto rhs = co_await Lower(maybeWrite.Cast()->Value, scope);
            if (!rhs.Value) {
                co_return TError(maybeWrite.Cast()->Value->Location, "Не удалось вычислить значение при присваивании полю '" + fieldName + "'.");
            }
            if (Module.Types.GetKind(fieldTypeId) == EKind::Struct) {
                auto rootName = RootVariableName(object);
                auto tempName = rootName.empty()
                    ? "$field_assign"
                    : "$" + rootName + "_assign";
                auto src = co_await EnsureStructAddress(
                    *rhs.Value,
                    fieldTypeId,
                    tempName,
                    maybeWrite.Cast()->Value->Location,
                    scope.Id.Id);
                emitter.Emit0("copy"_op, {fieldPtr, src, TImm{(int64_t)Module.Types.SizeInBytes(fieldTypeId)}});
            } else {
                emitter.Emit0("ste"_op, {fieldPtr, *rhs.Value});
            }
            co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
        }
    } else if (auto maybeMultiIndex = NAst::TMaybeNode<NAst::TMultiIndexExpr>(expr)) {
        auto multiIndex = maybeMultiIndex.Cast();
        auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(multiIndex->Collection);
        if (!maybeIdent) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::MULTI_INDEX_COLLECTION_MUST_BE_IDENTIFIER>());
        }
        auto asg = maybeIdent.Cast();
        auto arraySymbol = Context.Lookup(asg->Name, scope.Id);
        if (!arraySymbol) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::UNDEFINED_NAME>());
        }
        auto value = co_await Lower(multiIndex->Collection, scope);
        if (!value.Value) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::FAILED_LOWER_COLLECTION>());
        }
        auto arrayPtr = *value.Value;
        if (arrayPtr.Type != TOperand::EType::Tmp) {
            co_return TError(multiIndex->Collection->Location, TErrorString::Get<EErrorId::COLLECTION_NOT_ARRAY>());
        }
        auto arrayType = Builder.GetType(arrayPtr.Tmp);
        int multiElemTypeId = FromAstType(expr->Type, Module.Types);
        int multiElemByteSize = Module.Types.SizeInBytes(multiElemTypeId);
        auto indices = co_await LowerIndices(*arraySymbol, multiIndex->Indices, scope, multiElemByteSize);
        if (!indices.Value) {
            co_return TError(asg->Location, TErrorString::Get<EErrorId::FAILED_LOWER_ARRAY_INDICES>());
        }
        auto totalIndex = *indices.Value;
        auto destPtr = emitter.Emit1("+"_op, {arrayPtr, totalIndex});
        Builder.SetType(destPtr, arrayType);
        if (Module.Types.GetKind(multiElemTypeId) == EKind::Struct) {
            Builder.SetType(destPtr, Module.Types.Ptr(multiElemTypeId));
            co_return TValueWithBlock{ destPtr, Builder.CurrentBlockLabel() };
        }
        auto loaded = emitter.Emit1("lde"_op, { destPtr });
        Builder.SetType(loaded, FromAstType(expr->Type, Module.Types));
        co_return TValueWithBlock{ loaded, Builder.CurrentBlockLabel() };
    } else if (auto maybeAsg = NAst::TMaybeNode<NAst::TAssignExpr>(expr)) {
        auto asg = maybeAsg.Cast();
        auto rhs = co_await Lower(asg->Value, scope);
        if (!rhs.Value) co_return TError(asg->Value->Location, TErrorString::Get<EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER>());
        auto sidOpt = Context.Lookup(asg->Name, scope.Id);
        if (!sidOpt) co_return TError(asg->Location, TErrorString::Get<EErrorId::ASSIGNMENT_TO_UNDEFINED>());

        DebugPoints_.Bind(asg->Location, sidOpt->FunctionLevelIdx, asg->Name);
        auto node = Context.GetSymbolNode(NSemantics::TSymbolId{sidOpt->Id});
        auto slotType = FromAstType(node->Type, Module.Types);

        auto storeSlot = TSlot{sidOpt->Id};
        auto localSlot = TLocal{sidOpt->FunctionLevelIdx};
        if (localSlot.Idx >= 0) {
            Builder.SetType(
                localSlot,
                slotType,
                LocalDebugInfo(asg->Name, node->Location, sidOpt->DeclScopeId, node->Type));
        }
        TOperand storeOperand = (localSlot.Idx >= 0) ? TOperand{localSlot} : TOperand{storeSlot};
        // slot type was set on variable declaration

        auto nodeType = NAst::UnwrapNamedType(node->Type);

        if (auto maybeRef = NAst::TMaybeType<NAst::TReferenceType>(nodeType)) {
            auto ref = maybeRef.Cast();
            auto addrTmp = emitter.Emit1("load"_op, {storeOperand});
            Builder.SetType(addrTmp, FromAstType(node->Type, Module.Types));

            auto refType = NAst::UnwrapNamedType(ref->ReferencedType);
            if (NAst::TMaybeType<NAst::TStructType>(refType).Cast() != nullptr) {
                // рез компл: addrTmp is Tmp pointer to destination, rhs is Tmp pointer to source
                int refTypeId = FromAstType(refType, Module.Types);
                int sizeBytes = Module.Types.SizeInBytes(refTypeId);
                auto src = co_await EnsureStructAddress(
                    *rhs.Value,
                    refTypeId,
                    "$" + asg->Name + "_assign",
                    asg->Value->Location,
                    scope.Id.Id);
                emitter.Emit0("copy"_op, {addrTmp, src, TImm{(int64_t)sizeBytes}});
            } else {
                // see cases/ref/index_ref
                emitter.Emit0("ste"_op, {addrTmp, *rhs.Value});
            }
        } else {
            auto storedValue = *rhs.Value;
            int storedTypeId = FromAstType(node->Type, Module.Types);
            if (localSlot.Idx >= 0 && Module.Types.GetKind(storedTypeId) == EKind::Struct) {
                storedValue = co_await EnsureStructAddress(
                    storedValue,
                    storedTypeId,
                    "$" + asg->Name + "_assign",
                    asg->Value->Location,
                    scope.Id.Id);
            }
            emitter.Emit0("stre"_op, {storeOperand, storedValue});
        }

        // store does not produce a value
        co_return TValueWithBlock{ {}, Builder.CurrentBlockLabel() };
    } else if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(expr)) {
        auto ident = maybeIdent.Cast();
        auto tmp = co_await LoadVar(ident->Name, scope, ident->Location);
        // if variable is a ref, need to dereference
        auto var = Context.Lookup(ident->Name, scope.Id);
        auto node = Context.GetSymbolNode(NSemantics::TSymbolId{var->Id});
        // tmp contains the address of the variable
        if (auto maybeRef = NAst::TMaybeType<NAst::TReferenceType>(node->Type)) {
            auto ref = maybeRef.Cast();
            auto refType = NAst::UnwrapNamedType(ref->ReferencedType);
            if (NAst::TMaybeType<NAst::TStructType>(refType)) {
                Builder.SetType(tmp, Module.Types.Ptr(FromAstType(ref->ReferencedType, Module.Types)));
            } else {
                auto derefTmp = emitter.Emit1("lde"_op, { tmp });
                Builder.SetType(derefTmp, FromAstType(ref->ReferencedType, Module.Types));
                tmp = derefTmp;
            }
        }

        co_return TValueWithBlock{ tmp, Builder.CurrentBlockLabel() };
    } else if (auto maybeVar = NAst::TMaybeNode<NAst::TVarStmt>(expr)) {
        // TODO: zero memory for strings?
        auto var = maybeVar.Cast();
        auto sidOpt = Context.Lookup(var->Name, scope.Id);
        if (!sidOpt) {
            co_return TError(var->Location, TErrorString::Get<EErrorId::VAR_HAS_NO_BINDING>());
        }
        if (sidOpt->FunctionLevelIdx >= 0) {
            SetLocalType(TLocal{sidOpt->FunctionLevelIdx},
                FromAstType(var->Type, Module.Types),
                LocalDebugInfo(var->Name, var->Location, sidOpt->DeclScopeId, var->Type), true);
        }
        if (var->Init) {
            auto assign = std::make_shared<NAst::TAssignExpr>(var->Location, var->Name, var->Init);
            assign->Type = std::make_shared<NAst::TVoidType>();
            auto assignRes = co_await Lower(assign, scope);
            Builder.SetCurrentBlock(assignRes.ProducingLabel);
        }
        if (NAst::TMaybeType<NAst::TArrayType>(var->Type)) {
            auto arrayType = FromAstType(var->Type, Module.Types);
            auto elemType = Module.Types.UnderlyingType(arrayType);
            auto elemSize = Module.Types.SizeInBytes(elemType);
            auto ctorId = co_await GlobalSymbolId("array_create");

            auto layout = co_await LowerArrayLayout(*sidOpt, var->Bounds, scope, var->Name, var->Location);
            auto tmp = LoadLayoutOperand(layout.TotalElements, expr->Location, scope.Id.Id);
            auto i64 = Module.Types.I(EKind::I64);
            auto arraySize = emitter.Emit1("*"_op, {tmp, TImm{elemSize, i64}});
            Builder.SetType(arraySize, i64);

            emitter.Emit0("arg"_op, {arraySize});
            auto arrayPtr = emitter.Emit1("call"_op, {TImm{ctorId}});
            Builder.SetType(arrayPtr, arrayType);

            TOperand arg = (sidOpt->FunctionLevelIdx >= 0)
                ? TOperand { TLocal{ sidOpt->FunctionLevelIdx } }
                : TOperand { TSlot{ sidOpt->Id } };
            emitter.Emit0("stre"_op, {arg, arrayPtr});
        }
        co_return TValueWithBlock{ std::nullopt, Builder.CurrentBlockLabel() };
    } else if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(expr)) {
        auto fun = maybeFun.Cast();
        // External functions have no body to lower; call sites import them.
        if (fun->IsExternal()) {
            co_return TValueWithBlock{ std::nullopt, TLabel{} };
        }
        // Emit under MangledName when set (cacheable/overloaded symbol), else Name.
        auto name = fun->MangledName.empty() ? fun->Name : fun->MangledName;
        if (scope.Id.Id != 0) {
            co_return TError(fun->Location, TErrorString::Get<EErrorId::NESTED_FUNCTIONS_NOT_SUPPORTED>());
        }
        auto params = fun->Params;
        auto body = fun->Body;
        auto type = NAst::TMaybeType<NAst::TFunctionType>(fun->Type).Cast();
        // Explicit symbol lookup (synchronous) instead of co_await on optionals.
        // Prefer direct decl binding (robust for nested functions where scope.Id may not be the decl scope).
        auto sidOpt = Context.Lookup(fun->Name, scope.Id);
        if (!sidOpt) {
            co_return TError(fun->Location, TErrorString::Get<EErrorId::UNBOUND_FUNCTION_SYMBOL>(fun->Name, scope.Id.Id));
        }
        auto funScope = fun->Body->Scope;
        auto functionScope = fun->Scope;
        emitter.DebugInfo = InstDebugInfo(fun->Location, functionScope);
        std::vector<TLocal> args; args.reserve(params.size());
        int i = 0;
        for (auto& p : params) {
            auto psid = Context.Lookup(p->Name, NSemantics::TScopeId{funScope});
            if (!psid) {
                co_return TError(p->Location, TErrorString::Get<EErrorId::PARAMETER_NO_BINDING>());
            }
            auto local = TLocal{ psid->FunctionLevelIdx };
            args.push_back(local);
            i++;
        }

        // auto currentFuncIdx = Builder.CurrentFunctionIdx(); // needed for nested functions
        auto funcIdx = Builder.NewFunction(name, args, sidOpt->Id);
        if (Module.DebugOptions.CollectMetadata()) {
            Module.Functions[funcIdx].DebugInfo = TFunctionDebugInfo{
                .Name = fun->OriginalName.empty() ? fun->Name : fun->OriginalName,
                .Location = fun->Location,
                .ScopeId = functionScope,
                .AstType = fun->Type,
            };
        }
        auto coroutineResultType = NAst::FutureResultType(fun->RetType);
        const bool isCoroutine = static_cast<bool>(coroutineResultType);
        auto physicalReturnAstType = isCoroutine
            ? std::make_shared<NAst::TPointerType>(std::make_shared<NAst::TVoidType>())
            : fun->RetType;
        auto returnType = FromAstType(physicalReturnAstType, Module.Types);
        Builder.SetReturnType(returnType);
        Module.Functions[funcIdx].IsCoroutine = isCoroutine;
        Module.Functions[funcIdx].Cacheable = fun->Cacheable;
        Module.Functions[funcIdx].Inline = fun->Inline;
        if (isCoroutine) {
            Module.Functions[funcIdx].CoroutineResultTypeId = FromAstType(coroutineResultType, Module.Types);
        }
        for (auto& a : args) {
            auto index = &a - &args[0];
            auto astType = type->ParamTypes[index];
            auto name = params[index]->Name;
            Builder.SetType(a,
                FromAstType(astType, Module.Types),
                LocalDebugInfo(name, params[index]->Location, functionScope, params[index]->Type));
        }
        DebugPoints_.Point("enter", InstDebugInfo(fun->Location, functionScope));
        for (auto arg : args) {
            DebugPoints_.Declare(arg, Module.Functions[funcIdx].LocalDebugInfo.empty()
                ? TLocalVarDebugInfo{} : Module.Functions[funcIdx].LocalDebugInfo[arg.Idx]);
        }
        int localCount = 0;
        for (const auto& symbol : Context.GetSymbols()) {
            if (symbol.FuncScopeId.Id == functionScope && symbol.FunctionLevelIdx >= 0) {
                localCount = std::max(localCount, symbol.FunctionLevelIdx + 1);
            }
        }
        Builder.ReserveLocals(localCount);
        if (NAst::TMaybeType<NAst::TStringType>(isCoroutine ? coroutineResultType : fun->RetType)) {
            // TODO: remove me
            // clutch: support string returnType
            Module.Functions[Builder.CurrentFunctionIdx()].ReturnTypeIsString = true;
        }

        // retLocal holds the function's return value, populated by every
        // `(return ...)` and (for hand-written core lang bodies without a
        // trailing return) by the body's fall-through value.
        auto retAstType = isCoroutine ? coroutineResultType : fun->RetType;
        std::optional<TLocal> retLocal;
        std::optional<int> retTypeId;
        if (!NAst::TMaybeType<NAst::TVoidType>(retAstType)) {
            retTypeId = FromAstType(retAstType, Module.Types);
            retLocal = Builder.AllocLocal(
                *retTypeId,
                LocalDebugInfo("$return", fun->Location, functionScope, retAstType));
        }

        // Create a dedicated final return block label beforehand and pass it
        // as ReturnLabel for `return` and early exits.
        auto endLabel = Builder.NewLabel();

        TBlockScope functionBodyScope {
            .FuncIdx = funcIdx,
            .Id = NSemantics::TScopeId{funScope},
            .BreakLabel = std::nullopt,
            .ContinueLabel = std::nullopt,
            .ReturnLabel = endLabel,
            .RetLocal = retLocal,
            .RetTypeId = retTypeId,
            .LastAssert = fun->LastAssert
        };
        for (auto& param : fun->Params) {
            if (param->Bounds.empty() || !NAst::TMaybeType<NAst::TArrayType>(param->Type)) {
                continue;
            }
            auto psid = Context.Lookup(param->Name, NSemantics::TScopeId{funScope});
            if (!psid) {
                co_return TError(param->Location, TErrorString::Get<EErrorId::PARAMETER_NO_BINDING>());
            }
            co_await LowerArrayLayout(*psid, param->Bounds, functionBodyScope, param->Name, param->Location);
        }

        auto loweredBody = co_await Lower(body, functionBodyScope);
        // Jump to final return block unless already terminated by earlier logic (e.g. explicit `return`).
        if (!Builder.IsCurrentBlockTerminated()) {
            if (retLocal) {
                if (!loweredBody.Value) {
                    // Body fell off the end without an explicit `return` and its
                    // last statement does not produce a value (hand-written core
                    // lang body without a trailing `return` must end in a
                    // value-producing expression).
                    co_return TError(fun->Location, "Тело функции должно заканчиваться оператором `return` или выражением, возвращающим значение.");
                }
                auto returnValue = *loweredBody.Value;
                if (retTypeId && Module.Types.GetKind(*retTypeId) == EKind::Struct) {
                    returnValue = co_await EnsureStructAddress(
                        returnValue,
                        *retTypeId,
                        "$return_value",
                        body->Location,
                        functionBodyScope.Id.Id);
                }
                emitter.Emit0("stre"_op, {TOperand{*retLocal}, returnValue});
            }
            emitter.Emit0("jmp"_op, { endLabel });
        }
        // Materialize final return block and emit single ret there.
        Builder.NewBlock(endLabel);
        if (retLocal) {
            TTmp returned;
            if (retTypeId && Module.Types.GetKind(*retTypeId) == EKind::Struct) {
                returned = emitter.Emit1("lea"_op, {TOperand{*retLocal}});
                Builder.SetType(returned, Module.Types.Ptr(*retTypeId));
            } else {
                returned = emitter.Emit1("load"_op, {TOperand{*retLocal}});
                Builder.SetType(returned, *retTypeId);
            }
            emitter.Emit0("ret"_op, {returned});
        } else {
            emitter.Emit0("ret"_op, {});
        }
        co_return TValueWithBlock{ {}, Builder.CurrentBlockLabel() };
    } else if (auto maybeAwait = NAst::TMaybeNode<NAst::TAwaitExpr>(expr)) {
        auto awaitExpr = maybeAwait.Cast();
        auto future = co_await Lower(awaitExpr->Operand, scope);
        if (!future.Value) {
            co_return TError(awaitExpr->Location, "await operand did not produce a future");
        }

        std::optional<TOperand> tmp = std::nullopt;
        if (NAst::TMaybeType<NAst::TVoidType>(awaitExpr->Type)) {
            emitter.Emit0("await"_op, {*future.Value});
        } else {
            tmp = emitter.Emit1("await"_op, {*future.Value});
            Builder.SetType(tmp->Tmp, FromAstType(awaitExpr->Type, Module.Types));
        }

        co_return TValueWithBlock{ tmp, Builder.CurrentBlockLabel() };
    } else if (auto maybeCall = NAst::TMaybeNode<NAst::TCallExpr>(expr)) {
        auto call = maybeCall.Cast();
        // Evaluate callee and perform a function call.

        int32_t calleeSymId = -1;
        std::string calleeName;
        if (auto maybeIdent = NAst::TMaybeNode<NAst::TIdentExpr>(call->Callee)) {
            auto ident = maybeIdent.Cast();
            auto sidOpt = Context.Lookup(ident->Name, scope.Id);
            calleeName = ident->Name;
                if (!sidOpt) co_return TError(ident->Location, TErrorString::Get<EErrorId::UNBOUND_FUNCTION_SYMBOL>(ident->Name, scope.Id.Id));
            calleeSymId = sidOpt->Id;
        } else {
            co_return TError(call->Callee->Location, TErrorString::Get<EErrorId::FUNCTION_CALL_NON_IDENTIFIER>());
        }

        auto funDecl = NAst::TMaybeNode<NAst::TFunDecl>(Context.GetSymbolNode(NSemantics::TSymbolId{calleeSymId})).Cast();
        if (!funDecl) {
            co_return TError(call->Callee->Location, TErrorString::Get<EErrorId::NOT_A_FUNCTION>());
        }
        if (auto op = NSemantics::VectorIntrinsic(funDecl->MangledName); !op.empty()) {
            co_return co_await LowerVectorIntrinsic(*call, op, scope);
        }
        auto futureResultType = NAst::FutureResultType(funDecl->RetType);
        NAst::TTypePtr returnType = futureResultType
            ? std::make_shared<NAst::TPointerType>(std::make_shared<NAst::TVoidType>())
            : PhysicalCallResultType(funDecl->RetType);
        std::vector<NAst::TTypePtr>* argTypes = nullptr;
        if (auto maybeFuncType = NAst::TMaybeType<NAst::TFunctionType>(funDecl->Type)) {
            argTypes = &maybeFuncType.Cast()->ParamTypes;
        }

        bool isExternalCall = funDecl->IsExternal();
        if (isExternalCall && !Module.SymIdToExtFuncIdx.contains(calleeSymId)) {
            ImportExternalFunction(calleeSymId, *funDecl);
        }

        std::vector<TOperand> argv;
        argv.reserve(call->Args.size());
        int i = 0;
        for (auto& a : call->Args) {
            auto& argType = (*argTypes)[i++];
            TValueWithBlock av;

            if (NAst::TMaybeType<NAst::TReferenceType>(argType)) {
                auto valueType = NAst::UnwrapNamedType(NAst::UnwrapReferenceType(a->Type));
                if (NAst::TMaybeType<NAst::TVectorType>(valueType)
                    && !NAst::TMaybeNode<NAst::TIdentExpr>(a) && !NAst::TMaybeNode<NAst::TFieldAccessExpr>(a)
                    && !NAst::TMaybeNode<NAst::TIndexExpr>(a))
                {
                    auto value = co_await Lower(a, scope);
                    if (!value.Value) {
                        co_return TError(a->Location, TErrorString::Get<EErrorId::INVALID_ARGUMENT>());
                    }
                    const auto address = MaterializeValue(*value.Value, FromAstType(valueType, Module.Types),
                        "$vector_argument", a->Location, scope.Id.Id);
                    av = TValueWithBlock{address, Builder.CurrentBlockLabel()};
                } else {
                    av = co_await LowerLValueAddress(a, scope);
                }
                if (av.Value && av.Value->Type == TOperand::EType::Tmp) {
                    Builder.SetType(av.Value->Tmp, FromAstType(argType, Module.Types));
                }
            } else {
                av = co_await Lower(a, scope);
                int argTypeId = FromAstType(argType, Module.Types);
                if (av.Value && Module.Types.GetKind(argTypeId) == EKind::Struct) {
                    // By-value struct ABI consumes an address-backed rvalue.
                    av.Value = co_await EnsureStructAddress(
                        *av.Value,
                        argTypeId,
                        "$argument",
                        a->Location,
                        scope.Id.Id);
                }
            }

            if (!av.Value) co_return TError(a->Location, TErrorString::Get<EErrorId::INVALID_ARGUMENT>());

            argv.push_back(*av.Value);
        }
        if (funDecl->NeedsLocator) {
            auto locatorId = co_await GlobalSymbolId("builtin::record_locator");
            const int i64 = Module.Types.I(EKind::I64);
            emitter.Emit0("arg"_op, {TImm{.Value = call->Location.Line, .TypeId = i64}});
            emitter.Emit0("arg"_op, {TImm{.Value = call->Location.Byte, .TypeId = i64}});
            emitter.Emit0("arg"_op, {TImm{.Value = call->Location.Column, .TypeId = i64}});
            emitter.Emit0("call"_op, {TImm{locatorId}});
        }
        for (auto arg : argv) {
            emitter.Emit0("arg"_op, {arg});
        }
        // Decide if callee returns a value (non-void). If void -> Emit0
        bool returnsValue = false;
        if (NAst::TMaybeType<NAst::TVoidType>(returnType)) {
            returnsValue = false;
        } else {
            returnsValue = true;
        }

        std::optional<TOperand> tmp = std::nullopt;
        if (returnsValue) {
            tmp = emitter.Emit1("call"_op, {TImm{calleeSymId}});
            int returnTypeId = FromAstType(returnType, Module.Types);
            Builder.SetType(tmp->Tmp, returnTypeId);
            if (Module.Types.GetKind(returnTypeId) == EKind::Struct) {
                tmp = co_await EnsureStructAddress(*tmp, returnTypeId, "$call_result", call->Location, scope.Id.Id);
            }
        } else {
            emitter.Emit0("call"_op, {TImm{calleeSymId}});
        }
        co_return TValueWithBlock{ tmp, Builder.CurrentBlockLabel() };
    } else {
        std::ostringstream oss;
        oss << expr;
        co_return TError(expr->Location, TErrorString::Get<EErrorId::NOT_IMPLEMENTED_LOWERING>(oss.str()));
    }
}

void TAstLowerer::ImportExternalFunction(int symbolId, const NAst::TFunDecl& funcDecl) {
    if (Module.SymIdToExtFuncIdx.find(symbolId) != Module.SymIdToExtFuncIdx.end()) {
        // already imported
        return;
    }
    // Pure inline functions have no physical implementation - skip registration
    // so backends (LLVM, WASM) don't emit imports or declarations for them.
    if (funcDecl.InlineFactory.has_value() || !NSemantics::VectorIntrinsic(funcDecl.MangledName).empty()) {
        return;
    }

    std::vector<int> argTypes; argTypes.reserve(funcDecl.Params.size());
    NAst::TTypePtr physRetType = NAst::FutureResultType(funcDecl.RetType)
        ? std::make_shared<NAst::TPointerType>(std::make_shared<NAst::TVoidType>())
        : PhysicalCallResultType(funcDecl.RetType);
    int returnType = FromAstType(physRetType, Module.Types);
    for (auto& p : funcDecl.Params) {
        argTypes.push_back(FromAstType(p->Type, Module.Types));
    }

    TExternalFunction func {
        .Name = funcDecl.Name,
        .MangledName = funcDecl.MangledName,
        .ArgTypes = std::move(argTypes),
        .ReturnTypeId = returnType,
        .Packed = funcDecl.Packed,
        .SymId = symbolId
    };
    int funIdx = Module.ExternalFunctions.size();
    Module.ExternalFunctions.push_back(func);
    Module.SymIdToExtFuncIdx[symbolId] = funIdx;
}

TExpectedTask<int, TError, TLocation> TAstLowerer::GlobalSymbolId(const std::string& name) {
    auto sidOpt = Context.Lookup(name, NSemantics::TScopeId{0});
    if (sidOpt) {
        auto funDecl = NAst::TMaybeNode<NAst::TFunDecl>(
            Context.GetSymbolNode(NSemantics::TSymbolId{sidOpt->Id}));
        if (funDecl && funDecl.Cast()->IsExternal()) {
            ImportExternalFunction(sidOpt->Id, *funDecl.Cast());
        }
        co_return sidOpt->Id;
    }
    co_return TError({}, TErrorString::Get<EErrorId::UNDEFINED_GLOBAL_SYMBOL>(name));
}

void TAstLowerer::ImportExternalFunctions() {
    for (const auto& [symbolId, func] : Context.GetExternalFunctions()) {
        ImportExternalFunction(symbolId, *func);
    }
}

std::expected<std::monostate, TError> TAstLowerer::LowerTop(const NAst::TExprPtr& expr) {
    NSemantics::TLifetimeValidator validator(Context);
    if (auto validation = validator.Validate(expr); !validation) {
        return std::unexpected(validation.error());
    }
    auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(expr);
    if (!maybeBlock) {
        return std::unexpected(TError(expr->Location, TErrorString::Get<EErrorId::ROOT_EXPR_MUST_BE_BLOCK>()));
    }
    auto block = maybeBlock.Cast();

    auto scope = TBlockScope {
        .FuncIdx = -1,
        .Id = NSemantics::TScopeId{0}
    };
    TInstrEmitter emitter{Builder, InstDebugInfo(expr->Location, scope.Id.Id), DebugPoints_, Module.DebugOptions.EmitDebugInfo};
    block->Scope = scope.Id.Id;

    std::unordered_set<int32_t> reachableFunctions;
    std::function<void(const NAst::TExprPtr&, NSemantics::TScopeId)> collectReferencedFunctions;
    std::function<void(int32_t)> markFunctionReachable;

    markFunctionReachable = [&](int32_t symbolId) {
        if (!reachableFunctions.insert(symbolId).second) {
            return;
        }
        auto fun = NAst::TMaybeNode<NAst::TFunDecl>(
            Context.GetSymbolNode(NSemantics::TSymbolId{symbolId}));
        if (!fun || fun.Cast()->IsExternal()) {
            return;
        }
        if (fun.Cast()->Body) {
            collectReferencedFunctions(fun.Cast()->Body, NSemantics::TScopeId{fun.Cast()->Body->Scope});
        }
        if (fun.Cast()->LastAssert) {
            collectReferencedFunctions(fun.Cast()->LastAssert, NSemantics::TScopeId{fun.Cast()->Body->Scope});
        }
    };

    collectReferencedFunctions = [&](const NAst::TExprPtr& node, NSemantics::TScopeId currentScope) {
        if (!node) {
            return;
        }
        if (auto block = NAst::TMaybeNode<NAst::TBlockExpr>(node)) {
            currentScope = NSemantics::TScopeId{block.Cast()->Scope};
        } else if (auto fun = NAst::TMaybeNode<NAst::TFunDecl>(node)) {
            if (fun.Cast()->Body) {
                collectReferencedFunctions(fun.Cast()->Body, NSemantics::TScopeId{fun.Cast()->Body->Scope});
            }
            if (fun.Cast()->LastAssert) {
                collectReferencedFunctions(fun.Cast()->LastAssert, NSemantics::TScopeId{fun.Cast()->Body->Scope});
            }
            return;
        } else if (auto call = NAst::TMaybeNode<NAst::TCallExpr>(node)) {
            if (auto ident = NAst::TMaybeNode<NAst::TIdentExpr>(call.Cast()->Callee)) {
                if (auto symbol = Context.Lookup(ident.Cast()->Name, currentScope)) {
                    if (NAst::TMaybeNode<NAst::TFunDecl>(
                            Context.GetSymbolNode(NSemantics::TSymbolId{symbol->Id})))
                    {
                        markFunctionReachable(symbol->Id);
                    }
                }
            }
        }
        for (const auto& child : node->Children()) {
            collectReferencedFunctions(child, currentScope);
        }
    };

    std::function<void(const std::shared_ptr<NAst::TBlockExpr>&, NSemantics::TScopeId)> collectTopLevelRoots =
        [&](const std::shared_ptr<NAst::TBlockExpr>& block, NSemantics::TScopeId currentScope)
    {
        for (const auto& stmt : block->Stmts) {
            Module.HasSourceModules |= stmt && stmt->Origin.has_value();
            if (auto fun = NAst::TMaybeNode<NAst::TFunDecl>(stmt)) {
                if (!stmt->Origin) {
                    if (auto symbol = Context.Lookup(fun.Cast()->Name, currentScope)) {
                        markFunctionReachable(symbol->Id);
                    }
                }
            } else if (auto nestedBlock = NAst::TMaybeNode<NAst::TBlockExpr>(stmt)) {
                collectTopLevelRoots(nestedBlock.Cast(), currentScope);
            } else {
                collectReferencedFunctions(stmt, currentScope);
            }
        }
    };
    collectTopLevelRoots(block, scope.Id);

    if (Module.DebugOptions.EmitDebugPoints) {
        if (auto symbol = Context.Lookup("__qumir_debug_point", scope.Id)) {
            Module.DebugPointSymbolId = symbol->Id;
        }
    }

    bool functionSeen = false;
    int constructorFunctionId = -1;
    int destructorFunctionId = -1;
    std::string constructorFunctionName = "$$module_constructor";
    std::string destructorFunctionName = "$$module_destructor";

    auto switchToConstructorFunction = [&]() {
        if (constructorFunctionId == -1) {
            constructorFunctionId = Builder.NewFunction(constructorFunctionName, {}, -1 /*symbolId*/);
            Builder.SetReturnType(Module.Types.I(EKind::Void));
        } else {
            Builder.SetCurrentFunction(constructorFunctionId);
        }
    };

    std::function<std::expected<std::monostate, TError>(const std::shared_ptr<NAst::TBlockExpr>&, const TBlockScope&)> lowerTopBlock = [&](const std::shared_ptr<NAst::TBlockExpr>& block, const TBlockScope& scope) -> std::expected<std::monostate, TError>
    {
        for (auto& s : block->Stmts) {
            if (auto maybeFun = NAst::TMaybeNode<NAst::TFunDecl>(s)) {
                if (auto symbol = Context.Lookup(maybeFun.Cast()->Name, scope.Id)) {
                    if (s->Origin && !reachableFunctions.contains(symbol->Id)
                        && !maybeFun.Cast()->Used) {
                        continue;
                    }
                }
                auto res = Lower(s, scope).result();
                if (!res) {
                    return std::unexpected(res.error());
                }
                functionSeen = true;
            } else if (auto maybeBlock = NAst::TMaybeNode<NAst::TBlockExpr>(s)) {
                auto res = lowerTopBlock(maybeBlock.Cast(), scope);
                if (!res) {
                    return std::unexpected(res.error());
                }
            } else if (auto maybeVar = NAst::TMaybeNode<NAst::TVarStmt>(s)) {
                if (functionSeen) {
                    return std::unexpected(TError(s->Location, TErrorString::Get<EErrorId::VARIABLE_DECLS_BEFORE_FUNS>()));
                }
                auto var = maybeVar.Cast();
                auto sidOpt = Context.Lookup(var->Name, scope.Id);
                if (!sidOpt) {
                    return std::unexpected(TError(var->Location, TErrorString::Get<EErrorId::VAR_HAS_NO_BINDING>()));
                }
                auto slotType = FromAstType(var->Type, Module.Types);
                if (Module.GlobalTypes.size() <= (size_t)sidOpt->Id) {
                    Module.GlobalTypes.resize(sidOpt->Id + 1);
                    Module.GlobalValues.resize(sidOpt->Id + 1); // TODO: unused
                }
                Module.GlobalTypes[sidOpt->Id] = slotType;

                if (NAst::TMaybeType<NAst::TArrayType>(var->Type)) {
                    // Arrays need to be constructed in the module constructor
                    switchToConstructorFunction();
                    auto lowered = Lower(s, scope).result();
                    if (!lowered) {
                        return std::unexpected(lowered.error());
                    }
                } else if (NAst::TMaybeType<NAst::TStringType>(var->Type)) {
                    // Strings need to be constructed in the module constructor
                    switchToConstructorFunction();
                    auto lowered = Lower(s, scope).result();
                    if (!lowered) {
                        return std::unexpected(lowered.error());
                    }
                } else if (var->Init) {
                    // Scalar global with an initializer: emit its store in the
                    // module constructor (otherwise the slot is never written -
                    // the VM grows global storage lazily on store, so a read
                    // would access an unbacked slot).
                    switchToConstructorFunction();
                    auto lowered = Lower(s, scope).result();
                    if (!lowered) {
                        return std::unexpected(lowered.error());
                    }
                }

            } else if (auto maybeAsg = NAst::TMaybeNode<NAst::TAssignExpr>(s)) {
                if (functionSeen) {
                        return std::unexpected(TError(s->Location, TErrorString::Get<EErrorId::VARIABLE_DECLS_BEFORE_FUNS>()));
                    }
                auto asg = maybeAsg.Cast();
                auto maybeNumber = NAst::TMaybeNode<NAst::TNumberExpr>(asg->Value);
                auto sid = Context.Lookup(asg->Name, scope.Id);
                if (!sid) {
                    return std::unexpected(TError(s->Location, TErrorString::Get<EErrorId::UNDEFINED_VARIABLE>(asg->Name)));
                }
                if (maybeNumber) {
                    auto num = maybeNumber.Cast();
                    if (num->IsFloat()) {
                        Module.GlobalValues[sid->Id] = TImm{.Value = std::bit_cast<int64_t>(num->FloatValue), .TypeId = Module.Types.I(EKind::F64)};
                    } else {
                        Module.GlobalValues[sid->Id] = TImm{.Value = num->IntValue, .TypeId = Module.Types.I(EKind::I64)};
                    }
                }
                auto maybeString = NAst::TMaybeNode<NAst::TStringLiteralExpr>(asg->Value);
                if (maybeString) {
                    auto str = maybeString.Cast();
                    auto id = Builder.StringLiteral(str->Value);
                    Module.GlobalValues[sid->Id] = TImm{.Value = id, .TypeId = Module.Types.Ptr(Module.Types.I(EKind::I8))};
                    // string globals are pointers
                    Module.GlobalTypes[sid->Id] = Module.Types.Ptr(Module.Types.I(EKind::I8));
                }

                switchToConstructorFunction();
                auto lowered = Lower(s, scope).result();
                if (!lowered) {
                    return std::unexpected(lowered.error());
                }
            } else if (NAst::TMaybeNode<NAst::TReplaceExpr>(s)) {
                if (functionSeen) {
                    return std::unexpected(TError(
                        s->Location,
                        TErrorString::Get<EErrorId::VARIABLE_DECLS_BEFORE_FUNS>()));
                }
                switchToConstructorFunction();
                auto lowered = Lower(s, scope).result();
                if (!lowered) {
                    return std::unexpected(lowered.error());
                }
            } else if (NAst::TMaybeNode<NAst::TGlobalCleanupExpr>(s)) {
                if (destructorFunctionId != -1) {
                    return std::unexpected(TError(
                        s->Location,
                        "Only one global cleanup node is allowed."));
                }
                destructorFunctionId = Builder.NewFunction(
                    destructorFunctionName,
                    {},
                    -2 /*symbolId*/);
                Builder.SetReturnType(Module.Types.I(EKind::Void));
                auto lowered = Lower(s, TBlockScope{
                    .FuncIdx = destructorFunctionId,
                    .Id = scope.Id,
                }).result();
                if (!lowered) {
                    return std::unexpected(lowered.error());
                }
                emitter.Emit0("ret"_op, {});
                Module.ModuleDestructorFunctionId = destructorFunctionId;
            } else if (NAst::TMaybeNode<NAst::TUseExpr>(s)) {
                // Imports are handled during parsing/name resolution; keep the AST node
                // for source/core printing but do not lower it to IR.
            } else if (NAst::TMaybeNode<NAst::TTypeDeclStmt>(s)) {
                // Type declarations are registered during name resolution; nothing to lower.
            } else {
                return std::unexpected(TError(s->Location, TErrorString::Get<EErrorId::UNEXPECTED_TOP_LEVEL_STATEMENT>(s->ToString())));
            }
        }

        return {};
    };

    if (auto res = lowerTopBlock(block, scope); !res) {
        return res;
    }
    if (constructorFunctionId != -1) {
        Builder.SetCurrentFunction(constructorFunctionId);
        emitter.Emit0("ret"_op, {});
        Module.ModuleConstructorFunctionId = constructorFunctionId;
    }

    NPasses::LowerDebugAsync(Module);
    return {};
}


} // namespace NIR
} // namespace NQumir
