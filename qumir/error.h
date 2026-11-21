#pragma once

#include "location.h"

#include <list>
#include <string>
#include <string_view>
#include <exception>
#include <format>

namespace NQumir {

enum class EErrorId {
    UNDEFINED_IDENTIFIER,

    // lower_ast specific
    WHILE_MISSING_CONDITION,
    WHILE_CONDITION_NOT_NUMBER,
    REPEAT_MISSING_CONDITION,
    REPEAT_CONDITION_NOT_NUMBER,
    FOR_MISSING_PRECONDITION,
    FOR_MISSING_PREBODY,
    FOR_MISSING_POSTBODY,
    FOR_CONDITION_NOT_NUMBER,

    ARRAY_INDEX_NOT_NUMBER,
    FAILED_LOWER_ARRAY_INDICES,
    FAILED_LOWER_COLLECTION,
    COLLECTION_NOT_ARRAY,

    OPERAND_OF_CAST_NOT_VALUE,
    UNSUPPORTED_CAST_TYPES,

    OPERAND_OF_UNARY_NOT_NUMBER,
    BINARY_OPERANDS_NOT_NUMBERS,

    IF_CONDITION_NOT_NUMBER,
    BREAK_NOT_IN_LOOP,
    CONTINUE_NOT_IN_LOOP,
    RETURN_OUTSIDE_FUNCTION,

    RIGHT_HAND_SIDE_NOT_NUMBER,
    UNDEFINED_VARIABLE,
    ASSIGNMENT_TO_UNDEFINED,

    NOT_IMPLEMENTED_LOWERING,
    ROOT_EXPR_MUST_BE_BLOCK,
    VARIABLE_DECLS_BEFORE_FUNS,
    VAR_DECL_NO_BINDING,
    UNBOUND_FUNCTION_SYMBOL,
    PARAMETER_NO_BINDING,
    FUNCTION_CALL_NON_IDENTIFIER,
    NOT_A_FUNCTION,
    ARG_REF_MUST_BE_IDENTIFIER,
    INVALID_ARGUMENT,

    UNDEFINED_GLOBAL_SYMBOL,
    UNEXPECTED_TOP_LEVEL_STATEMENT,
    MULTI_INDEX_COLLECTION_MUST_BE_IDENTIFIER,
    VAR_HAS_NO_BINDING,
    UNDEFINED_NAME,
    NESTED_FUNCTIONS_NOT_SUPPORTED,
    PARSER_MESSAGE,

