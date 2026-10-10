#include "type_annotation.h"
#include "coercions.h"

#include <qumir/optional.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/parser.h>
#include <qumir/parser/lexer.h>
#include <qumir/parser/type.h>
#include <qumir/semantics/name_resolution/name_resolver.h>
#include <qumir/semantics/return_analysis.h>
#include <qumir/semantics/vector_intrinsics.h>

#include <algorithm>
#include <bit>
#include <iostream>
#include <sstream>
#include <cassert>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace NQumir {
namespace NTypeAnnotation {

using namespace NAst;

namespace {

struct TGenericParamSets {
    std::unordered_set<std::string> Types;
    std::unordered_set<std::string> Values;
    std::map<std::string, TTypePtr> ValueTypes;
};

struct TGenericBindings {
    std::map<std::string, TTypePtr> Types;
    std::map<std::string, std::string> Values;
};

using TTask = TExpectedTask<TExprPtr, TError, TLocation>;
using TFunDeclTask = TExpectedTask<std::shared_ptr<TFunDecl>, TError, TLocation>;
using TGenericBindingsTask = TExpectedTask<TGenericBindings, TError, TLocation>;
using TGenericBoolTask = TExpectedTask<bool, TError, TLocation>;
using TRegisteredOpTask = TExpectedTask<std::optional<NSemantics::TNameResolver::TRegisteredOp>, TError, TLocation>;

TTask DoAnnotate(TExprPtr expr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId);

bool IsValidVectorSize(size_t size) {
    return size >= 2 && size <= 32 && (size & (size - 1)) == 0;
}

bool IsWideIntegerType(const TTypePtr& type) {
    auto integer = TMaybeType<TIntegerType>(UnwrapNamedType(type)).Cast();
    return integer && integer->BitWidth() > 64;
}

std::optional<TError> ValidateVectorTypes(const TTypePtr& type, const TLocation& location) {
    std::unordered_set<const TType*> checked;
    auto validate = [&](auto& self, const TTypePtr& current) -> std::optional<TError> {
        if (!current || !checked.insert(current.get()).second) {
            return {};
        }
        if (auto vector = TMaybeType<TVectorType>(current)) {
            if (vector.Cast()->SizeParam.empty() && !IsValidVectorSize(vector.Cast()->Size)) {
                return TError(location, "Размер вектора должен быть 2, 4, 8, 16 или 32.");
            }
            auto element = UnwrapNamedType(vector.Cast()->ElementType);
            if (element && !TMaybeType<TNamedType>(element) && !TMaybeType<TIntegerType>(element)
                && !TMaybeType<TFloatType>(element) && !TMaybeType<TBoolType>(element))
            {
                return TError(location, "Тип элемента вектора должен быть числом или bool.");
            }
            if (IsWideIntegerType(vector.Cast()->ElementType)) {
                return TError(location, "Целочисленный элемент вектора должен быть не шире 64 бит.");
            }
            return self(self, vector.Cast()->ElementType);
        }
        if (auto named = TMaybeType<TNamedType>(current)) {
            for (const auto& arg : named.Cast()->TypeArgs) {
                if (auto error = self(self, arg.Type)) {
                    return error;
                }
            }
            return self(self, named.Cast()->UnderlyingType);
        }
        if (auto reference = TMaybeType<TReferenceType>(current)) {
            return self(self, reference.Cast()->ReferencedType);
        }
        if (auto pointer = TMaybeType<TPointerType>(current)) {
            return self(self, pointer.Cast()->PointeeType);
        }
        if (auto array = TMaybeType<TArrayType>(current)) {
            return self(self, array.Cast()->ElementType);
        }
        if (auto future = TMaybeType<TFutureType>(current)) {
            return self(self, future.Cast()->ResultType);
        }
        if (auto tensor = TMaybeType<TTensorType>(current)) {
            return self(self, tensor.Cast()->ElementType);
        }
        if (auto function = TMaybeType<TFunctionType>(current)) {
            for (const auto& param : function.Cast()->ParamTypes) {
                if (auto error = self(self, param)) {
                    return error;
                }
            }
            return self(self, function.Cast()->ReturnType);
        }
        if (auto structure = TMaybeType<TStructType>(current)) {
            for (const auto& [name, field] : structure.Cast()->Fields) {
                if (auto error = self(self, field)) {
                    return error;
                }
            }
        }
        return {};
    };
    return validate(validate, type);
}

TTask AnnotateIdent(
    std::shared_ptr<TIdentExpr> ident,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId,
    bool pathThrough);
TTask AnnotateStructConstruct(
    std::shared_ptr<TStructConstructExpr> structure,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId);
TFunDeclTask InstantiateGenericFunction(
    std::shared_ptr<TFunDecl> genericDecl,
    const std::vector<TExprPtr>& args,
    NSemantics::TNameResolver& context,
    TLocation callLoc,
    TTypePtr expectedReturnType = nullptr);
TRegisteredOpTask InstantiateGenericOperator(
    const std::string& op,
    const std::vector<TExprPtr>& args,
    TTypePtr expectedReturnType,
    NSemantics::TNameResolver& context,
    TLocation callLoc);
TTask TryGenericBinaryOp(
    TExprPtr left,
    TExprPtr right,
    const TOperator& op,
    NSemantics::TNameResolver& ctx);
TTask TryGenericUnaryOp(
    const std::string& op,
    TExprPtr operand,
    NSemantics::TNameResolver& ctx,
    TLocation loc);

// Defined further below, needed already in AnnotateFunDecl to skip
// body-annotation for generic declarations.
bool HasGenericParams(const TFunDecl& decl);

std::string FormatCallSignature(const std::string& name, const std::vector<TExprPtr>& args) {
    std::ostringstream out;
    out << name << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << TypeDiagnosticName(UnwrapReferenceType(args[i]->Type));
    }
    out << ")";
    return out.str();
}

bool WideningIntOK(TTypePtr typeSrc, TTypePtr typeDst) {
    auto src = TMaybeType<TIntegerType>(typeSrc).Cast();
    auto dst = TMaybeType<TIntegerType>(typeDst).Cast();
    if (!src || !dst) {
        return false;
    }

    int wSrc = src->BitWidth();
    int wDst = dst->BitWidth();
    bool signedSrc = src->IsSigned();
    bool signedDst = dst->IsSigned();

    if (signedSrc == signedDst) {
        return wDst >= wSrc;
    }

    if (!signedSrc && signedDst) {
        // unsigned -> signed
        return wDst > wSrc; // u8->i16, u16->i32
    }

    if (signedSrc && !signedDst) {
        // signed -> unsigned: forbidden
        return false;
    }

    return false;
}

bool IntegerLiteralFits(int64_t value, const TIntegerType& type) {
    if (!type.IsSigned() && value < 0) {
        return false;
    }

    switch (type.Kind) {
        case TIntegerType::I8:
            return value >= std::numeric_limits<int8_t>::min()
                && value <= std::numeric_limits<int8_t>::max();
        case TIntegerType::I16:
            return value >= std::numeric_limits<int16_t>::min()
                && value <= std::numeric_limits<int16_t>::max();
        case TIntegerType::I32:
            return value >= std::numeric_limits<int32_t>::min()
                && value <= std::numeric_limits<int32_t>::max();
        case TIntegerType::I64:
        case TIntegerType::I128:
            return true;
        case TIntegerType::U8:
            return static_cast<uint64_t>(value) <= std::numeric_limits<uint8_t>::max();
        case TIntegerType::U16:
            return static_cast<uint64_t>(value) <= std::numeric_limits<uint16_t>::max();
        case TIntegerType::U32:
            return static_cast<uint64_t>(value) <= std::numeric_limits<uint32_t>::max();
        case TIntegerType::U64:
        case TIntegerType::U128:
            return true;
    }
    return false;
}

bool RetypeIntegerLiteralIfFits(TExprPtr expr, TTypePtr toType) {
    if (!expr || !toType) {
        return false;
    }
    auto maybeNumber = TMaybeNode<TNumberExpr>(expr);
    if (!maybeNumber) {
        return false;
    }

    auto number = maybeNumber.Cast();
    if (number->IsFloat()) {
        return false;
    }
    if (!TMaybeType<TIntegerType>(number->Type)) {
        return false;
    }
    if (TMaybeType<TFloatType>(toType)) {
        number->FloatValue = static_cast<double>(number->IntValue);
        number->Type = std::move(toType);
        return true;
    }
    auto maybeInteger = TMaybeType<TIntegerType>(toType);
    if (!maybeInteger) {
        return false;
    }
    if (!IntegerLiteralFits(number->IntValue, *maybeInteger.Cast())) {
        return false;
    }

    number->Type = std::move(toType);
    return true;
}

bool IsIntegerLiteral(TExprPtr expr) {
    auto maybeNumber = TMaybeNode<TNumberExpr>(expr);
    return maybeNumber
        && !maybeNumber.Cast()->IsFloat()
        && TMaybeType<TIntegerType>(maybeNumber.Cast()->Type);
}

bool IntegerLiteralCanUseType(TExprPtr expr, TTypePtr toType) {
    if (!expr || !toType) {
        return false;
    }
    auto maybeNumber = TMaybeNode<TNumberExpr>(expr);
    if (maybeNumber && TMaybeType<TFloatType>(toType)) {
        return !maybeNumber.Cast()->IsFloat()
            && TMaybeType<TIntegerType>(maybeNumber.Cast()->Type);
    }
    auto maybeInteger = TMaybeType<TIntegerType>(toType);
    return maybeNumber
        && maybeInteger
        && !maybeNumber.Cast()->IsFloat()
        && TMaybeType<TIntegerType>(maybeNumber.Cast()->Type)
        && IntegerLiteralFits(maybeNumber.Cast()->IntValue, *maybeInteger.Cast());
}

bool EqualTypes(TTypePtr a, TTypePtr b) {
    if (!a || !b) {
        return a == b;
    }
    if (a->TypeName() != b->TypeName()) {
        return false;
    }

    auto maybeAInt = TMaybeType<TIntegerType>(a);
    auto maybeBInt = TMaybeType<TIntegerType>(b);
    if (maybeAInt && maybeBInt) {
        return maybeAInt.Cast()->Kind == maybeBInt.Cast()->Kind;
    }

    auto maybeAFloat = TMaybeType<TFloatType>(a);
    auto maybeBFloat = TMaybeType<TFloatType>(b);
    if (maybeAFloat && maybeBFloat) {
        // we hame 1 float type for now
        return true;
    }

    auto maybeANamed = TMaybeType<TNamedType>(a);
    auto maybeBNamed = TMaybeType<TNamedType>(b);
    if (maybeANamed && maybeBNamed) {
        return TypeKey(a) == TypeKey(b);
    }

    auto maybeAFuture = TMaybeType<TFutureType>(a);
    auto maybeBFuture = TMaybeType<TFutureType>(b);
    if (maybeAFuture && maybeBFuture) {
        return EqualTypes(maybeAFuture.Cast()->ResultType, maybeBFuture.Cast()->ResultType);
    }

    if (auto vector = TMaybeType<TVectorType>(a)) {
        auto other = TMaybeType<TVectorType>(b).Cast();
        return vector.Cast()->Size == other->Size
            && vector.Cast()->SizeParam == other->SizeParam
            && EqualTypes(vector.Cast()->ElementType, other->ElementType);
    }

    return true;
}

bool CanImplicit(TTypePtr S, TTypePtr D, NSemantics::TNameResolver* ctx) {
    if (EqualTypes(S, D)) {
        return true;
    }

    if (TMaybeType<TIntegerType>(S) && TMaybeType<TIntegerType>(D)) {
        return WideningIntOK(S, D);
    }
    if (TMaybeType<TIntegerType>(S) && TMaybeType<TFloatType>(D)) {
        return true;
    }
    if (TMaybeType<TFloatType>(S) && TMaybeType<TIntegerType>(D)) {
        return true; // float to int conversion allowed
    }
    if ((TMaybeType<TFloatType>(S) || TMaybeType<TIntegerType>(S)) && TMaybeType<TBoolType>(D)) {
        return true;
    }
    if (TMaybeType<TSymbolType>(S) && TMaybeType<TStringType>(D)) {
        return true; // symbol to string conversion allowed
    }

    auto sourceVector = TMaybeType<TVectorType>(S).Cast();
    auto targetVector = TMaybeType<TVectorType>(D).Cast();
    if (
        sourceVector && targetVector && sourceVector->Size == targetVector->Size
        && CanImplicit(sourceVector->ElementType, targetVector->ElementType, ctx))
    {
        return true;
    }

    auto maybePointerS = TMaybeType<TPointerType>(S);
    auto maybePointerD = TMaybeType<TPointerType>(D);

    if (maybePointerS && maybePointerD) {
        if (TMaybeType<TVoidType>(maybePointerD.Cast()->PointeeType)) {
            return true; // T* -> void*
        }
        if (TMaybeType<TVoidType>(maybePointerS.Cast()->PointeeType)) {
            return false; // void* -> T* (requires cast)
        }
        return EqualTypes(
            maybePointerD.Cast()->PointeeType,
            maybePointerS.Cast()->PointeeType);
    }

    if (ctx->GetCast(S, D)) return true;
    return false;
}

// Returns the inline replacement if the function has an InlineFactory, nullptr otherwise.
// The replacement has no Type set - callers that are coroutines must DoAnnotate it.
TExprPtr TryApplyInlineFactory(const std::shared_ptr<TFunDecl>& funDecl, std::vector<TExprPtr> args) {
    if (funDecl->InlineFactory) {
        return (*funDecl->InlineFactory)(std::move(args));
    }
    return nullptr;
}

// Looks up a function by name in the given scope, then applies its InlineFactory if present.
TExprPtr TryInlineByName(const std::string& name, std::vector<TExprPtr> args,
                         NSemantics::TNameResolver& ctx, NSemantics::TScopeId scopeId) {
    auto symId = ctx.Lookup(name, scopeId);
    if (!symId) return nullptr;
    auto node = ctx.GetSymbolNode(NSemantics::TSymbolId{symId->Id});
    if (auto maybeFunDecl = TMaybeNode<TFunDecl>(node)) {
        return TryApplyInlineFactory(maybeFunDecl.Cast(), std::move(args));
    }
    return nullptr;
}

TExprPtr InsertImplicitCastIfNeeded(TExprPtr expr, TTypePtr toType, NSemantics::TNameResolver* ctx) {
    if (!expr->Type || !toType) {
        return expr;
    }
    if (EqualTypes(expr->Type, toType)) {
        return expr;
    }
    if (!CanImplicit(expr->Type, toType, ctx)) {
        return expr;
    }

    if (auto maybeNumber = TMaybeNode<TNumberExpr>(expr)) {
        auto num = maybeNumber.Cast();
        if (TMaybeType<TIntegerType>(toType)) {
            if (num->IsFloat()) {
                // float to int
                int64_t intVal = static_cast<int64_t>(num->FloatValue);
                auto newNum = std::make_shared<TNumberExpr>(num->Location, intVal);
                newNum->Type = toType;
                return newNum;
            } else {
                // int to int (widening)
                num->Type = toType;
                return num;
            }
        }
        if (TMaybeType<TFloatType>(toType)) {
            if (num->IsFloat()) {
                // float to float (widening)
                num->Type = toType;
                return num;
            } else {
                // int to float
                double floatVal = static_cast<double>(num->IntValue);
                auto newNum = std::make_shared<TNumberExpr>(num->Location, floatVal);
                newNum->Type = toType;
                return newNum;
            }
        }
    }

    if (auto synthName = ctx->GetCast(expr->Type, toType)) {
        auto callee = std::make_shared<TIdentExpr>(expr->Location, *synthName);
        callee->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{expr->Type}, toType);
        auto call = std::make_shared<TCallExpr>(expr->Location, callee,
            std::vector<TExprPtr>{expr});
        call->Type = toType;
        return call;
    }

    return MakeCast(std::move(expr), std::move(toType));
}

// Annotates expr if its Type is not yet set (i.e. came from an inline factory replacement).
TTask AnnotateIfNeeded(TExprPtr expr, NSemantics::TNameResolver& ctx, NSemantics::TScopeId scopeId) {
    if (expr->Type) co_return expr;
    co_return co_await DoAnnotate(expr, ctx, scopeId);
}

TExprPtr MakeModuleOpCall(const std::string& synthName, std::vector<TExprPtr> args,
    TTypePtr returnType, TLocation loc, NSemantics::TNameResolver& ctx)
{
    auto callee = std::make_shared<TIdentExpr>(loc, synthName);
    std::vector<TTypePtr> argTypes;
    for (auto& a : args) argTypes.push_back(a->Type);
    callee->Type = std::make_shared<TFunctionType>(std::move(argTypes), returnType);
    auto call = std::make_shared<TCallExpr>(loc, callee, std::move(args));
    call->Type = returnType;
    return call;
}

TTypePtr CommonNumericType(TTypePtr a, TTypePtr b) {
    if (TMaybeType<TFloatType>(a) && TMaybeType<TFloatType>(b)) {
        return a;
    }
    if (TMaybeType<TFloatType>(a) && TMaybeType<TIntegerType>(b)) {
        return a;
    }
    if (TMaybeType<TIntegerType>(a) && TMaybeType<TFloatType>(b)) {
        return b;
    }
    if (TMaybeType<TIntegerType>(a) && TMaybeType<TIntegerType>(b)) {
        if (WideningIntOK(a, b)) {
            return b;
        }
        if (WideningIntOK(b, a)) {
            return a;
        }
        return {};
    }
    return {};
}

TTypePtr CommonValueType(TTypePtr a, TTypePtr b, NSemantics::TNameResolver& ctx) {
    if (EqualTypes(a, b)) {
        return a;
    }

    if (auto numeric = CommonNumericType(a, b)) {
        return numeric;
    }

    if (CanImplicit(a, b, &ctx)) {
        return b;
    }

    if (CanImplicit(b, a, &ctx)) {
        return a;
    }

    return {};
}

TExprPtr AnnotateNumber(std::shared_ptr<TNumberExpr> num) {
    if (num->Type) {
        return num;
    }
    if (num->IsFloat()) {
        num->Type = std::make_shared<TFloatType>();
    } else {
        num->Type = std::make_shared<TIntegerType>();
    }
    return num;
}

