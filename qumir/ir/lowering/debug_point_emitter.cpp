#include "debug_point_emitter.h"

#include <limits>

namespace NQumir {
namespace NIR {

using namespace NLiterals;

TDebugPointEmitter::TDebugPointEmitter(TModule& module, TBuilder& builder)
    : Module_(module)
    , Builder_(builder)
{}

void TDebugPointEmitter::Point(std::string_view kind, const TInstrDebugInfo& info, int localId, std::string_view op) {
    if (
        !Module_.DebugOptions.EmitDebugPoints
        || Module_.Functions[Builder_.CurrentFunctionIdx()].Name.starts_with("$$")
        || Module_.Functions[Builder_.CurrentFunctionIdx()].Name.starts_with("__qumir_debug_"))
    {
        return;
    }
    if (SymbolId_ < 0 && Module_.DebugPointSymbolId >= 0) {
        SymbolId_ = Module_.DebugPointSymbolId;
    }
    if (SymbolId_ < 0) {
        SymbolId_ = std::numeric_limits<int>::max();
        Module_.SymIdToExtFuncIdx[SymbolId_] = Module_.ExternalFunctions.size();
        Module_.ExternalFunctions.push_back({
            .Name = "__qumir_debug_point",
            .MangledName = "__qumir_debug_point",
            .ArgTypes = {Module_.Types.I(EKind::I32), Module_.Types.Ptr(Module_.Types.I(EKind::Void))},
            .ReturnTypeId = Module_.Types.I(EKind::Void),
            .SymId = SymbolId_,
        });
    }
    const int id = Module_.DebugPoints.size();
    Module_.DebugPoints.push_back({std::string(kind), Builder_.CurrentFunctionIdx(), info, localId, std::string(op)});
    TOperand address = TImm{0, Module_.Types.I(EKind::I32)};
    if (localId >= 0) {
        auto ptr = Builder_.Emit1("lea"_op, {TLocal{localId}});
        Builder_.SetType(ptr, Module_.Types.Ptr(Module_.Functions[Builder_.CurrentFunctionIdx()].LocalTypes[localId]));
        address = ptr;
    }
    // A debug call must precede the entire argument packet of the source call.
    Builder_.Emit0("arg"_op, {TImm{id, Module_.Types.I(EKind::I32)}});
    Builder_.Emit0("arg"_op, {address});
    Builder_.Emit0("call"_op, {TImm{SymbolId_}});
}

void TDebugPointEmitter::Before(TOp op, const TInstrDebugInfo& info) {
    if (!Module_.DebugOptions.EmitDebugPoints || op == "phi"_op) {
        return;
    }
    if (op == "ret"_op) {
        Point("leave", info);
    } else if (!PendingArgs_) {
        Point("instruction", info, -1, op.ToString());
    }
    if (op == "arg"_op) {
        PendingArgs_ = true;
    }
}

void TDebugPointEmitter::After(TOp op, const TInstrDebugInfo& info) {
    if (op == "call"_op) {
        PendingArgs_ = false;
        Point("instruction", info, -1, "call");
    }
}

void TDebugPointEmitter::Declare(TLocal local, const TLocalVarDebugInfo& info) {
    if (!info.Name.empty() && info.Name.front() != '$') {
        Point("variable", {info.Location, info.ScopeId}, local.Idx);
    }
}

void TDebugPointEmitter::Bind(const TLocation& location, int localId, std::string_view name) {
    if (Module_.DebugOptions.EmitDebugPoints && localId >= 0) {
        Module_.DebugBindings.push_back({Builder_.CurrentFunctionIdx(), location, localId, std::string(name)});
    }
}

} // namespace NIR
} // namespace NQumir