    COUNT,
};

class TErrorString {
public:
    template<EErrorId id, typename... TArgs>
    static std::string Get(TArgs&&... args) {
        return std::format(GetFormat(id), std::forward<TArgs>(args)...);
    }

private:
    static consteval const char* GetFormat(EErrorId id) {
        switch (id) {
        case EErrorId::UNDEFINED_IDENTIFIER: return "неопределённый идентификатор `{}`";

        case EErrorId::WHILE_MISSING_CONDITION: return "while: условие обязательно";
        case EErrorId::WHILE_CONDITION_NOT_NUMBER: return "условие while должно быть числом";
        case EErrorId::REPEAT_MISSING_CONDITION: return "repeat-until: условие обязательно";
        case EErrorId::REPEAT_CONDITION_NOT_NUMBER: return "условие repeat-until должно быть числом";
        case EErrorId::FOR_MISSING_PRECONDITION: return "for: предусловие обязательно";
        case EErrorId::FOR_MISSING_PREBODY: return "for: предтело обязательно";
        case EErrorId::FOR_MISSING_POSTBODY: return "for: посттело обязательно";
        case EErrorId::FOR_CONDITION_NOT_NUMBER: return "условие for должно быть числом";

        case EErrorId::ARRAY_INDEX_NOT_NUMBER: return "индекс массива должен быть числом";
        case EErrorId::FAILED_LOWER_ARRAY_INDICES: return "не удалось опустить индексы массива";
        case EErrorId::FAILED_LOWER_COLLECTION: return "не удалось опустить коллекцию";
        case EErrorId::COLLECTION_NOT_ARRAY: return "коллекция не является массивом";

        case EErrorId::OPERAND_OF_CAST_NOT_VALUE: return "операнд преобразования должен быть значением";
        case EErrorId::UNSUPPORTED_CAST_TYPES: return "неподдерживаемое приведение типов: из `{}` в `{}`";

        case EErrorId::OPERAND_OF_UNARY_NOT_NUMBER: return "операнд унарного должен быть числом";
        case EErrorId::BINARY_OPERANDS_NOT_NUMBERS: return "операнды бинарной операции должны быть числами";

        case EErrorId::IF_CONDITION_NOT_NUMBER: return "условие if должно быть числом";
        case EErrorId::BREAK_NOT_IN_LOOP: return "break вне цикла";
        case EErrorId::CONTINUE_NOT_IN_LOOP: return "continue вне цикла";
        case EErrorId::RETURN_OUTSIDE_FUNCTION: return "return вне функции";

        case EErrorId::RIGHT_HAND_SIDE_NOT_NUMBER: return "правый операнд присваивания должен быть числом";
        case EErrorId::UNDEFINED_VARIABLE: return "неопределённая переменная `{}`";
        case EErrorId::ASSIGNMENT_TO_UNDEFINED: return "присваивание неопределённому";

        case EErrorId::NOT_IMPLEMENTED_LOWERING: return "не реализовано: понижение для узла AST: `{}`";
        case EErrorId::ROOT_EXPR_MUST_BE_BLOCK: return "корневое выражение должно быть блоком";
        case EErrorId::VARIABLE_DECLS_BEFORE_FUNS: return "объявления переменных должны идти до объявлений функций";
        case EErrorId::VAR_DECL_NO_BINDING: return "объявление var не имеет привязки";

        case EErrorId::UNBOUND_FUNCTION_SYMBOL: return "непривязанный символ функции `{}` в области `{}`";
        case EErrorId::PARAMETER_NO_BINDING: return "параметр не имеет привязки";
        case EErrorId::FUNCTION_CALL_NON_IDENTIFIER: return "вызов функции для не-идентификатора не поддерживается";
        case EErrorId::NOT_A_FUNCTION: return "не функция";
        case EErrorId::ARG_REF_MUST_BE_IDENTIFIER: return "аргумент для параметра-ссылки должен быть идентификатором";
        case EErrorId::INVALID_ARGUMENT: return "недопустимый аргумент";

        case EErrorId::UNDEFINED_GLOBAL_SYMBOL: return "неопределённый глобальный символ: `{}`";
        case EErrorId::UNEXPECTED_TOP_LEVEL_STATEMENT: return "команды вне алгоритма не поддерживаются; перенесите их между `нач' и `кон'";
        case EErrorId::MULTI_INDEX_COLLECTION_MUST_BE_IDENTIFIER: return "multi-index коллекция должна быть идентификатором";
        case EErrorId::VAR_HAS_NO_BINDING: return "переменная не имеет привязки";
        case EErrorId::UNDEFINED_NAME: return "неопределённое имя";
        case EErrorId::NESTED_FUNCTIONS_NOT_SUPPORTED: return "вложенные объявления функций не поддерживаются";
        case EErrorId::PARSER_MESSAGE: return "{}";
        case EErrorId::COUNT: return "";

        default: return "";
        }
    }
};

class TError : public std::exception {
public:
    TError(const std::string& message)
        : Msg(message)
    { }

    TError(TLocation loc, const std::string& message)
        : Location(loc)
        , Msg(message)
    { }

    TError(TLocation loc, const std::exception& ex)
        : Location(loc)
    {
        if (auto e = dynamic_cast<const TError*>(&ex)) {
            // If wrapping an existing parser error at the same location with no message,
            // flatten to avoid duplicate empty frames.
            if (e->Location && e->Location->Line == Location->Line && e->Location->Column == Location->Column && e->Msg.empty()) {
                Msg = e->Msg; // likely empty
                Children = e->Children;
            } else {
                Children.push_back(*e);
            }
        } else {
            // For generic exceptions, store the message directly instead of nesting.
            Msg = ex.what();
        }
    }

    TError(const std::exception& ex)
        : TError({}, ex)
    { }

    TError(TLocation loc, const std::list<TError>& children)
        : Location(loc)
        , Children(children)
    { }

    const char* what() const noexcept override { return Msg.c_str(); }

    std::string ToString() const;

    std::list<TError>& GetChildren() {
        return Children;
    }

private:
    std::string ToString(int indent) const;

    std::string Msg;
    std::optional<TLocation> Location;
    std::list<TError> Children;
};

void SignErrorsIfMarked(std::string_view source, std::string signature);

} // namespace NQumir