std::string AvailableStructFields(const TStructType& structure) {
    std::string available;
    for (const auto& [name, _] : structure.Fields) {
        if (!available.empty()) {
            available += ", ";
        }
        available += name;
    }
    return available;
}

std::string FormatStructFieldForDiagnostic(const std::pair<std::string, TTypePtr>& field) {
    return "'" + field.first + ": " + TypeDiagnosticName(field.second) + "'";
}

std::string FormatStructCastMismatch(const TStructType& source, const TStructType& target) {
    std::vector<std::string> details;
    const auto count = std::max(source.Fields.size(), target.Fields.size());
    for (size_t i = 0; i < count; ++i) {
        const auto index = std::to_string(i + 1);
        if (i >= source.Fields.size()) {
            details.push_back("поле #" + index + ": ожидается "
                + FormatStructFieldForDiagnostic(target.Fields[i])
                + ", но в исходной структуре поля нет");
            continue;
        }
        if (i >= target.Fields.size()) {
            details.push_back("поле #" + index + ": лишнее исходное поле "
                + FormatStructFieldForDiagnostic(source.Fields[i]));
            continue;
        }

        const auto& sourceField = source.Fields[i];
        const auto& targetField = target.Fields[i];
        if (sourceField.first != targetField.first) {
            details.push_back("поле #" + index + ": ожидается "
                + FormatStructFieldForDiagnostic(targetField)
                + ", получено "
                + FormatStructFieldForDiagnostic(sourceField));
            continue;
        }
        if (TypeKey(sourceField.second) != TypeKey(targetField.second)) {
            details.push_back("поле #" + index + " '" + sourceField.first
                + "': ожидается " + TypeDiagnosticName(targetField.second)
                + ", получено " + TypeDiagnosticName(sourceField.second));
        }
    }

    if (details.empty()) {
        return "исходная структура: " + TypeDiagnosticName(std::make_shared<TStructType>(source))
            + ", целевая структура: " + TypeDiagnosticName(std::make_shared<TStructType>(target));
    }

    std::string result;
    for (size_t i = 0; i < details.size(); ++i) {
        if (i != 0) {
            result += "; ";
        }
        result += details[i];
    }
    return result;
}

bool HasConcreteStructFieldTypes(const TStructType& structure) {
    for (const auto& [_, fieldType] : structure.Fields) {
        if (!fieldType) {
            return false;
        }
    }
    return true;
}

bool ApplyStructConstructContext(TExprPtr expr, TTypePtr targetType) {
    if (!TMaybeType<TStructType>(UnwrapNamedType(targetType))) {
        return false;
    }
    if (auto structure = TMaybeNode<TStructConstructExpr>(expr)) {
        structure.Cast()->Type = targetType;
        return true;
    }
    if (auto ifExpr = TMaybeNode<TIfExpr>(expr)) {
        bool applied = false;
        if (ifExpr.Cast()->Then) {
            applied = ApplyStructConstructContext(ifExpr.Cast()->Then, targetType) || applied;
        }
        if (ifExpr.Cast()->Else) {
            applied = ApplyStructConstructContext(ifExpr.Cast()->Else, targetType) || applied;
        }
        return applied;
    }
    return false;
}

void BackfillStructFieldTypes(const std::shared_ptr<TStructConstructExpr>& structure) {
    auto structType = TMaybeType<TStructType>(UnwrapNamedType(structure->Type)).Cast();
    if (!structType) {
        return;
    }
    for (size_t i = 0; i < structure->Fields.size() && i < structType->Fields.size(); ++i) {
        structType->Fields[i].second = structure->Fields[i]->Type;
    }
}

TTask AnnotateStructConstruct(std::shared_ptr<TStructConstructExpr> structure, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    auto targetStruct = TMaybeType<TStructType>(UnwrapNamedType(UnwrapReferenceType(structure->Type))).Cast();
    if (!targetStruct || !HasConcreteStructFieldTypes(*targetStruct)) {
        for (auto& field : structure->Fields) {
            field = co_await DoAnnotate(field, context, scopeId);
        }
        BackfillStructFieldTypes(structure);
        co_return structure;
    }

    std::unordered_map<std::string, size_t> fieldByName;
    fieldByName.reserve(targetStruct->Fields.size());
    for (size_t i = 0; i < targetStruct->Fields.size(); ++i) {
        fieldByName.emplace(targetStruct->Fields[i].first, i);
    }

    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < structure->Fields.size(); ++i) {
        std::string fieldName;
        if (i < structure->FieldNames.size()) {
            fieldName = structure->FieldNames[i];
        } else if (i < targetStruct->Fields.size()) {
            fieldName = targetStruct->Fields[i].first;
        } else {
            co_return TError(structure->Location, "Слишком много полей при конструировании структуры.");
        }

        auto targetIt = fieldByName.find(fieldName);
        if (targetIt == fieldByName.end()) {
            co_return TError(structure->Location,
                "Поле '" + fieldName + "' не найдено в структуре. Доступные поля: " + AvailableStructFields(*targetStruct) + ".");
        }
        if (!seen.insert(fieldName).second) {
            co_return TError(structure->Location,
                "Поле '" + fieldName + "' указано несколько раз при конструировании структуры.");
        }

        auto fieldType = targetStruct->Fields[targetIt->second].second;
        auto& field = structure->Fields[i];
        field = co_await DoAnnotate(field, context, scopeId);
        if (!field->Type) {
            co_return TError(field->Location,
                "Не удалось определить тип поля '" + fieldName + "' при конструировании структуры.");
        }

        auto valueType = UnwrapReferenceType(field->Type);
        if (!EqualTypes(valueType, fieldType)) {
            if (!CanImplicit(valueType, fieldType, &context)) {
                co_return TError(field->Location,
                    "Нельзя неявно преобразовать тип '" + TypeDiagnosticName(valueType) +
                    "' к типу поля '" + fieldName + "' (" + TypeDiagnosticName(fieldType) + ").");
            }
            field = InsertImplicitCastIfNeeded(field, fieldType, &context);
        }
    }

    co_return structure;
}

TTask AnnotateCast(std::shared_ptr<TCastExpr> cast, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    bool contextualStructConstruct = ApplyStructConstructContext(cast->Operand, cast->Type);
    cast->Operand = co_await DoAnnotate(cast->Operand, context, scopeId);
    if (cast->Operand->Type) {
        auto sourceType = UnwrapReferenceType(cast->Operand->Type);
        auto targetType = UnwrapReferenceType(cast->Type);
        if (TypeKey(sourceType) == TypeKey(targetType)) {
            co_return cast;
        }
        auto sourceStructType = UnwrapNamedType(sourceType);
        auto targetStructType = UnwrapNamedType(targetType);
        if (auto synthName = context.GetCast(cast->Operand->Type, cast->Type)) {
            auto callee = std::make_shared<TIdentExpr>(cast->Location, *synthName);
            callee->Type = std::make_shared<TFunctionType>(
                std::vector<TTypePtr>{cast->Operand->Type}, cast->Type);
            auto call = std::make_shared<TCallExpr>(cast->Location, callee,
                std::vector<TExprPtr>{cast->Operand});
            call->Type = cast->Type;
            co_return call;
        }
        if (auto op = co_await InstantiateGenericOperator("cast", {cast->Operand}, cast->Type, context, cast->Location)) {
            co_return MakeModuleOpCall(op->SynthName, {cast->Operand}, op->ReturnType, cast->Location, context);
        }
        auto sourceVector = TMaybeType<TVectorType>(sourceStructType).Cast();
        auto targetVector = TMaybeType<TVectorType>(targetStructType).Cast();
        if ((sourceVector || targetVector)
            && (!sourceVector || !targetVector || sourceVector->Size != targetVector->Size))
        {
            co_return TError(cast->Location, "Приведение векторов требует одинакового числа элементов.");
        }
        if (!contextualStructConstruct
            && TMaybeType<TStructType>(sourceStructType)
            && TMaybeType<TStructType>(targetStructType)
            && TypeKey(sourceStructType) != TypeKey(targetStructType))
        {
            auto sourceStruct = TMaybeType<TStructType>(sourceStructType).Cast();
            auto targetStruct = TMaybeType<TStructType>(targetStructType).Cast();
            co_return TError(cast->Location,
                "Нельзя привести структуру к целевому типу: поля структуры не совпадают: "
                + FormatStructCastMismatch(*sourceStruct, *targetStruct) + ".");
        }
    }
    co_return cast;
}

TTask AnnotateUnary(std::shared_ptr<TUnaryExpr> unary, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    unary->Operand = co_await DoAnnotate(unary->Operand, context, scopeId);
    unary->Type = unary->Operand->Type;
    if (auto vector = TMaybeType<TVectorType>(UnwrapNamedType(UnwrapReferenceType(unary->Type))).Cast()) {
        auto element = UnwrapNamedType(vector->ElementType);
        if (unary->Operator == TOperator("!")
            && (TMaybeType<TIntegerType>(element) || TMaybeType<TFloatType>(element) || TMaybeType<TBoolType>(element)))
        {
            unary->Type = std::make_shared<TVectorType>(std::make_shared<TBoolType>(), vector->Size);
            co_return unary;
        }
        if (unary->Operator == TOperator("~") && TMaybeType<TIntegerType>(element)) {
            unary->Type = UnwrapReferenceType(unary->Type);
            co_return unary;
        }
    }
    if (unary->Operator == '-') {
        auto maybeInt = TMaybeType<TIntegerType>(unary->Type);
        if (maybeInt) {
            auto intType = maybeInt.Cast();
            if (false /*!intType->IsSigned()*/) {
                co_return TError(unary->Location, "Нельзя применять унарный минус к беззнаковому целому типу");
            }
            co_return unary;
        }
        auto maybeFloat = TMaybeType<TFloatType>(unary->Type);
        if (maybeFloat) {
            co_return unary;
        }
        if (auto m = context.GetUnaryOp("neg", UnwrapReferenceType(unary->Type))) {
            co_return co_await AnnotateIfNeeded(MakeModuleOpCall(m->SynthName, {unary->Operand}, m->ReturnType, unary->Location, context), context, scopeId);
        }
        if (auto result = co_await TryGenericUnaryOp("neg", unary->Operand, context, unary->Location)) {
            co_return co_await AnnotateIfNeeded(result, context, scopeId);
        }
        co_return TError(unary->Location, "Нельзя применять унарный минус к нечисловому типу");
    }
    if (unary->Operator == TOperator("!")) {
        auto maybeBool = TMaybeType<TBoolType>(unary->Type);
        if (maybeBool) {
            unary->Type = std::make_shared<TBoolType>();
            co_return unary;
        }
        auto maybeInt = TMaybeType<TIntegerType>(unary->Type);
        if (maybeInt) {
            unary->Type = std::make_shared<TBoolType>();
            co_return unary;
        }
        auto maybeFloat = TMaybeType<TFloatType>(unary->Type);
        if (maybeFloat) {
            unary->Type = std::make_shared<TBoolType>();
            co_return unary;
        }
        if (auto m = context.GetUnaryOp("!", UnwrapReferenceType(unary->Type))) {
            co_return co_await AnnotateIfNeeded(MakeModuleOpCall(m->SynthName, {unary->Operand}, m->ReturnType, unary->Location, context), context, scopeId);
        }
        if (auto result = co_await TryGenericUnaryOp("!", unary->Operand, context, unary->Location)) {
            co_return co_await AnnotateIfNeeded(result, context, scopeId);
        }
        co_return TError(unary->Location, "Оператор отрицания (не) применяется только к логическим выражениям");
    }
    if (unary->Operator == TOperator("~")) {
        if (TMaybeType<TIntegerType>(unary->Type)) {
            co_return unary; // keeps the operand's width and signedness
        }
        if (auto m = context.GetUnaryOp("~", UnwrapReferenceType(unary->Type))) {
            co_return co_await AnnotateIfNeeded(MakeModuleOpCall(m->SynthName, {unary->Operand}, m->ReturnType, unary->Location, context), context, scopeId);
        }
        if (auto result = co_await TryGenericUnaryOp("~", unary->Operand, context, unary->Location)) {
            co_return co_await AnnotateIfNeeded(result, context, scopeId);
        }
        co_return TError(unary->Location, "Битовое отрицание применяется только к целым числам");
    }
    co_return unary;
}

TTask AnnotateFunDecl(std::shared_ptr<TFunDecl> funDecl, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    std::vector<TTypePtr> params;
    for (auto& p : funDecl->Params) {
        co_await DoAnnotate(p, context, NSemantics::TScopeId{funDecl->Body->Scope});
        if (!p->Type) {
            co_return TError(p->Location, "Не указан тип параметра функции: " + p->Name);
        }
        params.push_back(p->Type);
    }
    if (!funDecl->RetType) {
        co_return TError(funDecl->Location, "Не указан возвращаемый тип функции");
    }

    funDecl->Type = std::make_shared<TFunctionType>(std::move(params), funDecl->RetType);

    // RetType as seen by `(return ...)` inside this function: for coroutines
    // this is the unwrapped Future<T> result type, otherwise funDecl->RetType
    // itself. Stored on the function-level scope so AnnotateReturn can find
    // it from any nested scope via TScope::FuncScope.
    auto funcScope = context.GetScope(NSemantics::TScopeId{funDecl->Scope});
    funcScope->RetType = FutureResultType(funDecl->RetType);
    if (!funcScope->RetType) {
        funcScope->RetType = funDecl->RetType;
    }

    if (funDecl->IsExternal()) {
        // External functions have no body to type-check; the body is ignored.
        co_return funDecl;
    }

    if (HasGenericParams(*funDecl)) {
        // Generic function: its body refers to
        // placeholder types and cannot be type-checked on its own - only
        // monomorphized clones (created at call sites by
        // InstantiateGenericFunction) get annotated.
        co_return funDecl;
    }

    co_await DoAnnotate(funDecl->Body, context, scopeId);
    if (!funDecl->Body->Type) {
        co_return TError(funDecl->Location, "Не удалось определить тип тела функции");
    }
    if (funDecl->LastAssert) {
        funDecl->LastAssert = co_await DoAnnotate(
            funDecl->LastAssert,
            context,
            NSemantics::TScopeId{funDecl->Body->Scope});
    }

    // A non-void body either yields an implicit return value or explicitly
    // returns on every path. Return expressions remain void-typed.
    if (!TMaybeType<TVoidType>(funcScope->RetType)) {
        auto bodyType = UnwrapReferenceType(funDecl->Body->Type);
        if (TMaybeType<TVoidType>(bodyType)) {
            if (!NSemantics::AlwaysReturns(funDecl->Body)) {
                co_return TError(funDecl->Location,
                    "Тело функции должно возвращать значение на каждом пути выполнения.");
            }
        } else {
            if (!EqualTypes(bodyType, funcScope->RetType)) {
                if (!CanImplicit(bodyType, funcScope->RetType, &context)) {
                    co_return TError(funDecl->Location,
                        "Тело функции должно возвращать значение типа '" + TypeDiagnosticName(funcScope->RetType) +
                        "', но имеет тип '" + TypeDiagnosticName(bodyType) + "'.");
                }
                funDecl->Body->Stmts.back() = InsertImplicitCastIfNeeded(funDecl->Body->Stmts.back(), funcScope->RetType, &context);
                funDecl->Body->Type = funcScope->RetType;
            }
        }
    }

    co_return funDecl;
}

TTask AnnotateReturn(std::shared_ptr<TReturnExpr> ret, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    auto scope = context.GetScope(scopeId);
    auto funcScope = scope->FuncScope;
    if (!funcScope || !funcScope->RetType) {
        co_return TError(ret->Location, "`return` вне функции.");
    }
    auto retType = funcScope->RetType;

    if (ret->Value) {
        ret->Value = co_await DoAnnotate(ret->Value, context, scopeId);
        if (!ret->Value->Type) {
            co_return TError(ret->Value->Location, "Не удалось определить тип возвращаемого значения.");
        }
        if (TMaybeType<TVoidType>(retType)) {
            co_return TError(ret->Location, "Нельзя вернуть значение из функции, возвращающей void.");
        }
        auto valueType = UnwrapReferenceType(ret->Value->Type);
        if (!EqualTypes(valueType, retType)) {
            if (!CanImplicit(valueType, retType, &context)) {
                co_return TError(ret->Location,
                    "Нельзя неявно преобразовать тип '" + TypeDiagnosticName(valueType) +
                    "' к возвращаемому типу функции '" + TypeDiagnosticName(retType) + "'.");
            }
            ret->Value = InsertImplicitCastIfNeeded(ret->Value, retType, &context);
        }
    } else {
        if (!TMaybeType<TVoidType>(retType)) {
            co_return TError(ret->Location,
                "Функция должна возвращать значение типа '" + TypeDiagnosticName(retType) + "', но `return` указан без значения.");
        }
    }

    ret->Type = std::make_shared<TVoidType>();
    co_return ret;
}

template<typename T>
TTask AnnotateLifetimeValue(
    std::shared_ptr<T> expr,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    expr->Value = co_await DoAnnotate(expr->Value, context, scopeId);
    if (!expr->Value->Type) {
        co_return TError(expr->Location, "Lifetime-выражение имеет значение без типа.");
    }
    expr->Type = expr->Value->Type;
    co_return expr;
}

TTask AnnotateDestroy(
    std::shared_ptr<TDestroyExpr> destroy,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    destroy->Value = co_await DoAnnotate(destroy->Value, context, scopeId);
    if (destroy->Aux) {
        destroy->Aux = co_await DoAnnotate(destroy->Aux, context, scopeId);
    }
    destroy->Type = std::make_shared<TVoidType>();
    co_return destroy;
}

TTask AnnotateReplace(
    std::shared_ptr<TReplaceExpr> replace,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    if (auto ident = TMaybeNode<TIdentExpr>(replace->Target)) {
        replace->Target = co_await AnnotateIdent(
            ident.Cast(),
            context,
            scopeId,
            true);
    } else {
        replace->Target = co_await DoAnnotate(replace->Target, context, scopeId);
    }
    replace->Value = co_await DoAnnotate(replace->Value, context, scopeId);
    auto targetType = UnwrapReferenceType(replace->Target->Type);
    auto valueType = UnwrapReferenceType(replace->Value->Type);
    if (!EqualTypes(valueType, targetType)) {
        if (!CanImplicit(valueType, targetType, &context)) {
            co_return TError(replace->Location,
                "Нельзя заменить значение типа '" + TypeDiagnosticName(targetType)
                + "' значением типа '" + TypeDiagnosticName(valueType) + "'.");
        }
        replace->Value = InsertImplicitCastIfNeeded(replace->Value, targetType, &context);
    }
    replace->Type = std::make_shared<TVoidType>();
    co_return replace;
}

TTask AnnotateCleanupExit(
    std::shared_ptr<TCleanupExitExpr> exit,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    if (exit->Kind == ECleanupExitKind::Return) {
        auto scope = context.GetScope(scopeId);
        auto funcScope = scope->FuncScope;
        if (!funcScope || !funcScope->RetType) {
            co_return TError(exit->Location, "`return` вне функции.");
        }
        const auto& returnType = funcScope->RetType;
        if (exit->Value) {
            exit->Value = co_await DoAnnotate(exit->Value, context, scopeId);
            if (TMaybeType<TVoidType>(returnType)) {
                co_return TError(exit->Location,
                    "Нельзя вернуть значение из функции, возвращающей void.");
            }
            auto valueType = UnwrapReferenceType(exit->Value->Type);
            if (!EqualTypes(valueType, returnType)) {
                if (!CanImplicit(valueType, returnType, &context)) {
                    co_return TError(exit->Location,
                        "Нельзя неявно преобразовать тип '" + TypeDiagnosticName(valueType)
                        + "' к возвращаемому типу функции '"
                        + TypeDiagnosticName(returnType) + "'.");
                }
                exit->Value = InsertImplicitCastIfNeeded(exit->Value, returnType, &context);
            }
        } else if (!TMaybeType<TVoidType>(returnType)) {
            co_return TError(exit->Location,
                "Функция должна возвращать значение типа '"
                + TypeDiagnosticName(returnType)
                + "', но `return` указан без значения.");
        }
    } else if (exit->Value) {
        co_return TError(exit->Location, "`break` и `continue` не могут иметь значение.");
    }

    for (auto& cleanup : exit->Cleanups) {
        cleanup = co_await DoAnnotate(cleanup, context, scopeId);
    }
    exit->Type = std::make_shared<TVoidType>();
    co_return exit;
}

TTask AnnotateGlobalCleanup(
    std::shared_ptr<TGlobalCleanupExpr> cleanup,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    for (auto& expr : cleanup->Cleanups) {
        expr = co_await DoAnnotate(expr, context, scopeId);
    }
    cleanup->Type = std::make_shared<TVoidType>();
    co_return cleanup;
}

// Tries exact match, then cast-left, then cast-right. Returns TCallExpr or nullptr.
TExprPtr TryModuleBinaryOp(TExprPtr left, TExprPtr right,
    const TOperator& op, NSemantics::TNameResolver& ctx)
{
    const std::string opStr = op.ToString();
    auto lt = UnwrapReferenceType(left->Type);
    auto rt = UnwrapReferenceType(right->Type);

    if (auto m = ctx.GetBinaryOp(opStr, lt, rt)) {
        return MakeModuleOpCall(m->SynthName, {left, right}, m->ReturnType, left->Location, ctx);
    }

    auto castedLeft = InsertImplicitCastIfNeeded(left, rt, &ctx);
    if (castedLeft != left) {
        if (auto m = ctx.GetBinaryOp(opStr, rt, rt)) {
            return MakeModuleOpCall(m->SynthName, {castedLeft, right}, m->ReturnType, left->Location, ctx);
        }
    }

    auto castedRight = InsertImplicitCastIfNeeded(right, lt, &ctx);
    if (castedRight != right) {
        if (auto m = ctx.GetBinaryOp(opStr, lt, lt)) {
            return MakeModuleOpCall(m->SynthName, {left, castedRight}, m->ReturnType, left->Location, ctx);
        }
    }

    return nullptr;
}

void RetypeIntegerLiteralOperands(TExprPtr& leftExpr, TExprPtr& rightExpr, TTypePtr& leftType, TTypePtr& rightType) {
    if (!TMaybeType<TIntegerType>(leftType) || !TMaybeType<TIntegerType>(rightType)) {
        return;
    }

    if (RetypeIntegerLiteralIfFits(rightExpr, leftType)) {
        rightType = UnwrapReferenceType(rightExpr->Type);
        return;
    }
    if (RetypeIntegerLiteralIfFits(leftExpr, rightType)) {
        leftType = UnwrapReferenceType(leftExpr->Type);
    }
}

struct TBinaryOpTypes {
    TTypePtr Left;
    TTypePtr Right;
    TTypePtr Result;
};

using TBinaryOpTypesResult = std::expected<TBinaryOpTypes, TError>;

bool IsNumericType(const TTypePtr& type) {
    return TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type);
}

TTask AnnotateVector(
    std::shared_ptr<TVectorExpr> vector,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    if (!IsValidVectorSize(vector->Elements.size())) {
        co_return TError(vector->Location, "Размер вектора должен быть 2, 4, 8, 16 или 32.");
    }
    auto target = TMaybeType<TVectorType>(UnwrapNamedType(UnwrapReferenceType(vector->Type))).Cast();
    if (vector->Type && !target) {
        co_return TError(vector->Location, "Конструктору вектора должен быть задан векторный тип.");
    }
    if (target && (target->Size <= 0 || static_cast<size_t>(target->Size) != vector->Elements.size())) {
        co_return TError(vector->Location, "Число элементов не совпадает с размером вектора.");
    }
    TTypePtr elementType = target
        ? target->ElementType
        : nullptr;
    auto isElementType = [](const TTypePtr& type) {
        return IsNumericType(type) || TMaybeType<TBoolType>(type);
    };
    if (target && !isElementType(elementType)) {
        co_return TError(vector->Location, "Тип элемента вектора должен быть числом или bool.");
    }
    for (auto& element : vector->Elements) {
        if (!element) {
            co_return TError(vector->Location, "Элемент вектора должен быть выражением.");
        }
        element = co_await DoAnnotate(element, context, scopeId);
        auto valueType = UnwrapReferenceType(element->Type);
        if (!isElementType(valueType)) {
            co_return TError(element->Location, "Элемент вектора должен быть числом или bool.");
        }
        if (!elementType) {
            elementType = valueType;
        }
        if (IsWideIntegerType(elementType)) {
            co_return TError(element->Location, "Целочисленный элемент вектора должен быть не шире 64 бит.");
        }
        if (target && RetypeIntegerLiteralIfFits(element, elementType)) {
            valueType = UnwrapReferenceType(element->Type);
        }
        if (!EqualTypes(valueType, elementType)) {
            if (!target || !CanImplicit(valueType, elementType, &context)) {
                co_return TError(element->Location,
                    "Тип элемента '" + TypeDiagnosticName(valueType)
                    + "' не соответствует типу элементов вектора '" + TypeDiagnosticName(elementType) + "'.");
            }
            element = TMaybeType<TReferenceType>(element->Type)
                ? MakeCast(element, elementType)
                : InsertImplicitCastIfNeeded(element, elementType, &context);
        }
    }
    if (!target) {
        vector->Type = std::make_shared<TVectorType>(elementType, static_cast<int>(vector->Elements.size()));
    }
    co_return vector;
}

bool IsStringType(const TTypePtr& type) {
    return TMaybeType<TStringType>(type) || TMaybeType<TSymbolType>(type);
}

TBinaryOpTypesResult AnnotateNumericBinaryOp(
    const std::shared_ptr<TBinaryExpr>& binary,
    TTypePtr left,
    TTypePtr right,
    NSemantics::TNameResolver& context)
{
    auto error = [&](std::string message) {
        return std::unexpected(TError(binary->Location, std::move(message)));
    };
    auto commonNumeric = [&](bool integerOnly, std::string message) -> TBinaryOpTypesResult {
        RetypeIntegerLiteralOperands(binary->Left, binary->Right, left, right);
        auto common = CommonNumericType(left, right);
        if (!common || (integerOnly && !TMaybeType<TIntegerType>(common))) {
            return error(std::move(message));
        }
        if (binary->Operator == TOperator("/")) {
            common = std::make_shared<TFloatType>();
        }
        return TBinaryOpTypes{common, common, common};
    };

    switch (binary->Operator) {
        case TOperator("+"):
        case TOperator("-"):
        case TOperator("*"):
        case TOperator("/"):
            return commonNumeric(false, "+, -, *, / применимы только к числам (оператор + также работает для строк)");
        case TOperator("//"):
        case TOperator("%"):
            return commonNumeric(true, "binary expression operands must be both integer types");
        case TOperator("**"):
            if (TMaybeType<TFloatType>(left) && IsNumericType(right)) {
                return TBinaryOpTypes{left, right, std::make_shared<TFloatType>()};
            }
            if (TMaybeType<TIntegerType>(left) && TMaybeType<TIntegerType>(right)) {
                return TBinaryOpTypes{left, right, std::make_shared<TIntegerType>()};
            }
            return error("binary expression operands must be numeric types (float ** float, float ** int, or int ** int)");
        case TOperator("&"):
        case TOperator("|"):
        case TOperator("^"):
        case TOperator("<<"):
        case TOperator(">>"):
            if (TMaybeType<TIntegerType>(left) && TMaybeType<TIntegerType>(right)) {
                // Lowering converts the RHS to the result width and selects signed shifts.
                return TBinaryOpTypes{left, right, left};
            }
            return error("Битовые операции применимы только к целым числам");
        case TOperator("<"):
        case TOperator("<="):
        case TOperator(">"):
        case TOperator(">="):
        case TOperator("=="):
        case TOperator("!="):
            if (IsNumericType(left) && IsNumericType(right)) {
                auto common = commonNumeric(false, "Операции сравнения применимы только к числам");
                if (!common) {
                    return common;
                }
                common->Result = std::make_shared<TBoolType>();
                return common;
            }
            if (TMaybeType<TBoolType>(left) && TMaybeType<TBoolType>(right)) {
                return TBinaryOpTypes{left, right, std::make_shared<TBoolType>()};
            }
            if (
                (binary->Operator == TOperator("==") || binary->Operator == TOperator("!="))
                && TMaybeType<TPointerType>(left) && TMaybeType<TPointerType>(right))
            {
                return TBinaryOpTypes{left, right, std::make_shared<TBoolType>()};
            }
            return error("Операции сравнения применимы только к совместимым типам");
        case TOperator("&&"):
        case TOperator("||"): {
            auto boolType = std::make_shared<TBoolType>();
            if (CanImplicit(left, boolType, &context) && CanImplicit(right, boolType, &context)) {
                return TBinaryOpTypes{boolType, boolType, boolType};
            }
            return error("Логические операции применимы только к логическим выражениям");
        }
        default:
            return error("Неизвестный бинарный оператор: '" + binary->Operator.ToString() + "'");
    }
}

TBinaryOpTypesResult AnnotateStringBinaryOp(
    const std::shared_ptr<TBinaryExpr>& binary,
    const TTypePtr& left,
    const TTypePtr& right)
{
    if (IsStringType(left) && IsStringType(right)) {
        auto stringType = std::make_shared<TStringType>();
        switch (binary->Operator) {
            case TOperator("+"):
                return TBinaryOpTypes{stringType, stringType, stringType};
            case TOperator("<"):
            case TOperator("<="):
            case TOperator(">"):
            case TOperator(">="):
            case TOperator("=="):
            case TOperator("!="):
                if (TMaybeType<TSymbolType>(left) && TMaybeType<TSymbolType>(right)) {
                    return TBinaryOpTypes{left, right, std::make_shared<TBoolType>()};
                }
                return TBinaryOpTypes{stringType, stringType, std::make_shared<TBoolType>()};
        }
    }
    return std::unexpected(TError(binary->Location,
        "Оператор '" + binary->Operator.ToString() + "' неприменим к типам "
        + TypeDiagnosticName(left) + " и " + TypeDiagnosticName(right)));
}

TBinaryOpTypesResult AnnotateVecBinaryOp(
    const std::shared_ptr<TBinaryExpr>& binary,
    const TTypePtr& left,
    const TTypePtr& right,
    NSemantics::TNameResolver& context)
{
    auto leftVector = TMaybeType<TVectorType>(left).Cast();
    auto rightVector = TMaybeType<TVectorType>(right).Cast();
    int size = leftVector
        ? leftVector->Size
        : rightVector->Size;
    if (!IsValidVectorSize(size) || (leftVector && rightVector && leftVector->Size != rightVector->Size)) {
        return std::unexpected(TError(binary->Location,
            "Размеры векторов должны совпадать и быть 2, 4, 8, 16 или 32: "
            + TypeDiagnosticName(left) + " и " + TypeDiagnosticName(right)));
    }
    auto leftElement = leftVector
        ? leftVector->ElementType
        : left;
    auto rightElement = rightVector
        ? rightVector->ElementType
        : right;
    auto isElementType = [&](const TTypePtr& type) {
        return IsNumericType(type) || TMaybeType<TBoolType>(type);
    };
    if (!isElementType(leftElement) || !isElementType(rightElement)) {
        return std::unexpected(TError(binary->Location,
            "Элементы векторов и скалярные операнды должны быть числами или bool: "
            + TypeDiagnosticName(left) + " и " + TypeDiagnosticName(right)));
    }

    RetypeIntegerLiteralOperands(binary->Left, binary->Right, leftElement, rightElement);
    auto types = AnnotateNumericBinaryOp(binary, leftElement, rightElement, context);
    if (
        (binary->Operator == TOperator("&") || binary->Operator == TOperator("|")
            || binary->Operator == TOperator("^"))
        && TMaybeType<TBoolType>(leftElement) && TMaybeType<TBoolType>(rightElement))
    {
        types = TBinaryOpTypes{leftElement, rightElement, leftElement};
    }
    if (!types) {
        return types;
    }
    const bool bitwise = binary->Operator == TOperator("&") || binary->Operator == TOperator("|")
        || binary->Operator == TOperator("^") || binary->Operator == TOperator("<<")
        || binary->Operator == TOperator(">>");
    if (bitwise) {
        // Vector lanes have one width; match scalar bitwise RHS conversion.
        types->Right = types->Left;
    }
    if ((leftVector && IsWideIntegerType(types->Left))
        || (rightVector && IsWideIntegerType(types->Right)) || IsWideIntegerType(types->Result))
    {
        return std::unexpected(TError(binary->Location,
            "Целочисленный элемент вектора должен быть не шире 64 бит."));
    }
    if (leftVector) {
        types->Left = std::make_shared<TVectorType>(types->Left, size);
    }
    if (rightVector) {
        types->Right = std::make_shared<TVectorType>(types->Right, size);
    }
    if (bitwise && !EqualTypes(UnwrapReferenceType(binary->Right->Type), types->Right)) {
        binary->Right = MakeCast(binary->Right, types->Right);
    }
    types->Result = std::make_shared<TVectorType>(types->Result, size);
    return types;
}

TTask AnnotateBinary(std::shared_ptr<TBinaryExpr> binary, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    auto explicitType = binary->Type;
    binary->Left = co_await DoAnnotate(binary->Left, context, scopeId);
    binary->Right = co_await DoAnnotate(binary->Right, context, scopeId);
    if (!binary->Left->Type || !binary->Right->Type) {
        co_return TError(binary->Location, "Не удалось определить типы выражения для бинарной операции");
    }
    auto left = UnwrapReferenceType(binary->Left->Type);
    auto right = UnwrapReferenceType(binary->Right->Type);
    TBinaryOpTypesResult types;
    if (TMaybeType<TVectorType>(left) || TMaybeType<TVectorType>(right)) {
        types = AnnotateVecBinaryOp(binary, left, right, context);
    } else if (IsStringType(left) || IsStringType(right)) {
        types = AnnotateStringBinaryOp(binary, left, right);
    } else {
        types = AnnotateNumericBinaryOp(binary, left, right, context);
    }
    if (!types) {
        if (auto result = TryModuleBinaryOp(binary->Left, binary->Right, binary->Operator, context)) {
            co_return co_await AnnotateIfNeeded(result, context, scopeId);
        }
        if (auto result = co_await TryGenericBinaryOp(binary->Left, binary->Right, binary->Operator, context)) {
            co_return co_await AnnotateIfNeeded(result, context, scopeId);
        }
        co_return types.error();
    }

    binary->Left = InsertImplicitCastIfNeeded(binary->Left, types->Left, &context);
    binary->Right = InsertImplicitCastIfNeeded(binary->Right, types->Right, &context);
    binary->Type = types->Result;
    if (explicitType && !EqualTypes(explicitType, binary->Type)) {
        co_return MakeCast(binary, explicitType);
    }
    if (explicitType) {
        binary->Type = explicitType;
    }
    co_return binary;
}

TTask AnnotateBlock(std::shared_ptr<TBlockExpr> block, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    for (auto& s : block->Stmts) {
        s = co_await DoAnnotate(s, context, NSemantics::TScopeId{block->Scope});
        if (!s->Type) {
            co_return TError(s->Location, "Не удалось определить тип выражения внутри блока");
        }
    }
    // Generic function declarations are blueprints,
    // not real functions - their bodies mention placeholder types and were
    // never body-annotated (see the HasGenericParams guard in
    // AnnotateFunDecl). Only their monomorphized clones, synthesized at call
    // sites and registered separately, should reach lowering/codegen - drop
    // the generic declarations themselves here so lowering never encounters
    // unresolved type parameters.
    std::erase_if(block->Stmts, [](const TExprPtr& s) {
        auto fdecl = TMaybeNode<TFunDecl>(s);
        return fdecl && HasGenericParams(*fdecl.Cast());
    });
    if (!block->Stmts.empty() && block->Stmts.back()->Type) {
        block->Type = block->Stmts.back()->Type;
    } else {
        block->Type = std::make_shared<TVoidType>();
    }
    co_return block;
}

TTask AnnotateAssign(std::shared_ptr<TAssignExpr> assign, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    assign->Value = co_await DoAnnotate(assign->Value, context, scopeId);
    if (!assign->Value->Type) {
        co_return TError(assign->Location, "Нельзя присвоить значение с неопределённым типом переменной: " + assign->Name);
    }
    auto symbolId = context.Lookup(assign->Name, scopeId);
    if (!symbolId) {
        co_return TError(assign->Location, "Переменная не определена: " + assign->Name);
    }
    auto sym = context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
    if (!sym || !sym->Type) {
        co_return TError(assign->Location, "У переменной не определён тип: " + assign->Name);
    }
    auto symbolType = UnwrapReferenceType(sym->Type);

    if (!symbolType->Mutable) {
        co_return TError(assign->Location, "Нельзя присвоить аргументу функции '" + assign->Name + "'. Присваивать можно только переменным, а для аргументов функции — только если они объявлены как 'рез' или 'арг рез'.");
    }

    auto valueType = UnwrapReferenceType(assign->Value->Type);
    if (!EqualTypes(valueType, symbolType)) {
        auto futureValueType = FutureResultType(valueType);
        if (futureValueType && CanImplicit(futureValueType, symbolType, &context)) {
            assign->Type = std::make_shared<NAst::TVoidType>();
            co_return assign;
        }
        if (!CanImplicit(valueType, symbolType, &context)) {
            co_return TError(assign->Location, "Нельзя неявно преобразовать тип '" + TypeDiagnosticName(valueType) + "' к типу '" + TypeDiagnosticName(symbolType) + "' при присваивании переменной '" + assign->Name + "'.");
        }
        assign->Value = InsertImplicitCastIfNeeded(assign->Value, sym->Type, &context);
    }

    assign->Type = std::make_shared<NAst::TVoidType>();
    co_return assign;
}

TTask AnnotateArrayAssign(std::shared_ptr<TArrayAssignExpr> arrayAssign, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    arrayAssign->Value = co_await DoAnnotate(arrayAssign->Value, context, scopeId);
    if (!arrayAssign->Value->Type) {
        co_return TError(arrayAssign->Location, "Нельзя присвоить значение без типа элементу массива '" + arrayAssign->Name + "'.");
    }
    auto symbolId = context.Lookup(arrayAssign->Name, scopeId);
    if (!symbolId) {
        co_return TError(arrayAssign->Location, "Массив не определён: '" + arrayAssign->Name + "'.");
    }
    auto sym = context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
    if (!sym || !sym->Type) {
        co_return TError(arrayAssign->Location, "У массива не определён тип: '" + arrayAssign->Name + "'.");
    }
    // Allow indexed assignment for arrays, strings and raw kernel pointers.
    auto maybeArrayType = TMaybeType<TArrayType>(sym->Type);
    auto maybeStringType = TMaybeType<TStringType>(sym->Type);
    auto maybePointerType = TMaybeType<TPointerType>(sym->Type);
    if (maybeArrayType) {
        auto arrayType = maybeArrayType.Cast();
        auto valueType = UnwrapReferenceType(arrayAssign->Value->Type);
        if (!EqualTypes(valueType, arrayType->ElementType)) {
            if (!CanImplicit(valueType, arrayType->ElementType, &context)) {
                co_return TError(arrayAssign->Location, "Нельзя неявно преобразовать тип '" +
                    TypeDiagnosticName(valueType) + "' к типу '" + TypeDiagnosticName(arrayType->ElementType) + "' при присваивании элементу массива '" + arrayAssign->Name + "'.");
            }
            arrayAssign->Value = InsertImplicitCastIfNeeded(arrayAssign->Value, arrayType->ElementType, &context);
        }
        for (auto& indexExpr : arrayAssign->Indices) {
            indexExpr = co_await DoAnnotate(indexExpr, context, scopeId);
            if (!indexExpr->Type) {
                co_return TError(arrayAssign->Location, "Индекс в присваивании элементу массива '" + arrayAssign->Name + "' не имеет типа. Убедитесь, что выражение индекса корректно и его тип определён.");
            }
            auto maybeIntType = TMaybeType<TIntegerType>(indexExpr->Type);
            if (!maybeIntType) {
                co_return TError(arrayAssign->Location, "Индекс в присваивании элементу массива '" + arrayAssign->Name + "' должен быть целым числом. Проверьте выражение индекса.");
            }
        }
        // check arity
        if (arrayAssign->Indices.size() != arrayType->Arity) {
            co_return TError(arrayAssign->Location, "Неверное количество индексов в присваивании элементу массива '" + arrayAssign->Name + "': ожидается " + std::to_string(arrayType->Arity) + ", получено " + std::to_string(arrayAssign->Indices.size()) + ".");
        }
        arrayAssign->Type = std::make_shared<NAst::TVoidType>();
        co_return arrayAssign;
    } else if (maybePointerType) {
        auto pointerType = maybePointerType.Cast();
        if (arrayAssign->Indices.size() != 1) {
            co_return TError(arrayAssign->Location, "В присваивании по указателю '" + arrayAssign->Name + "' должен быть ровно один индекс.");
        }

        arrayAssign->Indices[0] = co_await DoAnnotate(arrayAssign->Indices[0], context, scopeId);
        if (!arrayAssign->Indices[0]->Type) {
            co_return TError(arrayAssign->Location, "Индекс в присваивании по указателю '" + arrayAssign->Name + "' не имеет типа.");
        }
        auto intType = std::make_shared<TIntegerType>();
        if (!EqualTypes(arrayAssign->Indices[0]->Type, intType)) {
            if (!CanImplicit(arrayAssign->Indices[0]->Type, intType, &context)) {
                co_return TError(arrayAssign->Location, "Индекс в присваивании по указателю '" + arrayAssign->Name + "' должен быть целым числом.");
            }
            arrayAssign->Indices[0] = InsertImplicitCastIfNeeded(arrayAssign->Indices[0], intType, &context);
        }

        auto valueType = UnwrapReferenceType(arrayAssign->Value->Type);
        if (!EqualTypes(valueType, pointerType->PointeeType)) {
            if (!CanImplicit(valueType, pointerType->PointeeType, &context)) {
                co_return TError(arrayAssign->Location, "Нельзя неявно преобразовать тип '" +
                    TypeDiagnosticName(valueType) + "' к типу '" + TypeDiagnosticName(pointerType->PointeeType) + "' при присваивании по указателю '" + arrayAssign->Name + "'.");
            }
            arrayAssign->Value = InsertImplicitCastIfNeeded(arrayAssign->Value, pointerType->PointeeType, &context);
        }

        arrayAssign->Type = std::make_shared<NAst::TVoidType>();
        co_return arrayAssign;
    } else if (maybeStringType) {
        // Only allow s[1] = 'c' (symbol to string)
        if (arrayAssign->Indices.size() != 1) {
            co_return TError(arrayAssign->Location, "В присваивании элементу строки '" + arrayAssign->Name + "' должно быть ровно один индекс (например, s[1]='a'). Получено индексов: " + std::to_string(arrayAssign->Indices.size()) + ".");
        }
        // Index must be integer
        arrayAssign->Indices[0] = co_await DoAnnotate(arrayAssign->Indices[0], context, scopeId);
        if (!arrayAssign->Indices[0]->Type || !TMaybeType<TIntegerType>(arrayAssign->Indices[0]->Type)) {
            co_return TError(arrayAssign->Location, "Индекс в присваивании элементу строки '" + arrayAssign->Name + "' должен быть целым числом (например, s[1]='a').");
        }
        // Value must be symbol (char)
        auto valueType = UnwrapReferenceType(arrayAssign->Value->Type);
        if (!TMaybeType<TSymbolType>(valueType)) {
            co_return TError(arrayAssign->Location, "Значение в присваивании элементу строки '" + arrayAssign->Name + "' должно быть символом (например, s[1]='a').");
        }
        arrayAssign->Type = std::make_shared<NAst::TVoidType>();
        co_return arrayAssign;
    } else {
        co_return TError(arrayAssign->Location, "Идентификатор '" + arrayAssign->Name + "' не является массивом, строкой или указателем, поэтому нельзя использовать присваивание по индексу.");
    }
}

TTask AnnotateVar(std::shared_ptr<TVarStmt> var, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    if (!var->Type) {
        // (var name = expr): type inferred from init expression
        if (!var->Init) {
            co_return TError(var->Location, "Переменная без типа должна иметь инициализатор: " + var->Name);
        }
        var->Init = co_await DoAnnotate(var->Init, context, scopeId);
        if (!var->Init->Type) {
            co_return TError(var->Init->Location, "Не удалось определить тип инициализатора переменной: " + var->Name);
        }
        var->Type = UnwrapReferenceType(var->Init->Type);
        auto sidOpt = context.Lookup(var->Name, scopeId);
        if (sidOpt) {
            auto sym = context.GetSymbolNode(NSemantics::TSymbolId{sidOpt->Id});
            if (sym) { sym->Type = var->Type; }
        }
        co_return var;
    }
    for (auto& [from, to] : var->Bounds) {
        if (from) {
            from = co_await DoAnnotate(from, context, scopeId);
        }
        if (to) {
            to = co_await DoAnnotate(to, context, scopeId);
        }
    }
    if (var->Init) {
        var->Init = co_await DoAnnotate(var->Init, context, scopeId);
        auto initType = UnwrapReferenceType(var->Init->Type);
        auto variableType = UnwrapReferenceType(var->Type);
        if (!EqualTypes(initType, variableType)) {
            if (!CanImplicit(initType, variableType, &context)) {
                co_return TError(var->Location,
                    "Нельзя инициализировать переменную '" + var->Name
                    + "' типа '" + TypeDiagnosticName(variableType)
                    + "' значением типа '" + TypeDiagnosticName(initType) + "'.");
            }
            var->Init = InsertImplicitCastIfNeeded(var->Init, variableType, &context);
        }
    }
    co_return var;
}

TTask AnnotateIdent(std::shared_ptr<TIdentExpr> ident, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId, bool pathThrough = false) {
    auto symbolId = context.Lookup(ident->Name, scopeId);
    if (!symbolId) {
        co_return TError(ident->Location, "Идентификатор '" + ident->Name + "' не определён. Проверьте правильность написания имени или объявите переменную перед использованием.");
    }
    auto sym = context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
    if (!sym) {
        co_return TError(ident->Location, "Внутренняя ошибка: не удалось получить информацию о символе идентификатора '" + ident->Name + "'.");
    }
    ident->Type = sym->Type;
    if (!ident->Type) {
        co_return TError(ident->Location, "У идентификатора '" + ident->Name + "' не определён тип. Возможно, переменная объявлена без типа или произошла ошибка при анализе типов.");
    }
    if (pathThrough) {
        co_return ident;
    }
    auto unwrappedType = UnwrapReferenceType(ident->Type);
    if (!unwrappedType->Readable) {
        co_return TError(ident->Location, "Нельзя читать из `рез' аргумента '" + ident->Name + "'.\n"
            "`рез' аргумент предназначен только для записи. Если нужно читать значение, объявите его как `арг рез'.");
    }
    if (auto maybeArray = TMaybeType<TArrayType>(unwrappedType)) {
        auto arrayType = maybeArray.Cast();
        if (!arrayType->ElementType->Readable) {
            co_return TError(ident->Location, "Нельзя читать элементы массива `рез' аргумента '" + ident->Name + "'.\n"
                "Массив `рез' аргумента предназначен только для записи. Если нужно читать элементы, объявите аргумент как `арг рез'.");
        }
    }

    co_return ident;
}

// --- Generic instantiation ---------------------------------------------------
//
// A function-level generic parameter list (`(fun id [T] ...)`) marks the
// declaration as generic. At each call site the concrete types bound to its
// type parameters are inferred from argument types, and a monomorphized clone
// of the function is substituted, registered under a synthetic mangled name
// and cached by it.

TGenericParamSets GenericParamSets(const TFunDecl& decl) {
    TGenericParamSets result;
    for (const auto& param : decl.GenericParams) {
        if (param.Kind == TGenericParam::EKind::Type) {
            result.Types.insert(param.Name);
        } else {
            result.Values.insert(param.Name);
            result.ValueTypes[param.Name] = param.ValueType;
        }
    }
    return result;
}

bool HasGenericParams(const TFunDecl& decl) {
    return !decl.GenericParams.empty();
}

// Rebuilds a fresh TType instance with the same derived "shape" (kind, name,
// nested types...) as `shape`, but default base TType attributes - the
// starting point for substituting a generic type parameter: the result must
// structurally equal the bound concrete type, while the parameter usage
// site's own attributes get overlaid on top (see SubstituteGenericType).
TTypePtr CloneTypeShape(const TTypePtr& shape) {
    if (!shape) {
        return shape;
    }
    if (auto t = TMaybeType<TIntegerType>(shape)) {
        return std::make_shared<TIntegerType>(t.Cast()->Kind);
    }
    if (TMaybeType<TFloatType>(shape)) {
        return std::make_shared<TFloatType>();
    }
    if (TMaybeType<TBoolType>(shape)) {
        return std::make_shared<TBoolType>();
    }
    if (TMaybeType<TStringType>(shape)) {
        return std::make_shared<TStringType>();
    }
    if (TMaybeType<TSymbolType>(shape)) {
        return std::make_shared<TSymbolType>();
    }
    if (TMaybeType<TVoidType>(shape)) {
        return std::make_shared<TVoidType>();
    }
    if (auto t = TMaybeType<TFunctionType>(shape)) {
        auto src = t.Cast();
        return std::make_shared<TFunctionType>(src->ParamTypes, src->ReturnType);
    }
    if (auto t = TMaybeType<TFutureType>(shape)) {
        return std::make_shared<TFutureType>(t.Cast()->ResultType);
    }
    if (auto t = TMaybeType<TArrayType>(shape)) {
        auto src = t.Cast();
        return std::make_shared<TArrayType>(src->ElementType, src->Arity);
    }
    if (auto t = TMaybeType<TVectorType>(shape)) {
        return std::make_shared<TVectorType>(*t.Cast());
    }
    if (auto t = TMaybeType<TPointerType>(shape)) {
        return std::make_shared<TPointerType>(t.Cast()->PointeeType);
    }
    if (auto t = TMaybeType<TReferenceType>(shape)) {
        return std::make_shared<TReferenceType>(t.Cast()->ReferencedType);
    }
    if (auto t = TMaybeType<TNamedType>(shape)) {
        auto src = t.Cast();
        auto result = std::make_shared<TNamedType>(src->Name, src->UnderlyingType, src->TypeArgs);
        result->Reference = src->Reference;
        return result;
    }
    if (auto t = TMaybeType<TStructType>(shape)) {
        return std::make_shared<TStructType>(t.Cast()->Fields);
    }
    return shape;
}

// Replaces every generic type parameter reachable in `type` with its bound
// concrete type from `bindings`, recursing into composite wrappers exactly
// like resolveTypeRef does. Preserves every TType attribute of the parameter
// usage site by copying the base TType sub-object onto the cloned shape.
TTypePtr SubstituteGenericType(
    const TTypePtr& type,
    const TGenericParamSets& genericParams,
    const TGenericBindings& bindings)
{
    if (!type) {
        return type;
    }
    if (auto named = TMaybeType<TNamedType>(type)) {
        auto placeholder = named.Cast();
        if (placeholder->TypeArgs.empty() && genericParams.Types.contains(placeholder->Name)) {
            auto it = bindings.Types.find(placeholder->Name);
            if (it == bindings.Types.end()) {
                return type; // left for the caller to detect (unbound generic parameter)
            }
            auto result = CloneTypeShape(it->second);
            static_cast<TType&>(*result) = static_cast<const TType&>(*placeholder);
            return result;
        }

        auto underlying = SubstituteGenericType(placeholder->UnderlyingType, genericParams, bindings);
        bool changed = underlying != placeholder->UnderlyingType;
        std::vector<TGenericArg> typeArgs;
        typeArgs.reserve(placeholder->TypeArgs.size());
        for (const auto& arg : placeholder->TypeArgs) {
            if (arg.Kind == TGenericArg::EKind::Type) {
                auto substituted = SubstituteGenericType(arg.Type, genericParams, bindings);
                changed = changed || substituted != arg.Type;
                typeArgs.push_back(TGenericArg::TypeArg(std::move(substituted)));
            } else {
                auto it = bindings.Values.find(arg.Value);
                if (it != bindings.Values.end() && genericParams.Values.contains(arg.Value)) {
                    changed = true;
                    typeArgs.push_back(TGenericArg::ValueArg(it->second));
                } else {
                    typeArgs.push_back(arg);
                }
            }
        }
        if (!changed) {
            return type;
        }
        auto result = std::make_shared<TNamedType>(placeholder->Name, std::move(underlying), std::move(typeArgs));
        result->Reference = placeholder->Reference;
        static_cast<TType&>(*result) = static_cast<const TType&>(*placeholder);
        return result;
    }
    if (auto arr = TMaybeType<TArrayType>(type)) {
        auto src = arr.Cast();
        auto elem = SubstituteGenericType(src->ElementType, genericParams, bindings);
        if (elem == src->ElementType) return type;
        auto result = std::make_shared<TArrayType>(std::move(elem), src->Arity);
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    if (auto vector = TMaybeType<TVectorType>(type)) {
        auto result = std::make_shared<TVectorType>(*vector.Cast());
        result->ElementType = SubstituteGenericType(result->ElementType, genericParams, bindings);
        if (auto it = bindings.Values.find(result->SizeParam); it != bindings.Values.end()) {
            result->BindSize(it->second);
        }
        return result;
    }
    if (auto ptr = TMaybeType<TPointerType>(type)) {
        auto src = ptr.Cast();
        auto pointee = SubstituteGenericType(src->PointeeType, genericParams, bindings);
        if (pointee == src->PointeeType) return type;
        auto result = std::make_shared<TPointerType>(std::move(pointee));
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    if (auto ref = TMaybeType<TReferenceType>(type)) {
        auto src = ref.Cast();
        auto referenced = SubstituteGenericType(src->ReferencedType, genericParams, bindings);
        if (referenced == src->ReferencedType) return type;
        auto result = std::make_shared<TReferenceType>(std::move(referenced));
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    if (auto future = TMaybeType<TFutureType>(type)) {
        auto src = future.Cast();
        auto inner = SubstituteGenericType(src->ResultType, genericParams, bindings);
        if (inner == src->ResultType) return type;
        auto result = std::make_shared<TFutureType>(std::move(inner));
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    if (auto fun = TMaybeType<TFunctionType>(type)) {
        auto src = fun.Cast();
        bool changed = false;
        std::vector<TTypePtr> params;
        params.reserve(src->ParamTypes.size());
        for (auto& p : src->ParamTypes) {
            auto sp = SubstituteGenericType(p, genericParams, bindings);
            changed = changed || (sp != p);
            params.push_back(std::move(sp));
        }
        auto ret = SubstituteGenericType(src->ReturnType, genericParams, bindings);
        changed = changed || (ret != src->ReturnType);
        if (!changed) return type;
        auto result = std::make_shared<TFunctionType>(std::move(params), std::move(ret));
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    if (auto str = TMaybeType<TStructType>(type)) {
        auto src = str.Cast();
        bool changed = false;
        std::vector<std::pair<std::string, TTypePtr>> fields;
        fields.reserve(src->Fields.size());
        for (auto& [name, fieldType] : src->Fields) {
            auto sf = SubstituteGenericType(fieldType, genericParams, bindings);
            changed = changed || (sf != fieldType);
            fields.emplace_back(name, std::move(sf));
        }
        if (!changed) return type;
        auto result = std::make_shared<TStructType>(std::move(fields));
        static_cast<TType&>(*result) = static_cast<const TType&>(*src);
        return result;
    }
    return type;
}

// Unifies a (possibly generic-parameterized) declared parameter type against
// the concrete type of an actual argument, extending `bindings`. Returns an
// error message when the same generic parameter name would have to bind to two
// structurally different concrete types.
std::optional<std::string> UnifyGenericType(
    const TTypePtr& paramType,
    const TTypePtr& argType,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    TExprPtr argExpr = nullptr,
    bool retypeIntegerLiterals = true)
{
    if (!paramType || !argType) {
        return std::nullopt;
    }
    if (auto named = TMaybeType<TNamedType>(paramType)) {
        auto placeholder = named.Cast();
        if (placeholder->TypeArgs.empty() && genericParams.Types.contains(placeholder->Name)) {
            auto concrete = UnwrapReferenceType(argType);
            auto [it, inserted] = bindings.Types.try_emplace(placeholder->Name, concrete);
            if (!inserted && TypeKey(it->second) != TypeKey(concrete)) {
                if (IntegerLiteralCanUseType(argExpr, it->second)) {
                    if (retypeIntegerLiterals) {
                        RetypeIntegerLiteralIfFits(argExpr, it->second);
                    }
                    concrete = it->second;
                }
            }
            if (!inserted && TypeKey(it->second) != TypeKey(concrete)) {
                return "тип обобщённого параметра '" + placeholder->Name + "' определяется неоднозначно: то как '"
                    + it->second->ToString() + "', то как '" + concrete->ToString() + "'";
            }
            return std::nullopt;
        }

        auto argNamed = TMaybeType<TNamedType>(argType);
        if (argNamed && placeholder->Name == argNamed.Cast()->Name
            && placeholder->TypeArgs.size() == argNamed.Cast()->TypeArgs.size())
        {
            for (size_t i = 0; i < placeholder->TypeArgs.size(); ++i) {
                const auto& placeholderArg = placeholder->TypeArgs[i];
                const auto& arg = argNamed.Cast()->TypeArgs[i];
                if (placeholderArg.Kind == TGenericArg::EKind::Type && arg.Kind == TGenericArg::EKind::Type) {
                    if (auto err = UnifyGenericType(
                            placeholderArg.Type,
                            arg.Type,
                            genericParams,
                            bindings,
                            nullptr,
                            retypeIntegerLiterals))
                    {
                        return err;
                    }
                } else if (placeholderArg.Kind == TGenericArg::EKind::Value && arg.Kind == TGenericArg::EKind::Value) {
                    if (genericParams.Values.contains(placeholderArg.Value)) {
                        auto [it, inserted] = bindings.Values.try_emplace(placeholderArg.Value, arg.Value);
                        if (!inserted && it->second != arg.Value) {
                            return "значение обобщённого параметра '" + placeholderArg.Value
                                + "' определяется неоднозначно: то как '"
                                + it->second + "', то как '" + arg.Value + "'";
                        }
                    } else if (placeholderArg.Value != arg.Value) {
                        return "значение параметра типа '" + placeholder->Name
                            + "' не совпадает: ожидается '" + placeholderArg.Value
                            + "', получено '" + arg.Value + "'";
                    }
                } else {
                    return "параметр типа '" + placeholder->Name + "' имеет несовместимый вид";
                }
            }
        }
        return std::nullopt;
    }
    if (auto arr = TMaybeType<TArrayType>(paramType)) {
        if (auto argArr = TMaybeType<TArrayType>(argType)) {
            return UnifyGenericType(
                arr.Cast()->ElementType,
                argArr.Cast()->ElementType,
                genericParams,
                bindings,
                nullptr,
                retypeIntegerLiterals);
        }
        return std::nullopt;
    }
    if (auto vector = TMaybeType<TVectorType>(paramType)) {
        auto actual = TMaybeType<TVectorType>(argType).Cast();
        if (!actual) {
            return "ожидается векторный аргумент";
        }
        auto param = vector.Cast();
        if (genericParams.Values.contains(param->SizeParam)) {
            const auto size = std::to_string(actual->Size);
            auto [it, inserted] = bindings.Values.try_emplace(param->SizeParam, size);
            if (!inserted && it->second != size) {
                return "значение обобщённого параметра '" + param->SizeParam + "' определяется неоднозначно";
            }
        } else if (param->Size != actual->Size) {
            return "размеры векторных аргументов должны совпадать";
        }
        return UnifyGenericType(param->ElementType, actual->ElementType, genericParams,
            bindings, nullptr, retypeIntegerLiterals);
    }
    if (auto ptr = TMaybeType<TPointerType>(paramType)) {
        if (auto argPtr = TMaybeType<TPointerType>(argType)) {
            return UnifyGenericType(
                ptr.Cast()->PointeeType,
                argPtr.Cast()->PointeeType,
                genericParams,
                bindings,
                nullptr,
                retypeIntegerLiterals);
        }
        return std::nullopt;
    }
    if (auto ref = TMaybeType<TReferenceType>(paramType)) {
        auto argRef = TMaybeType<TReferenceType>(argType);
        return UnifyGenericType(
            ref.Cast()->ReferencedType,
            argRef ? argRef.Cast()->ReferencedType : argType,
            genericParams,
            bindings,
            argExpr,
            retypeIntegerLiterals);
    }
    if (auto future = TMaybeType<TFutureType>(paramType)) {
        if (auto argFuture = TMaybeType<TFutureType>(argType)) {
            return UnifyGenericType(
                future.Cast()->ResultType,
                argFuture.Cast()->ResultType,
                genericParams,
                bindings,
                nullptr,
                retypeIntegerLiterals);
        }
        return std::nullopt;
    }
    if (auto fun = TMaybeType<TFunctionType>(paramType)) {
        auto argFun = TMaybeType<TFunctionType>(argType);
        if (!argFun) {
            return std::nullopt;
        }
        auto pf = fun.Cast();
        auto af = argFun.Cast();
        if (pf->ParamTypes.size() != af->ParamTypes.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < pf->ParamTypes.size(); ++i) {
            if (auto err = UnifyGenericType(
                    pf->ParamTypes[i],
                    af->ParamTypes[i],
                    genericParams,
                    bindings,
                    nullptr,
                    retypeIntegerLiterals))
            {
                return err;
            }
        }
        return UnifyGenericType(
            pf->ReturnType,
            af->ReturnType,
            genericParams,
            bindings,
            nullptr,
            retypeIntegerLiterals);
    }
    if (auto str = TMaybeType<TStructType>(paramType)) {
        auto argStr = TMaybeType<TStructType>(argType);
        if (!argStr) {
            return std::nullopt;
        }
        auto ps = str.Cast();
        auto as = argStr.Cast();
        if (ps->Fields.size() != as->Fields.size()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < ps->Fields.size(); ++i) {
            if (auto err = UnifyGenericType(
                    ps->Fields[i].second,
                    as->Fields[i].second,
                    genericParams,
                    bindings,
                    nullptr,
                    retypeIntegerLiterals))
            {
                return err;
            }
        }
    }
    return std::nullopt;
}

bool IsGenericTypeParamRef(const TTypePtr& type, const std::unordered_set<std::string>& genericTypeParams) {
    auto named = TMaybeType<TNamedType>(UnwrapReferenceType(type));
    return named
        && named.Cast()->TypeArgs.empty()
        && genericTypeParams.contains(named.Cast()->Name);
}

std::optional<std::string> InferGenericBindings(
    const TFunDecl& decl,
    const std::vector<TExprPtr>& args,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    bool retypeIntegerLiterals,
    TTypePtr expectedReturnType = nullptr,
    bool requireAllBindings = true)
{
    const size_t count = std::min(decl.Params.size(), args.size());
    auto unifyArg = [&](size_t i) -> std::optional<std::string> {
        return UnifyGenericType(
            decl.Params[i]->Type,
            UnwrapReferenceType(args[i]->Type),
            genericParams,
            bindings,
            args[i],
            retypeIntegerLiterals);
    };

    for (size_t i = 0; i < count; ++i) {
        if (!IsGenericTypeParamRef(decl.Params[i]->Type, genericParams.Types)) {
            if (auto err = unifyArg(i)) {
                return err;
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (IsGenericTypeParamRef(decl.Params[i]->Type, genericParams.Types) && !IsIntegerLiteral(args[i])) {
            if (auto err = unifyArg(i)) {
                return err;
            }
        }
    }
    if (expectedReturnType) {
        if (auto err = UnifyGenericType(
                decl.RetType,
                UnwrapReferenceType(expectedReturnType),
                genericParams,
                bindings,
                nullptr,
                retypeIntegerLiterals))
        {
            return err;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (IsGenericTypeParamRef(decl.Params[i]->Type, genericParams.Types) && IsIntegerLiteral(args[i])) {
            if (auto err = unifyArg(i)) {
                return err;
            }
        }
    }

    if (requireAllBindings) {
        for (const auto& param : decl.GenericParams) {
            if (param.Kind == TGenericParam::EKind::Type && !bindings.Types.contains(param.Name)) {
                return "не удалось определить тип обобщённого параметра '" + param.Name + "' по аргументам вызова";
            }
            if (param.Kind == TGenericParam::EKind::Value && !bindings.Values.contains(param.Name)) {
                return "не удалось определить значение обобщённого параметра '" + param.Name + "' по аргументам вызова";
            }
        }
    }
    return std::nullopt;
}

// Deep-clones an AST subtree, substituting generic type parameters in every
// declared/annotated type along the way (TVarStmt::Type, cast target types,
// ...) and resetting scope bookkeeping (TBlockExpr/TFunDecl/TLetExpr
// ::Scope, TLetExpr::TBinding::Symbol) so the clone gets fresh symbol-table
// entries on the next name-resolution pass: a cloned body cannot share scopes
// with its generic declaration - Symbols bind scope entries to specific AST node
// identities, and sharing would make lookups resolve to the generic declaration's
// (wrongly-typed) original nodes instead of this clone's.
TExprPtr CloneAndSubstituteExpr(
    const TExprPtr& node,
    const TGenericParamSets& genericParams,
    const TGenericBindings& bindings)
{
    if (!node) {
        return node;
    }
    auto clone = ShallowCloneNode(node);
    if (auto ident = TMaybeNode<TIdentExpr>(clone)) {
        auto it = bindings.Values.find(ident.Cast()->Name);
        if (it != bindings.Values.end() && genericParams.Values.contains(ident.Cast()->Name)) {
            const int64_t value = it->second.starts_with('-')
                ? std::stoll(it->second)
                : std::bit_cast<int64_t>(static_cast<uint64_t>(std::stoull(it->second)));
            auto number = std::make_shared<TNumberExpr>(clone->Location, value);
            number->Type = genericParams.ValueTypes.at(ident.Cast()->Name);
            return number;
        }
    }
    if (clone->Type) {
        clone->Type = SubstituteGenericType(clone->Type, genericParams, bindings);
    }
    if (auto block = TMaybeNode<TBlockExpr>(clone)) {
        block.Cast()->Scope = -1;
    } else if (auto fdecl = TMaybeNode<TFunDecl>(clone)) {
        fdecl.Cast()->Scope = -1;
    }
    for (auto* child : clone->MutableChildren()) {
        *child = CloneAndSubstituteExpr(*child, genericParams, bindings);
    }
    return clone;
}

bool AllGenericBindingsPresent(
    const std::vector<TGenericParam>& params,
    const TGenericBindings& bindings)
{
    for (const auto& param : params) {
        if (param.Kind == TGenericParam::EKind::Type && !bindings.Types.contains(param.Name)) {
            return false;
        }
        if (param.Kind == TGenericParam::EKind::Value && !bindings.Values.contains(param.Name)) {
            return false;
        }
    }
    return true;
}

bool ContainsGenericParam(
    const TTypePtr& type,
    const TGenericParamSets& genericParams)
{
    if (!type) {
        return false;
    }
    if (auto named = TMaybeType<TNamedType>(type)) {
        auto src = named.Cast();
        if (src->TypeArgs.empty() && genericParams.Types.contains(src->Name)) {
            return true;
        }
        for (const auto& arg : src->TypeArgs) {
            if (arg.Kind == TGenericArg::EKind::Type) {
                if (ContainsGenericParam(arg.Type, genericParams)) {
                    return true;
                }
            } else if (genericParams.Values.contains(arg.Value)) {
                return true;
            }
        }
        return ContainsGenericParam(src->UnderlyingType, genericParams);
    }
    if (auto arr = TMaybeType<TArrayType>(type)) {
        return ContainsGenericParam(arr.Cast()->ElementType, genericParams);
    }
    if (auto vector = TMaybeType<TVectorType>(type)) {
        return genericParams.Values.contains(vector.Cast()->SizeParam)
            || ContainsGenericParam(vector.Cast()->ElementType, genericParams);
    }
    if (auto ptr = TMaybeType<TPointerType>(type)) {
        return ContainsGenericParam(ptr.Cast()->PointeeType, genericParams);
    }
    if (auto ref = TMaybeType<TReferenceType>(type)) {
        return ContainsGenericParam(ref.Cast()->ReferencedType, genericParams);
    }
    if (auto future = TMaybeType<TFutureType>(type)) {
        return ContainsGenericParam(future.Cast()->ResultType, genericParams);
    }
    if (auto fun = TMaybeType<TFunctionType>(type)) {
        for (const auto& param : fun.Cast()->ParamTypes) {
            if (ContainsGenericParam(param, genericParams)) {
                return true;
            }
        }
        return ContainsGenericParam(fun.Cast()->ReturnType, genericParams);
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            if (ContainsGenericParam(fieldType, genericParams)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<TGenericParam> UnboundGenericParams(
    const TFunDecl& decl,
    const TGenericBindings& bindings)
{
    std::vector<TGenericParam> result;
    for (const auto& param : decl.GenericParams) {
        if (param.Kind == TGenericParam::EKind::Type && !bindings.Types.contains(param.Name)) {
            result.push_back(param);
        } else if (param.Kind == TGenericParam::EKind::Value && !bindings.Values.contains(param.Name)) {
            result.push_back(param);
        }
    }
    return result;
}

std::optional<std::string> RequireAllGenericBindings(
    const std::vector<TGenericParam>& params,
    const TGenericBindings& bindings)
{
    for (const auto& param : params) {
        if (param.Kind == TGenericParam::EKind::Type && !bindings.Types.contains(param.Name)) {
            return "не удалось определить тип обобщённого параметра '" + param.Name
                + "' по аргументам вызова или телу функции";
        }
        if (param.Kind == TGenericParam::EKind::Value && !bindings.Values.contains(param.Name)) {
            return "не удалось определить значение обобщённого параметра '" + param.Name
                + "' по аргументам вызова или телу функции";
        }
    }
    return std::nullopt;
}

TGenericBoolTask InferGenericBindingsFromReturnContext(
    TExprPtr& expr,
    TTypePtr targetType,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId);

TGenericBoolTask InferGenericBindingsFromStructConstruct(
    std::shared_ptr<TStructConstructExpr> structure,
    TTypePtr targetType,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    auto targetStruct = TMaybeType<TStructType>(UnwrapNamedType(UnwrapReferenceType(targetType))).Cast();
    if (!targetStruct) {
        co_return false;
    }

    std::unordered_map<std::string, size_t> fieldByName;
    for (size_t i = 0; i < targetStruct->Fields.size(); ++i) {
        fieldByName.emplace(targetStruct->Fields[i].first, i);
    }

    bool inferred = false;
    for (size_t i = 0; i < structure->Fields.size(); ++i) {
        size_t fieldIndex = i;
        if (i < structure->FieldNames.size()) {
            auto it = fieldByName.find(structure->FieldNames[i]);
            if (it == fieldByName.end()) {
                continue;
            }
            fieldIndex = it->second;
        }
        if (fieldIndex >= targetStruct->Fields.size()) {
            continue;
        }

        auto fieldTargetType = targetStruct->Fields[fieldIndex].second;
        if (!ContainsGenericParam(fieldTargetType, genericParams)) {
            continue;
        }

        auto& field = structure->Fields[i];
        inferred = (co_await InferGenericBindingsFromReturnContext(
            field,
            fieldTargetType,
            genericParams,
            bindings,
            context,
            scopeId)) || inferred;
        if (!field->Type) {
            field = co_await DoAnnotate(field, context, scopeId);
        }
        if (!field->Type) {
            co_return TError(field->Location, "Не удалось определить тип поля возвращаемой структуры.");
        }
        if (auto err = UnifyGenericType(
                fieldTargetType,
                UnwrapReferenceType(field->Type),
                genericParams,
                bindings))
        {
            co_return TError(field->Location, *err);
        }
        inferred = true;
    }

    co_return inferred;
}

TGenericBoolTask InferGenericBindingsFromReturnContext(
    TExprPtr& expr,
    TTypePtr targetType,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    if (!expr || !targetType || !ContainsGenericParam(targetType, genericParams)) {
        co_return false;
    }

    if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        co_return co_await InferGenericBindingsFromReturnContext(
            cast.Cast()->Operand,
            cast.Cast()->Type,
            genericParams,
            bindings,
            context,
            scopeId);
    }

    if (auto ifExpr = TMaybeNode<TIfExpr>(expr)) {
        bool inferred = false;
        inferred = (co_await InferGenericBindingsFromReturnContext(
            ifExpr.Cast()->Then,
            targetType,
            genericParams,
            bindings,
            context,
            scopeId)) || inferred;
        if (ifExpr.Cast()->Else) {
            inferred = (co_await InferGenericBindingsFromReturnContext(
                ifExpr.Cast()->Else,
                targetType,
                genericParams,
                bindings,
                context,
                scopeId)) || inferred;
        }
        co_return inferred;
    }

    if (auto structure = TMaybeNode<TStructConstructExpr>(expr)) {
        co_return co_await InferGenericBindingsFromStructConstruct(
            structure.Cast(),
            targetType,
            genericParams,
            bindings,
            context,
            scopeId);
    }

    expr = co_await DoAnnotate(expr, context, scopeId);
    if (!expr->Type) {
        co_return TError(expr->Location, "Не удалось определить тип возвращаемого выражения.");
    }
    if (auto err = UnifyGenericType(
            targetType,
            UnwrapReferenceType(expr->Type),
            genericParams,
            bindings))
    {
        co_return TError(expr->Location, *err);
    }
    co_return true;
}

TGenericBoolTask InferGenericBindingsFromExplicitReturns(
    TExprPtr& node,
    TTypePtr returnType,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    if (!node) {
        co_return false;
    }
    if (auto ret = TMaybeNode<TReturnExpr>(node)) {
        if (!ret.Cast()->Value) {
            co_return false;
        }
        co_return co_await InferGenericBindingsFromReturnContext(
            ret.Cast()->Value,
            returnType,
            genericParams,
            bindings,
            context,
            scopeId);
    }

    bool inferred = false;
    for (auto* child : node->MutableChildren()) {
        if (!child || !*child) {
            continue;
        }
        inferred = (co_await InferGenericBindingsFromExplicitReturns(
            *child,
            returnType,
            genericParams,
            bindings,
            context,
            scopeId)) || inferred;
    }
    co_return inferred;
}

TGenericBoolTask InferGenericBindingsFromReturnBody(
    const std::shared_ptr<TFunDecl>& genericDecl,
    const TGenericParamSets& genericParams,
    TGenericBindings& bindings,
    NSemantics::TNameResolver& context,
    TLocation callLoc)
{
    if (AllGenericBindingsPresent(genericDecl->GenericParams, bindings)) {
        co_return false;
    }

    std::vector<TParam> params;
    params.reserve(genericDecl->Params.size());
    for (auto& param : genericDecl->Params) {
        auto clonedParam = std::make_shared<TVarStmt>(*param);
        clonedParam->Type = SubstituteGenericType(param->Type, genericParams, bindings);
        for (auto& bound : clonedParam->Bounds) {
            bound.first = CloneAndSubstituteExpr(bound.first, genericParams, bindings);
            bound.second = CloneAndSubstituteExpr(bound.second, genericParams, bindings);
        }
        params.push_back(std::move(clonedParam));
    }

    auto retType = SubstituteGenericType(genericDecl->RetType, genericParams, bindings);
    auto body = TMaybeNode<TBlockExpr>(CloneAndSubstituteExpr(genericDecl->Body, genericParams, bindings)).Cast();
    auto probe = std::make_shared<TFunDecl>(
        genericDecl->Location,
        genericDecl->Name + "$return_infer",
        UnboundGenericParams(*genericDecl, bindings),
        std::move(params),
        body,
        retType);

    std::vector<TTypePtr> paramTypes;
    paramTypes.reserve(probe->Params.size());
    for (auto& param : probe->Params) {
        paramTypes.push_back(param->Type);
    }
    probe->Type = std::make_shared<TFunctionType>(std::move(paramTypes), probe->RetType);

    auto declRes = context.ResolveInstantiatedFunDecl(probe, false);
    if (!declRes) {
        co_return declRes.error();
    }

    auto returnType = FutureResultType(probe->RetType);
    if (!returnType) {
        returnType = probe->RetType;
    }

    auto scopeId = NSemantics::TScopeId{probe->Scope};
    TExprPtr probeBody = probe->Body;
    bool inferred = co_await InferGenericBindingsFromExplicitReturns(
        probeBody,
        returnType,
        genericParams,
        bindings,
        context,
        scopeId);

    if (!inferred && !probe->Body->Stmts.empty() && !TMaybeType<TVoidType>(returnType)) {
        inferred = co_await InferGenericBindingsFromReturnContext(
            probe->Body->Stmts.back(),
            returnType,
            genericParams,
            bindings,
            context,
            scopeId);
    }

    if (!inferred) {
        co_return TError(callLoc,
            "Не удалось вывести оставшиеся обобщённые параметры функции '"
            + genericDecl->Name + "' по телу функции.");
    }
    co_return true;
}

TGenericBindingsTask InferCompleteGenericBindings(
    const std::shared_ptr<TFunDecl>& genericDecl,
    const std::vector<TExprPtr>& args,
    NSemantics::TNameResolver& context,
    TLocation callLoc,
    bool retypeIntegerLiterals,
    TTypePtr expectedReturnType)
{
    auto genericParams = GenericParamSets(*genericDecl);
    TGenericBindings bindings;
    if (auto err = InferGenericBindings(
            *genericDecl,
            args,
            genericParams,
            bindings,
            retypeIntegerLiterals,
            expectedReturnType,
            false))
    {
        co_return TError(callLoc, *err);
    }

    if (!AllGenericBindingsPresent(genericDecl->GenericParams, bindings)) {
        co_await InferGenericBindingsFromReturnBody(
            genericDecl,
            genericParams,
            bindings,
            context,
            callLoc);
    }

    if (auto err = RequireAllGenericBindings(genericDecl->GenericParams, bindings)) {
        co_return TError(callLoc, *err);
    }
    co_return bindings;
}

// Clones+substitutes a generic TFunDecl for the concrete types inferred from
// `args`, registers the result under a synthetic mangled name and
// (re-)resolves+annotates its body. Caching by that name (via a root-scope
// symbol lookup - DeclareFunction inside ResolveInstantiatedFunDecl registers
// it there) both deduplicates repeat instantiations and guards against
// infinite recursion for mutually-recursive generics: the symbol is
// look-up-able before its body gets annotated, so a recursive call resolves
// to the already-registered (in-progress) clone instead of re-entering.
TFunDeclTask InstantiateGenericFunction(
    std::shared_ptr<TFunDecl> genericDecl,
    const std::vector<TExprPtr>& args,
    NSemantics::TNameResolver& context,
    TLocation callLoc,
    TTypePtr expectedReturnType)
{
    if (!genericDecl->Body) {
        co_return TError(callLoc, "External generic functions are not supported: " + genericDecl->Name);
    }

    auto genericParams = GenericParamSets(*genericDecl);
    auto bindings = co_await InferCompleteGenericBindings(
        genericDecl,
        args,
        context,
        callLoc,
        true,
        expectedReturnType);

    std::vector<TParam> params;
    params.reserve(genericDecl->Params.size());
    std::vector<TTypePtr> paramTypes;
    paramTypes.reserve(genericDecl->Params.size());
    for (auto& param : genericDecl->Params) {
        auto clonedParam = std::make_shared<TVarStmt>(*param);
        clonedParam->Type = SubstituteGenericType(param->Type, genericParams, bindings);
        for (auto& bound : clonedParam->Bounds) {
            bound.first = CloneAndSubstituteExpr(bound.first, genericParams, bindings);
            bound.second = CloneAndSubstituteExpr(bound.second, genericParams, bindings);
        }
        if (auto error = ValidateVectorTypes(clonedParam->Type, callLoc)) {
            co_return *error;
        }
        paramTypes.push_back(clonedParam->Type);
        params.push_back(std::move(clonedParam));
    }
    auto retType = SubstituteGenericType(genericDecl->RetType, genericParams, bindings);

    if (auto error = ValidateVectorTypes(retType, callLoc)) {
        co_return *error;
    }
    std::string mangledName = MangleGenericInstance(genericDecl->Name, retType, paramTypes);

    auto rootScopeId = context.GetOrCreateRootScope()->Id;
    if (auto found = context.Lookup(mangledName, rootScopeId)) {
        if (auto cached = TMaybeNode<TFunDecl>(context.GetSymbolNode(NSemantics::TSymbolId{found->Id})).Cast()) {
            co_return cached;
        }
    }

    auto body = TMaybeNode<TBlockExpr>(CloneAndSubstituteExpr(genericDecl->Body, genericParams, bindings)).Cast();

    auto cloned = std::make_shared<TFunDecl>(genericDecl->Location, mangledName, std::vector<TGenericParam>{}, std::move(params), body, retType);
    cloned->OriginalName = genericDecl->OriginalName.empty()
        ? genericDecl->Name
        : genericDecl->OriginalName;
    cloned->Inline = genericDecl->Inline;
    cloned->LastAssert = CloneAndSubstituteExpr(genericDecl->LastAssert, genericParams, bindings);
    cloned->Type = std::make_shared<TFunctionType>(std::move(paramTypes), cloned->RetType);

    auto declRes = context.ResolveInstantiatedFunDecl(cloned);
    if (!declRes) {
        co_return declRes.error();
    }

    // RetType as seen by `(return ...)` inside this function: stored on the
    // function-level scope so AnnotateReturn can find it from any nested
    // scope via TScope::FuncScope. Mirrors AnnotateFunDecl, which is not
    // called for instantiated clones (their body is annotated directly here).
    auto funcScope = context.GetScope(NSemantics::TScopeId{cloned->Scope});
    funcScope->RetType = FutureResultType(cloned->RetType);
    if (!funcScope->RetType) {
        funcScope->RetType = cloned->RetType;
    }

    co_await DoAnnotate(cloned->Body, context, NSemantics::TScopeId{cloned->Scope});
    if (!cloned->Body->Type) {
        co_return TError(callLoc, "Не удалось определить тип тела инстанциированной функции '" + genericDecl->Name + "'");
    }

    // Body must either yield the function's return value directly (no
    // trailing `return` - hand-written core lang) or be void-typed (every
    // path ends in an explicit `return`). Mirrors AnnotateFunDecl.
    if (!TMaybeType<TVoidType>(funcScope->RetType)) {
        auto bodyType = UnwrapReferenceType(cloned->Body->Type);
        if (!TMaybeType<TVoidType>(bodyType)) {
            if (!EqualTypes(bodyType, funcScope->RetType)) {
                if (!CanImplicit(bodyType, funcScope->RetType, &context)) {
                    co_return TError(cloned->Location,
                        "Тело функции должно возвращать значение типа '" + TypeDiagnosticName(funcScope->RetType) +
                        "', но имеет тип '" + TypeDiagnosticName(bodyType) + "'.");
                }
                cloned->Body->Stmts.back() = InsertImplicitCastIfNeeded(cloned->Body->Stmts.back(), funcScope->RetType, &context);
                cloned->Body->Type = funcScope->RetType;
            }
        }
    }

    co_return cloned;
}

std::optional<TTypePtr> SubstituteGenericOperatorReturnType(
    const std::shared_ptr<TFunDecl>& decl,
    const std::vector<TExprPtr>& args,
    NSemantics::TNameResolver& context,
    TTypePtr expectedReturnType,
    TLocation loc,
    TError& error)
{
    auto bindingsResult = InferCompleteGenericBindings(
        decl,
        args,
        context,
        loc,
        false,
        expectedReturnType).result();
    if (!bindingsResult) {
        error = TError(loc,
            "Не удалось вывести типы обобщённых параметров оператора '"
            + *decl->OperatorName + "': " + bindingsResult.error().what());
        return std::nullopt;
    }
    return SubstituteGenericType(decl->RetType, GenericParamSets(*decl), *bindingsResult);
}

TRegisteredOpTask InstantiateGenericOperator(
    const std::string& op,
    const std::vector<TExprPtr>& args,
    TTypePtr expectedReturnType,
    NSemantics::TNameResolver& context,
    TLocation callLoc)
{
    std::optional<TArgCost> bestCost;
    std::shared_ptr<TFunDecl> bestDecl;
    bool ambiguous = false;
    std::optional<TError> delayedError;

    for (const auto& candidate : context.LookupGenericOperatorDecls(op, args.size())) {
        if (!candidate || !candidate->OperatorName || !HasGenericParams(*candidate)) {
            continue;
        }
        TArgCost totalCost;
        bool viable = true;
        auto genericParams = GenericParamSets(*candidate);
        for (size_t i = 0; i < args.size(); ++i) {
            auto argType = UnwrapReferenceType(args[i]->Type);
            auto cost = ArgCost(argType, candidate->Params[i]->Type, &context, &genericParams.Types, &genericParams.Values);
            if (!cost) {
                viable = false;
                break;
            }
            totalCost = totalCost + *cost;
        }
        if (!viable) {
            continue;
        }
        TError error(callLoc, "");
        auto retType = SubstituteGenericOperatorReturnType(
            candidate,
            args,
            context,
            expectedReturnType,
            callLoc,
            error);
        if (!retType) {
            delayedError = std::move(error);
            continue;
        }
        if (expectedReturnType && !EqualTypes(*retType, expectedReturnType)) {
            continue;
        }
        if (!bestCost || totalCost < *bestCost) {
            bestCost = totalCost;
            bestDecl = candidate;
            ambiguous = false;
        } else if (totalCost == *bestCost) {
            ambiguous = true;
        }
    }

    if (!bestDecl) {
        if (delayedError) {
            co_return *delayedError;
        }
        co_return std::optional<NSemantics::TNameResolver::TRegisteredOp>{};
    }
    if (ambiguous) {
        co_return TError(callLoc, "ambiguous generic operator overload for '" + op + "'");
    }

    auto instantiated = co_await InstantiateGenericFunction(bestDecl, args, context, callLoc, expectedReturnType);
    if (expectedReturnType && !EqualTypes(instantiated->RetType, expectedReturnType)) {
        co_return std::optional<NSemantics::TNameResolver::TRegisteredOp>{};
    }
    co_return NSemantics::TNameResolver::TRegisteredOp{
        .SynthName = instantiated->Name,
        .ReturnType = instantiated->RetType,
    };
}

TTask TryGenericUnaryOp(
    const std::string& op,
    TExprPtr operand,
    NSemantics::TNameResolver& ctx,
    TLocation loc)
{
    if (auto m = co_await InstantiateGenericOperator(op, {operand}, nullptr, ctx, loc)) {
        co_return MakeModuleOpCall(m->SynthName, {operand}, m->ReturnType, loc, ctx);
    }
    co_return TExprPtr{};
}

TTask TryGenericBinaryOp(TExprPtr left, TExprPtr right,
    const TOperator& op, NSemantics::TNameResolver& ctx)
{
    const std::string opStr = op.ToString();
    auto lt = UnwrapReferenceType(left->Type);
    auto rt = UnwrapReferenceType(right->Type);

    if (auto m = co_await InstantiateGenericOperator(opStr, {left, right}, nullptr, ctx, left->Location)) {
        co_return MakeModuleOpCall(m->SynthName, {left, right}, m->ReturnType, left->Location, ctx);
    }

    auto castedLeft = InsertImplicitCastIfNeeded(left, rt, &ctx);
    if (castedLeft != left) {
        if (auto m = co_await InstantiateGenericOperator(opStr, {castedLeft, right}, nullptr, ctx, left->Location)) {
            co_return MakeModuleOpCall(m->SynthName, {castedLeft, right}, m->ReturnType, left->Location, ctx);
        }
    }

    auto castedRight = InsertImplicitCastIfNeeded(right, lt, &ctx);
    if (castedRight != right) {
        if (auto m = co_await InstantiateGenericOperator(opStr, {left, castedRight}, nullptr, ctx, left->Location)) {
            co_return MakeModuleOpCall(m->SynthName, {left, castedRight}, m->ReturnType, left->Location, ctx);
        }
    }

    co_return TExprPtr{};
}

TTask AnnotateOverloadedCall(
    std::shared_ptr<TCallExpr> call,
    const std::vector<NSemantics::TSymbolId>& overloads,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    for (auto& arg : call->Args) {
        arg = co_await DoAnnotate(arg, context, scopeId);
        if (!arg->Type) {
            co_return TError(arg->Location, "argument has no type");
        }
    }

    std::optional<TArgCost> bestCost;
    std::shared_ptr<TFunDecl> bestDecl;
    bool ambiguous = false;

    for (const auto& symId : overloads) {
        auto funDecl = TMaybeNode<TFunDecl>(context.GetSymbolNode(symId)).Cast();
        if (!funDecl || funDecl->Params.size() != call->Args.size()) {
            continue;
        }
        TArgCost totalCost;
        bool viable = true;
        auto genericParams = GenericParamSets(*funDecl);
        for (size_t i = 0; i < call->Args.size(); ++i) {
            auto cost = ArgCost(call->Args[i]->Type, funDecl->Params[i]->Type, &context, &genericParams.Types, &genericParams.Values);
            if (!cost) {
                viable = false;
                break;
            }
            totalCost = totalCost + *cost;
        }
        if (!viable) {
            continue;
        }
        if (HasGenericParams(*funDecl)) {
            TGenericBindings bindings;
            if (InferGenericBindings(
                    *funDecl,
                    call->Args,
                    genericParams,
                    bindings,
                    false,
                    nullptr,
                    false))
            {
                continue;
            }
        }
        if (!bestCost || totalCost < *bestCost) {
            bestCost = totalCost;
            bestDecl = funDecl;
            ambiguous = false;
        } else if (totalCost == *bestCost) {
            ambiguous = true;
        }
    }

    const auto callName = TMaybeNode<TIdentExpr>(call->Callee).Cast()->Name;
    const auto callSignature = FormatCallSignature(callName, call->Args);
    if (!bestDecl) {
        co_return TError(call->Location, "no matching overload for call " + callSignature);
    }
    if (ambiguous) {
        co_return TError(call->Location, "ambiguous overload for call " + callSignature);
    }

    if (HasGenericParams(*bestDecl)) {
        auto instantiated = co_await InstantiateGenericFunction(bestDecl, call->Args, context, call->Location);
        bestDecl = instantiated;
    }

    for (size_t i = 0; i < call->Args.size(); ++i) {
        call->Args[i] = InsertImplicitCastIfNeeded(call->Args[i], bestDecl->Params[i]->Type, &context);
    }

    auto callee = std::make_shared<TIdentExpr>(call->Callee->Location, bestDecl->Name);
    callee->Type = bestDecl->Type;
    call->Callee = callee;
    call->Type = bestDecl->RetType;

    if (bestDecl->InlineFactory) {
        auto inlined = (*bestDecl->InlineFactory)(call->Args);
        if (inlined) {
            co_return co_await DoAnnotate(inlined, context, scopeId);
        }
    }

    co_return call;
}

TTask AnnotateVectorIntrinsic(
    std::shared_ptr<TCallExpr> call,
    std::string_view op,
    NSemantics::TNameResolver& context,
    NSemantics::TScopeId scopeId)
{
    const size_t count = op == "shuffle"
        ? 2
        : 1;
    if (call->Args.size() != count) {
        co_return TError(call->Location, std::string(op) + ": неверное число аргументов.");
    }
    std::vector<TTypePtr> argTypes;
    std::vector<TParam> params;
    for (auto& arg : call->Args) {
        arg = co_await DoAnnotate(arg, context, scopeId);
        auto type = UnwrapNamedType(UnwrapReferenceType(arg->Type));
        argTypes.push_back(type);
        params.push_back(std::make_shared<TVarStmt>(arg->Location, "$arg" + std::to_string(params.size()), type));
    }
    auto vector = TMaybeType<TVectorType>(argTypes[0]).Cast();
    if (!vector) {
        co_return TError(call->Location, std::string(op) + ": ожидается вектор.");
    }
    TTypePtr result = std::make_shared<TBoolType>();
    if (op == "shuffle") {
        auto mask = TMaybeNode<TVectorExpr>(call->Args[1]).Cast();
        if (!mask || mask->Elements.size() != static_cast<size_t>(vector->Size)) {
            co_return TError(call->Location, "shuffle: маска должна быть векторным литералом той же длины.");
        }
        for (const auto& element : mask->Elements) {
            auto number = TMaybeNode<TNumberExpr>(element).Cast();
            if (!number || number->IsFloat() || number->IntValue < 0 || number->IntValue >= vector->Size) {
                co_return TError(element->Location, "shuffle: индекс должен быть целой константой в пределах вектора.");
            }
        }
        result = argTypes[0];
    } else if (!TMaybeType<TBoolType>(UnwrapNamedType(vector->ElementType))) {
        co_return TError(call->Location, std::string(op) + ": ожидается вектор bool.");
    }

    const std::string builtin = "builtin::" + std::string(op);
    const std::string name = MangleGenericInstance(builtin, result, argTypes);
    const auto root = context.GetOrCreateRootScope()->Id;
    if (!context.Lookup(name, root)) {
        // Typed declarations keep ordinary call validation and lifetime handling.
        auto function = std::make_shared<TFunDecl>(call->Location, name, std::vector<TGenericParam>{},
            std::move(params), nullptr, result);
        function->MangledName = builtin;
        function->Type = std::make_shared<TFunctionType>(argTypes, result);
        context.DeclareFunction(name, function);
    }
    call->Callee = std::make_shared<TIdentExpr>(call->Callee->Location, name);
    call->Callee->Type = std::make_shared<TFunctionType>(std::move(argTypes), result);
    call->Type = result;
    co_return call;
}

TTask AnnotateCall(std::shared_ptr<TCallExpr> call, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    if (auto maybeIdent = TMaybeNode<TIdentExpr>(call->Callee)) {
        auto op = NSemantics::VectorIntrinsic(maybeIdent.Cast()->Name);
        if (!op.empty() && !context.Lookup(maybeIdent.Cast()->Name, scopeId)) {
            co_return co_await AnnotateVectorIntrinsic(call, op, context, scopeId);
        }
        if (auto symbol = context.Lookup(maybeIdent.Cast()->Name, scopeId)) {
            auto function = TMaybeNode<TFunDecl>(context.GetSymbolNode(NSemantics::TSymbolId{symbol->Id})).Cast();
            if (function && !NSemantics::VectorIntrinsic(function->MangledName).empty()) {
                co_return co_await AnnotateVectorIntrinsic(call,
                    NSemantics::VectorIntrinsic(function->MangledName), context, scopeId);
            }
        }
        auto overloads = context.LookupOverloads(maybeIdent.Cast()->Name, scopeId);
        if (overloads.size() > 1) {
            co_return co_await AnnotateOverloadedCall(call, overloads, context, scopeId);
        }
        // The single candidate has generic parameters - instantiate it
        // for the call's argument types and re-target the callee at the
        // synthesized clone, registered in the root scope under a mangled
        // name that ordinary identifier resolution can find.
        if (overloads.size() == 1) {
            auto genericDecl = TMaybeNode<TFunDecl>(context.GetSymbolNode(overloads[0])).Cast();
            if (genericDecl && HasGenericParams(*genericDecl)) {
                for (auto& arg : call->Args) {
                    arg = co_await DoAnnotate(arg, context, scopeId);
                    if (!arg->Type) {
                        co_return TError(arg->Location, "argument has no type");
                    }
                }
                if (genericDecl->Params.size() != call->Args.size()) {
                    co_return TError(call->Location, "Неверное количество аргументов при вызове функции '" + genericDecl->Name +
                        "': ожидается " + std::to_string(genericDecl->Params.size()) + ", передано " + std::to_string(call->Args.size()) + ".");
                }
                auto instantiated = co_await InstantiateGenericFunction(genericDecl, call->Args, context, call->Location);
                call->Callee = std::make_shared<TIdentExpr>(call->Callee->Location, instantiated->Name);
            }
        }
    }

    call->Callee = co_await DoAnnotate(call->Callee, context, scopeId);
    if (!call->Callee->Type) {
        co_return TError(call->Location, "Нельзя вызвать выражение без типа (функция или процедура не определена или не имеет типа).");
    }
    auto maybeFunType = TMaybeType<TFunctionType>(call->Callee->Type);
    if (maybeFunType) {
        auto funT = maybeFunType.Cast();
        if (funT->ParamTypes.size() != call->Args.size()) {
            auto callee = TMaybeNode<TIdentExpr>(call->Callee);
            std::string message = "Неверное количество аргументов при вызове "
                + (callee ? "`" + callee.Cast()->Name + "'" : std::string("функции"))
                + ": ожидается " + std::to_string(funT->ParamTypes.size()) + ", передано " + std::to_string(call->Args.size()) + ".";
            // "вправо(60)" with Робот means the Черепаха command.
            if (callee) {
                for (const auto& module : context.ModulesWithFunction(callee.Cast()->Name, call->Args.size())) {
                    message += "\n `" + callee.Cast()->Name + "' с таким числом аргументов есть в модуле `" + module + "'.";
                }
            }
            co_return TError(call->Location, message);
        }
        for (size_t i = 0; i < call->Args.size(); ++i) {
            auto& paramT = funT->ParamTypes[i];
            auto maybeRef = TMaybeType<TReferenceType>(paramT);

            auto& arg = call->Args[i];
            auto maybeIdent = TMaybeNode<TIdentExpr>(arg);
            // TODO: support array cells as ref arguments?
            if (maybeIdent) {
                arg = co_await AnnotateIdent(maybeIdent.Cast(), context, scopeId, /* path-through = */ true);
            } else {
                arg = co_await DoAnnotate(arg, context, scopeId);
            }
            if (!arg->Type) {
                co_return TError(arg->Location, "Аргумент #" + std::to_string(i+1) + " не имеет типа. Проверьте корректность выражения.");
            }

            // if ParamT is reference type, arg must be of the same referenced type or ident of that underlying type
            if (maybeRef) {
                auto paramRefT = maybeRef.Cast();
                if (maybeIdent) {
                    auto identType = UnwrapReferenceType(maybeIdent.Cast()->Type);
                    // ident must be writable
                    if (maybeIdent && !identType->Mutable) {
                        co_return TError(arg->Location,
                            "Аргумент #" + std::to_string(i+1) + ": нельзя передать этот идентификатор в ссылочный (рез/арг рез) параметр, так как он не является изменяемым (immutable).\n"
                            "\nПричина: вы передаёте, например, аргумент функции, объявленный как обычный, в параметр, объявленный как 'рез' или 'арг рез'.\n"
                            "\nРешения:\n"
                            "- Объявите параметр исходной функции как 'арг рез', если хотите передавать его по ссылке и изменять.\n"
                            "- Или скопируйте значение в отдельную переменную и передайте её.\n"
                            "\nПример:\n"
                            "алг ф(цел i)\n  g(i)  // ошибка: i нельзя передать как 'рез'\n\nалг g(рез i)\n  ...\n");
                    }
                }
                auto argTypeUnwrapped = UnwrapReferenceType(arg->Type);
                if (!EqualTypes(argTypeUnwrapped, paramRefT->ReferencedType)) {
                    co_return TError(arg->Location, "Аргумент #" + std::to_string(i+1) + ": тип '" + TypeDiagnosticName(arg->Type) + "' не совпадает с типом ссылочного параметра '" + TypeDiagnosticName(paramT) + "'.");
                }
                // no implicit cast for reference parameters
                continue;
            }
            RetypeIntegerLiteralIfFits(arg, paramT);
            if (!EqualTypes(arg->Type, paramT)) {
                if (!CanImplicit(UnwrapReferenceType(arg->Type), paramT, &context)) {
                    co_return TError(arg->Location, "Аргумент #" + std::to_string(i+1) + ": нельзя неявно преобразовать тип '" + TypeDiagnosticName(arg->Type) + "' к типу параметра '" + TypeDiagnosticName(paramT) + "'.");
                }
                arg = InsertImplicitCastIfNeeded(arg, paramT, &context);
            }
        }
        call->Type = maybeFunType.Cast()->ReturnType;

    } else {
        call->Type = call->Callee->Type;
    }
    co_return call;
}

TTask AnnotateAwait(std::shared_ptr<TAwaitExpr> awaitExpr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    awaitExpr->Operand = co_await DoAnnotate(awaitExpr->Operand, context, scopeId);
    if (!awaitExpr->Operand->Type) {
        co_return TError(awaitExpr->Location, "Нельзя ожидать выражение без типа.");
    }

    auto resultType = FutureResultType(awaitExpr->Operand->Type);
    if (!resultType) {
        co_return TError(awaitExpr->Location, "Нельзя ожидать результат выражения, которое не возвращает Future.");
    }

    awaitExpr->Type = resultType;
    co_return awaitExpr;
}

// Type of an `если' branch as it will be AFTER the implied `await'.
//
// Executor commands (Робот, Черепаха, Чертёжник, Рисователь) are declared as
// future<void> - they suspend the coroutine. The `await' around them is added
// by CoroutineAnnotationTransform, which runs AFTER the first type annotation
// pass, so on that first pass a `то' branch ending in an executor command is
// seen here as future<void>. It is still a statement, not a value, hence the
// unwrap when deciding "statement or value context".
//
// Not used for the common-type computation below: there a future is meaningful
// on its own, and unwrapping it would make `если c то <future<цел>> иначе <цел>
// все' insert a bogus cast.
TTypePtr SettledBranchType(const TTypePtr& type) {
    auto unwrapped = UnwrapReferenceType(type);
    if (auto awaited = FutureResultType(unwrapped)) {
        return awaited;
    }
    return unwrapped;
}

TTask AnnotateIfExpr(std::shared_ptr<TIfExpr> ifExpr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    ifExpr->Cond = co_await DoAnnotate(ifExpr->Cond, context, scopeId);
    if (!ifExpr->Cond->Type) {
        co_return TError(ifExpr->Cond->Location, "Условие в `если' не имеет типа. Проверьте корректность выражения в условии.");
    }
    auto boolT = std::make_shared<TBoolType>();
    auto condType = UnwrapReferenceType(ifExpr->Cond->Type);
    if (!EqualTypes(condType, boolT)) {
        if (!CanImplicit(condType, boolT, &context)) {
            co_return TError(ifExpr->Cond->Location, "Условие в `если' должно иметь логический тип или приводиться к нему.");
        }
        ifExpr->Cond = InsertImplicitCastIfNeeded(ifExpr->Cond, boolT, &context);
    }

    ifExpr->Then = co_await DoAnnotate(ifExpr->Then, context, scopeId);
    if (!ifExpr->Then->Type) {
        co_return TError(ifExpr->Then->Location, "Ветвь `то' в `если' не имеет типа.");
    }

    if (!ifExpr->Else) {
        // No else branch: then must be void, overall type is void
        auto thenType = SettledBranchType(ifExpr->Then->Type);
        if (!TMaybeType<TVoidType>(thenType)) {
            co_return TError(ifExpr->Then->Location, "if-expression без ветви `иначе': ветвь `то' должна иметь тип void.");
        }
        ifExpr->Type = std::make_shared<TVoidType>();
        co_return ifExpr;
    }

    ifExpr->Else = co_await DoAnnotate(ifExpr->Else, context, scopeId);
    if (!ifExpr->Else->Type) {
        co_return TError(ifExpr->Else->Location, "Ветвь `иначе' в `если' не имеет типа.");
    }

    auto thenType = UnwrapReferenceType(ifExpr->Then->Type);
    auto elseType = UnwrapReferenceType(ifExpr->Else->Type);

    // Both branches void -> statement context
    if (TMaybeType<TVoidType>(SettledBranchType(thenType))
        && TMaybeType<TVoidType>(SettledBranchType(elseType))) {
        ifExpr->Type = std::make_shared<TVoidType>();
        co_return ifExpr;
    }

    // Expression context: both branches must yield a compatible non-void type
    if (TMaybeType<TVoidType>(thenType) || TMaybeType<TVoidType>(elseType)) {
        co_return TError(ifExpr->Location, "if-expression: ветви должны иметь одинаковый не-void тип.");
    }
    auto common = CommonValueType(thenType, elseType, context);
    if (!common) {
        co_return TError(ifExpr->Location,
            "if-expression: типы ветвей несовместимы: '" + TypeDiagnosticName(thenType) +
            "' и '" + TypeDiagnosticName(elseType) + "'.");
    }
    ifExpr->Then = InsertImplicitCastIfNeeded(ifExpr->Then, common, &context);
    ifExpr->Else = InsertImplicitCastIfNeeded(ifExpr->Else, common, &context);
    ifExpr->Type = common;
    co_return ifExpr;
}

TTask AnnotateWhile(std::shared_ptr<TWhileStmtExpr> loop, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    loop->Cond = co_await DoAnnotate(loop->Cond, context, scopeId);
    loop->Cond = InsertImplicitCastIfNeeded(loop->Cond, std::make_shared<TBoolType>(), &context);
    loop->Body = co_await DoAnnotate(loop->Body, context, scopeId);
    loop->Type = std::make_shared<TVoidType>();
    co_return loop;
}

TTask AnnotateRepeat(std::shared_ptr<TRepeatStmtExpr> loop, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    loop->Body = co_await DoAnnotate(loop->Body, context, scopeId);
    loop->Cond = co_await DoAnnotate(loop->Cond, context, scopeId);
    loop->Cond = InsertImplicitCastIfNeeded(loop->Cond, std::make_shared<TBoolType>(), &context);
    loop->Type = std::make_shared<TVoidType>();
    co_return loop;
}

TTask AnnotateFor(std::shared_ptr<TForStmtExpr> loop, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    auto intType = std::make_shared<TIntegerType>();

    auto symbolId = context.Lookup(loop->VarName, scopeId);
    if (!symbolId) {
        co_return TError(loop->Location, "Переменная цикла не определена: " + loop->VarName);
    }
    auto symbol = context.GetSymbolNode(NSemantics::TSymbolId{symbolId->Id});
    if (!symbol || !TMaybeType<TIntegerType>(symbol->Type)) {
        co_return TError(loop->Location, "Переменная цикла должна иметь целый тип: " + loop->VarName);
    }

    loop->From = co_await DoAnnotate(loop->From, context, scopeId);
    loop->From = InsertImplicitCastIfNeeded(loop->From, intType, &context);
    loop->To = co_await DoAnnotate(loop->To, context, scopeId);
    loop->To = InsertImplicitCastIfNeeded(loop->To, intType, &context);
    if (loop->Step) {
        loop->Step = co_await DoAnnotate(loop->Step, context, scopeId);
        loop->Step = InsertImplicitCastIfNeeded(loop->Step, intType, &context);
    }
    loop->Body = co_await DoAnnotate(loop->Body, context, scopeId);
    loop->Type = std::make_shared<TVoidType>();
    co_return loop;
}

TTask AnnotateTimes(std::shared_ptr<TTimesStmtExpr> loop, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    loop->Count = co_await DoAnnotate(loop->Count, context, scopeId);
    loop->Count = InsertImplicitCastIfNeeded(loop->Count, std::make_shared<TIntegerType>(), &context);
    loop->Body = co_await DoAnnotate(loop->Body, context, scopeId);
    loop->Type = std::make_shared<TVoidType>();
    co_return loop;
}

TTask AnnotateMultiIndex(std::shared_ptr<TMultiIndexExpr> indexExpr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    indexExpr->Collection = co_await DoAnnotate(indexExpr->Collection, context, scopeId);
    if (!indexExpr->Collection->Type) {
        co_return TError(indexExpr->Location, "В выражении многомерного индекса не удалось определить тип коллекции (массив). Проверьте корректность выражения.");
    }
    auto maybeArrayType = TMaybeType<TArrayType>(indexExpr->Collection->Type);
    if (!maybeArrayType) {
        co_return TError(indexExpr->Location, "Многомерная индексация поддерживается только для массивов. Проверьте, что вы обращаетесь к массиву, а не к другому типу.");
    }

    auto intType = std::make_shared<TIntegerType>();
    for (size_t i = 0; i < indexExpr->Indices.size(); ++i) {
        auto& index = indexExpr->Indices[i];
        index = co_await DoAnnotate(index, context, scopeId);
        if (!index->Type) {
            co_return TError(index->Location, "Индекс #" + std::to_string(i+1) + " в многомерном обращении к массиву не имеет типа. Проверьте выражение индекса.");
        }
        if (!EqualTypes(index->Type, intType)) {
            if (!CanImplicit(index->Type, intType, &context)) {
                co_return TError(index->Location, "Индекс #" + std::to_string(i+1) + " в многомерном обращении к массиву должен быть целым числом.");
            }
            index = InsertImplicitCastIfNeeded(index, intType, &context);
        }
    }
    auto arrayType = maybeArrayType.Cast();
    indexExpr->Type = arrayType->ElementType;

    co_return indexExpr;
}

TTask AnnotateIndex(std::shared_ptr<TIndexExpr> indexExpr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    indexExpr->Collection = co_await DoAnnotate(indexExpr->Collection, context, scopeId);
    if (!indexExpr->Collection->Type) {
        co_return TError(indexExpr->Location, "В выражении индексации не удалось определить тип коллекции (массива или строки). Проверьте корректность выражения.");
    }
    indexExpr->Index = co_await DoAnnotate(indexExpr->Index, context, scopeId);
    if (!indexExpr->Index->Type) {
        co_return TError(indexExpr->Location, "Индекс в выражении индексации не имеет типа. Убедитесь, что выражение индекса корректно и его тип определён.");
    }
    auto intType = std::make_shared<TIntegerType>();
    if (!EqualTypes(indexExpr->Index->Type, intType)) {
        if (!CanImplicit(indexExpr->Index->Type, intType, &context)) {
            co_return TError(indexExpr->Location, "Индекс в выражении индексации должен быть целым числом. Например: a[2], s[1].");
        }
        indexExpr->Index = InsertImplicitCastIfNeeded(indexExpr->Index, intType, &context);
    }
    auto collectionType = UnwrapNamedType(UnwrapReferenceType(indexExpr->Collection->Type));
    if (TMaybeType<TStringType>(collectionType)) {
        indexExpr->Type = std::make_shared<TSymbolType>();
    } else if (auto maybeArrayType = TMaybeType<TArrayType>(collectionType)) {
        auto arrayType = maybeArrayType.Cast();
        indexExpr->Type = arrayType->ElementType;
    } else if (auto maybePointerType = TMaybeType<TPointerType>(collectionType)) {
        indexExpr->Type = maybePointerType.Cast()->PointeeType;
    } else if (auto vector = TMaybeType<TVectorType>(collectionType)) {
        indexExpr->Type = vector.Cast()->ElementType;
    } else {
        co_return TError(indexExpr->Location, "Индексация поддерживается только для массивов, строк, указателей и векторов.\n"
            "Пример корректной индексации массива: a[2] (где a — массив).\n"
            "Пример корректной индексации строки: s[1] (где s — строка).\n"
            "Проверьте, что вы обращаетесь к массиву, строке или указателю, а не к другому типу.");
    }

    co_return indexExpr;
}

TTask AnnotateSlice(std::shared_ptr<TSliceExpr> sliceExpr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    sliceExpr->Collection = co_await DoAnnotate(sliceExpr->Collection, context, scopeId);
    if (!sliceExpr->Collection->Type) {
        co_return TError(sliceExpr->Location, "В выражении среза не удалось определить тип коллекции. Проверьте корректность выражения.");
    }
    auto collectionType = UnwrapReferenceType(sliceExpr->Collection->Type);
    if (!TMaybeType<TStringType>(collectionType)) {
        co_return TError(sliceExpr->Location, "Срезы поддерживаются только для строк.\n"
            "Пример корректного среза: s[1:3] (где s — строка).\n"
            "Проверьте, что вы используете строку, а не другой тип.");
    }
    sliceExpr->Start = co_await DoAnnotate(sliceExpr->Start, context, scopeId);
    if (!sliceExpr->Start->Type) {
        co_return TError(sliceExpr->Location, "Начальный индекс в срезе не имеет типа. Убедитесь, что выражение индекса корректно и его тип определён.");
    }
    auto intType = std::make_shared<TIntegerType>();
    if (!EqualTypes(sliceExpr->Start->Type, intType)) {
        if (!CanImplicit(sliceExpr->Start->Type, intType, &context)) {
            co_return TError(sliceExpr->Location, "Начальный индекс в срезе должен быть целым числом. Пример: s[1:3].");
        }
        sliceExpr->Start = InsertImplicitCastIfNeeded(sliceExpr->Start, intType, &context);
    }
    if (!sliceExpr->End) {
        // (slice collection [start]) without an end bound is a single-element slice.
        sliceExpr->End = ShallowCloneNode(sliceExpr->Start);
    } else {
        sliceExpr->End = co_await DoAnnotate(sliceExpr->End, context, scopeId);
        if (!sliceExpr->End->Type) {
            co_return TError(sliceExpr->Location, "Конечный индекс в срезе не имеет типа. Убедитесь, что выражение индекса корректно и его тип определён.");
        }
        if (!EqualTypes(sliceExpr->End->Type, intType)) {
            if (!CanImplicit(sliceExpr->End->Type, intType, &context)) {
                co_return TError(sliceExpr->Location, "Конечный индекс в срезе должен быть целым числом. Пример: s[1:3].");
            }
            sliceExpr->End = InsertImplicitCastIfNeeded(sliceExpr->End, intType, &context);
        }
    }
    // Срез строки возвращает строку
    sliceExpr->Type = collectionType;

    co_return sliceExpr;
}

TTask AnnotateFieldAccess(std::shared_ptr<TFieldAccessExpr> fieldAccess, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    fieldAccess->Object = co_await DoAnnotate(fieldAccess->Object, context, scopeId);
    if (!fieldAccess->Object->Type) {
        co_return TError(fieldAccess->Location, "Не удалось определить тип объекта в выражении обращения к полю '" + fieldAccess->FieldName + "'.");
    }

    auto objType = UnwrapNamedType(UnwrapReferenceType(fieldAccess->Object->Type));
    auto maybeStruct = TMaybeType<TStructType>(objType);
    if (!maybeStruct) {
        co_return TError(fieldAccess->Location,
            "Обращение к полю '" + fieldAccess->FieldName + "' невозможно: объект не является структурой (тип: " + TypeDiagnosticName(fieldAccess->Object->Type) + ").");
    }
    auto structType = maybeStruct.Cast();

    const auto& fields = structType->Fields;
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        if (fields[i].first == fieldAccess->FieldName) {
            fieldAccess->FieldIndex = i;
            fieldAccess->Type = fields[i].second;
            co_return fieldAccess;
        }
    }

    std::string available;
    for (const auto& [name, _] : fields) {
        if (!available.empty()) available += ", ";
        available += name;
    }
    co_return TError(fieldAccess->Location,
        "Поле '" + fieldAccess->FieldName + "' не найдено в структуре. Доступные поля: " + available + ".");
}

static std::pair<std::shared_ptr<TStructType>, std::string>
ResolveStructField(TTypePtr objType, const std::string& fieldName, int& outIndex) {
    auto structType = TMaybeType<TStructType>(UnwrapNamedType(UnwrapReferenceType(objType))).Cast();
    if (!structType) return {nullptr, ""};
    const auto& fields = structType->Fields;
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        if (fields[i].first == fieldName) {
            outIndex = i;
            return {structType, ""};
        }
    }
    std::string available;
    for (const auto& [name, _] : fields) {
        if (!available.empty()) available += ", ";
        available += name;
    }
    return {nullptr, available};
}

TTask AnnotateFieldAssign(std::shared_ptr<TFieldAssignExpr> fieldAssign, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    fieldAssign->Object = co_await DoAnnotate(fieldAssign->Object, context, scopeId);
    if (!fieldAssign->Object->Type) {
        co_return TError(fieldAssign->Location, "Не удалось определить тип объекта при присваивании полю '" + fieldAssign->FieldName + "'.");
    }

    int fieldIdx = -1;
    auto [structType, available] = ResolveStructField(fieldAssign->Object->Type, fieldAssign->FieldName, fieldIdx);
    if (!structType) {
        if (available.empty()) {
            co_return TError(fieldAssign->Location,
                "Присваивание полю '" + fieldAssign->FieldName + "' невозможно: объект не является структурой.");
        }
        co_return TError(fieldAssign->Location,
            "Поле '" + fieldAssign->FieldName + "' не найдено в структуре. Доступные поля: " + available + ".");
    }
    fieldAssign->FieldIndex = fieldIdx;
    auto fieldType = structType->Fields[fieldIdx].second;

    fieldAssign->Value = co_await DoAnnotate(fieldAssign->Value, context, scopeId);
    if (!fieldAssign->Value->Type) {
        co_return TError(fieldAssign->Location, "Не удалось определить тип значения при присваивании полю '" + fieldAssign->FieldName + "'.");
    }
    auto valueType = UnwrapReferenceType(fieldAssign->Value->Type);
    if (!EqualTypes(valueType, fieldType)) {
        if (!CanImplicit(valueType, fieldType, &context)) {
            co_return TError(fieldAssign->Location,
                "Нельзя неявно преобразовать тип '" + TypeDiagnosticName(valueType) +
                "' к типу поля '" + fieldAssign->FieldName + "' (" + TypeDiagnosticName(fieldType) + ").");
        }
        fieldAssign->Value = InsertImplicitCastIfNeeded(fieldAssign->Value, fieldType, &context);
    }

    fieldAssign->Type = std::make_shared<TVoidType>();
    co_return fieldAssign;
}

TTask DoAnnotate(TExprPtr expr, NSemantics::TNameResolver& context, NSemantics::TScopeId scopeId) {
    if (auto error = ValidateVectorTypes(expr->Type, expr->Location)) {
        co_return *error;
    }
    if (auto maybeBinary = TMaybeNode<TBinaryExpr>(expr)) {
        co_return co_await AnnotateBinary(maybeBinary.Cast(), context, scopeId);
    } else if (auto maybeNum = TMaybeNode<TNumberExpr>(expr)) {
        co_return AnnotateNumber(maybeNum.Cast());
    } else if (auto vector = TMaybeNode<TVectorExpr>(expr)) {
        co_return co_await AnnotateVector(vector.Cast(), context, scopeId);
    } else if (auto maybeUnary = TMaybeNode<TUnaryExpr>(expr)) {
        co_return co_await AnnotateUnary(maybeUnary.Cast(), context, scopeId);
    } else if (auto maybeCast = TMaybeNode<TCastExpr>(expr)) {
        co_return co_await AnnotateCast(maybeCast.Cast(), context, scopeId);
    } else if (auto maybeStruct = TMaybeNode<TStructConstructExpr>(expr)) {
        co_return co_await AnnotateStructConstruct(maybeStruct.Cast(), context, scopeId);
    } else if (auto maybeBlock = TMaybeNode<TBlockExpr>(expr)) {
        co_return co_await AnnotateBlock(maybeBlock.Cast(), context, scopeId);
    } else if (auto maybeIdent = TMaybeNode<TIdentExpr>(expr)) {
        co_return co_await AnnotateIdent(maybeIdent.Cast(), context, scopeId);
    } else if (auto maybeAssign = TMaybeNode<TAssignExpr>(expr)) {
        co_return co_await AnnotateAssign(maybeAssign.Cast(), context, scopeId);
    } else if (auto maybeArrayAssign = TMaybeNode<TArrayAssignExpr>(expr)) {
        co_return co_await AnnotateArrayAssign(maybeArrayAssign.Cast(), context, scopeId);
    } else if (auto maybeMultiIndex = TMaybeNode<TMultiIndexExpr>(expr)) {
        co_return co_await AnnotateMultiIndex(maybeMultiIndex.Cast(), context, scopeId);
    } else if (auto maybeVar = TMaybeNode<TVarStmt>(expr)) {
        co_return co_await AnnotateVar(maybeVar.Cast(), context, scopeId);
    } else if (auto maybeFunDecl = TMaybeNode<TFunDecl>(expr)) {
        co_return co_await AnnotateFunDecl(maybeFunDecl.Cast(), context, scopeId);
    } else if (auto maybeCall = TMaybeNode<TCallExpr>(expr)) {
        co_return co_await AnnotateCall(maybeCall.Cast(), context, scopeId);
    } else if (auto maybeAwait = TMaybeNode<TAwaitExpr>(expr)) {
        co_return co_await AnnotateAwait(maybeAwait.Cast(), context, scopeId);
    } else if (auto maybeIf = TMaybeNode<TIfExpr>(expr)) {
        co_return co_await AnnotateIfExpr(maybeIf.Cast(), context, scopeId);
    } else if (auto maybeIndex = TMaybeNode<TIndexExpr>(expr)) {
        co_return co_await AnnotateIndex(maybeIndex.Cast(), context, scopeId);
    } else if (auto maybeSlice = TMaybeNode<TSliceExpr>(expr)) {
        co_return co_await AnnotateSlice(maybeSlice.Cast(), context, scopeId);
    } else if (auto maybeFieldAccess = TMaybeNode<TFieldAccessExpr>(expr)) {
        co_return co_await AnnotateFieldAccess(maybeFieldAccess.Cast(), context, scopeId);
    } else if (auto maybeFieldAssign = TMaybeNode<TFieldAssignExpr>(expr)) {
        co_return co_await AnnotateFieldAssign(maybeFieldAssign.Cast(), context, scopeId);
    } else if (TMaybeNode<TUseExpr>(expr)) {
        expr->Type = std::make_shared<TVoidType>();
        co_return expr;
    } else if (TMaybeNode<TBreakStmt>(expr)) {
        expr->Type = std::make_shared<TVoidType>();
        co_return expr;
    } else if (TMaybeNode<TContinueStmt>(expr)) {
        expr->Type = std::make_shared<TVoidType>();
        co_return expr;
    } else if (auto maybeReturn = TMaybeNode<TReturnExpr>(expr)) {
        co_return co_await AnnotateReturn(maybeReturn.Cast(), context, scopeId);
    } else if (auto output = TMaybeNode<TOutputExpr>(expr)) {
        for (auto* child : output.Cast()->MutableChildren()) {
            if (*child) {
                *child = co_await DoAnnotate(*child, context, scopeId);
            }
        }
        for (const auto& arg : output.Cast()->Args) {
            auto type = UnwrapReferenceType(arg.Expr->Type);
            if ((TMaybeType<TVectorType>(UnwrapNamedType(type)) || TMaybeType<TNamedType>(type))
                && !context.GetUnaryOp("print", type))
            {
                if (auto printer = co_await InstantiateGenericOperator(
                        "print", {arg.Expr}, std::make_shared<TVoidType>(), context, arg.Expr->Location))
                {
                    context.RegisterUnaryOp("print", type, *printer);
                }
            }
        }
        co_return output.Cast();
    } else if (auto retain = TMaybeNode<TRetainExpr>(expr)) {
        co_return co_await AnnotateLifetimeValue(retain.Cast(), context, scopeId);
    } else if (auto ownLiteral = TMaybeNode<TOwnLiteralExpr>(expr)) {
        co_return co_await AnnotateLifetimeValue(ownLiteral.Cast(), context, scopeId);
    } else if (auto move = TMaybeNode<TMoveExpr>(expr)) {
        co_return co_await AnnotateLifetimeValue(move.Cast(), context, scopeId);
    } else if (auto borrow = TMaybeNode<TBorrowExpr>(expr)) {
        co_return co_await AnnotateLifetimeValue(borrow.Cast(), context, scopeId);
    } else if (auto destroy = TMaybeNode<TDestroyExpr>(expr)) {
        co_return co_await AnnotateDestroy(destroy.Cast(), context, scopeId);
    } else if (auto replace = TMaybeNode<TReplaceExpr>(expr)) {
        co_return co_await AnnotateReplace(replace.Cast(), context, scopeId);
    } else if (auto exit = TMaybeNode<TCleanupExitExpr>(expr)) {
        co_return co_await AnnotateCleanupExit(exit.Cast(), context, scopeId);
    } else if (auto cleanup = TMaybeNode<TGlobalCleanupExpr>(expr)) {
        co_return co_await AnnotateGlobalCleanup(cleanup.Cast(), context, scopeId);
    } else if (auto maybeLoop = TMaybeNode<TWhileStmtExpr>(expr)) {
        co_return co_await AnnotateWhile(maybeLoop.Cast(), context, scopeId);
    } else if (auto maybeLoop = TMaybeNode<TRepeatStmtExpr>(expr)) {
        co_return co_await AnnotateRepeat(maybeLoop.Cast(), context, scopeId);
    } else if (auto maybeLoop = TMaybeNode<TForStmtExpr>(expr)) {
        co_return co_await AnnotateFor(maybeLoop.Cast(), context, scopeId);
    } else if (auto maybeLoop = TMaybeNode<TTimesStmtExpr>(expr)) {
        co_return co_await AnnotateTimes(maybeLoop.Cast(), context, scopeId);
    } else {
        if (!expr->Type) {
            // if expr->Type => node was annotated on construction
            co_return TError(expr->Location,
                "Не удалось определить тип выражения для аннотации типов: '" + std::string(expr->NodeName()) + "'.\n"
                "Возможно, это неизвестный или некорректный вид выражения.\n"
                "Проверьте корректность синтаксиса и структуры кода.\n"
                "\nПример: выражение типа ??? не поддерживается.\n"
                "Если вы считаете, что это ошибка компилятора, пожалуйста, сообщите о ней с примером кода.");
        }

        for (auto* child : expr->MutableChildren()) {
            if (*child) {
                *child = co_await DoAnnotate(*child, context, scopeId);
            }
        }
    }
    co_return expr;
}

} // namespace

TTypeAnnotator::TTypeAnnotator(NSemantics::TNameResolver& context)
    : Context(context)
{}

std::expected<TExprPtr, TError> TTypeAnnotator::Annotate(TExprPtr expr)
{
    auto result = DoAnnotate(expr, Context, NSemantics::TScopeId{0}).result();
    if (!result) {
        return result;
    }
    // Append monomorphized generic-function clones to the top-level block so
    // lowering compiles them exactly like ordinary top-level functions - see
    // GetGenericInstantiations' comment for why. Drained (not just read):
    // Annotate runs repeatedly during the source fixpoint and once after final
    // AST rewrites, so a non-draining read would re-splice already-spliced
    // clones every time.
    if (auto block = TMaybeNode<TBlockExpr>(*result)) {
        for (auto& fdecl : Context.TakeGenericInstantiations()) {
            block.Cast()->Stmts.push_back(fdecl);
        }
    }
    return result;
}

} // namespace NTypeAnnotation
} // namespace NQumir
