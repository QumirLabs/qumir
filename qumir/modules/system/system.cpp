#include "system.h"

#include <iostream>

#include <stdlib.h>
#include <math.h>

#include <qumir/runtime/runtime.h>

namespace NQumir {
namespace NRegistry {

namespace {
    template<typename T>
    NAst::TTypePtr outType() {
        auto underlyingType = MakeIntrusive<T>();
        underlyingType->Mutable = true;
        underlyingType->Readable = false;
        auto refType = MakeIntrusive<NAst::TReferenceType>(underlyingType);
        return refType;
    }

    NAst::TExprPtr number(const NAst::TTypePtr& type, int64_t value) {
        auto expr = MakeIntrusive<NAst::TNumberExpr>(TLocation{}, value);
        expr->Type = type;
        return expr;
    }

    NAst::TExprPtr unary(const char* op, const NAst::TExprPtr& operand, const NAst::TTypePtr& type) {
        auto expr = MakeIntrusive<NAst::TUnaryExpr>(TLocation{}, NAst::TOperator(op), operand);
        expr->Type = type;
        return expr;
    }

    NAst::TExprPtr binary(const char* op, const NAst::TExprPtr& left, const NAst::TExprPtr& right, const NAst::TTypePtr& type) {
        auto expr = MakeIntrusive<NAst::TBinaryExpr>(TLocation{}, NAst::TOperator(op), left, right);
        expr->Type = type;
        return expr;
    }

    NAst::TExprPtr ifExpr(const NAst::TExprPtr& cond, const NAst::TExprPtr& thenExpr, const NAst::TExprPtr& elseExpr, const NAst::TTypePtr& type) {
        auto expr = MakeIntrusive<NAst::TIfExpr>(TLocation{}, cond, thenExpr, elseExpr);
        expr->Type = type;
        return expr;
    }
}

SystemModule::SystemModule() {
    auto integerType = MakeIntrusive<NAst::TIntegerType>();
    auto floatType = MakeIntrusive<NAst::TFloatType>();
    auto boolType = MakeIntrusive<NAst::TBoolType>();
    auto voidType = MakeIntrusive<NAst::TVoidType>();
    auto stringType = MakeIntrusive<NAst::TStringType>();
    auto voidPtrType = MakeIntrusive<NAst::TPointerType>(voidType);
    auto fileUnderlying = MakeIntrusive<NAst::TIntegerType>(NAst::TIntegerType::I32);
    auto fileType = MakeIntrusive<NAst::TNamedType>("file", fileUnderlying);
    auto symbolType = MakeIntrusive<NAst::TSymbolType>();

    ExternalTypes_ = {
        { .Name = "file", .Type = fileUnderlying },
    };

    auto ident = [](const std::string& name) {
        return MakeIntrusive<NAst::TIdentExpr>(TLocation{}, name);
    };

    auto letBlock = [](std::vector<std::pair<std::string, NAst::TExprPtr>> vars, const NAst::TExprPtr& body) -> NAst::TExprPtr {
        auto bodyType = body->Type;
        std::vector<NAst::TExprPtr> stmts;
        for (auto& [name, value] : vars) {
            auto var = MakeIntrusive<NAst::TVarStmt>(TLocation{}, std::move(name), nullptr);
            var->Init = std::move(value);
            stmts.push_back(std::move(var));
        }
        stmts.push_back(body);
        auto block = MakeIntrusive<NAst::TBlockExpr>(TLocation{}, std::move(stmts));
        block->Type = bodyType;
        return block;
    };

    auto inlineMin = [&](const NAst::TTypePtr& type) {
        return [type, boolType, &ident, &letBlock](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
            auto body = ifExpr(
                binary("<", ident("$$left"), ident("$$right"), boolType),
                ident("$$left"),
                ident("$$right"),
                type);
            return letBlock({{"$$left", args[0]}, {"$$right", args[1]}}, std::move(body));
        };
    };
    auto inlineMax = [&](const NAst::TTypePtr& type) {
        return [type, boolType, &ident, &letBlock](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
            auto body = ifExpr(
                binary(">", ident("$$left"), ident("$$right"), boolType),
                ident("$$left"),
                ident("$$right"),
                type);
            return letBlock({{"$$left", args[0]}, {"$$right", args[1]}}, std::move(body));
        };
    };
    auto inlineAbs = [&](const NAst::TTypePtr& type) {
        return [type, boolType, &ident, &letBlock](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
            auto body = ifExpr(
                binary("<", ident("$$value"), number(type, 0), boolType),
                unary("-", ident("$$value"), type),
                ident("$$value"),
                type);
            return letBlock({{"$$value", args[0]}}, std::move(body));
        };
    };

    std::vector<TExternalFunction> functions = {
        {
            .Name = "sign",
            .MangledName = "sign",
            .ArgTypes = { floatType },
            .ReturnType = integerType,
            .Inline = [integerType, floatType, boolType, &ident, &letBlock](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                auto zero = number(floatType, 0);
                auto body = ifExpr(
                    binary(">", ident("$$value"), zero, boolType),
                    number(integerType, 1),
                    ifExpr(
                        binary("<", ident("$$value"), zero, boolType),
                        number(integerType, -1),
                        number(integerType, 0),
                        integerType),
                    integerType);
                return letBlock({{"$$value", args[0]}}, std::move(body));
            },
        },
        {
            .Name = "imin",
            .MangledName = "min_int64_t",
            .ArgTypes = { integerType, integerType },
            .ReturnType = integerType,
            .Inline = inlineMin(integerType),
        },
        {
            .Name = "imax",
            .MangledName = "max_int64_t",
            .ArgTypes = { integerType, integerType },
            .ReturnType = integerType,
            .Inline = inlineMax(integerType),
        },
        {
            .Name = "min",
            .MangledName = "min_double",
            .ArgTypes = { floatType, floatType },
            .ReturnType = floatType,
            .Inline = inlineMin(floatType),
        },
        {
            .Name = "max",
            .MangledName = "max_double",
            .ArgTypes = { floatType, floatType },
            .ReturnType = floatType,
            .Inline = inlineMax(floatType),
        },
        {
            .Name = "sqrt",
            .MangledName = "sqrt",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(sqrt(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "iabs",
            .MangledName = "labs",
            .ArgTypes = { integerType },
            .ReturnType = integerType,
            .Inline = inlineAbs(integerType),
        },
        {
            .Name = "abs",
            .MangledName = "fabs",
            .ArgTypes = { floatType },
            .ReturnType = floatType,
            .Inline = inlineAbs(floatType),
        },
        {
            .Name = "sin",
            .MangledName = "sin",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(sin(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "cos",
            .MangledName = "cos",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(cos(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "tg",
            .MangledName = "tan",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(tan(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "ctg",
            .MangledName = "cotan",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(cotan(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "arcsin",
            .MangledName = "asin",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(asin(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "arccos",
            .MangledName = "acos",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(acos(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "arctg",
            .MangledName = "atan",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(atan(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "ln",
            .MangledName = "log",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(log(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "lg",
            .MangledName = "log10",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(log10(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "exp",
            .MangledName = "exp",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(exp(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "div",
            .MangledName = "div_qum",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(div_qum(std::bit_cast<int64_t>(args[0]), std::bit_cast<int64_t>(args[1])));
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = integerType,
        },
        {
            .Name = "mod",
            .MangledName = "mod_qum",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(mod_qum(std::bit_cast<int64_t>(args[0]), std::bit_cast<int64_t>(args[1])));
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = integerType,
        },
        {
            .Name = "fpow",
            .MangledName = "fpow",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(fpow(std::bit_cast<double>(args[0]), static_cast<int>(std::bit_cast<int64_t>(args[1]))));
            },
            .ArgTypes = { floatType, integerType },
            .ReturnType = floatType,
        },
        {
            .Name = "pow",
            .MangledName = "pow",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(pow(std::bit_cast<double>(args[0]), std::bit_cast<double>(args[1])));
            },
            .ArgTypes = { floatType, floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "лит_в_вещ",
            .MangledName = "str_to_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                const void* addr = std::bit_cast<const void*>(args[1]);
                double result = NRuntime::str_to_double(reinterpret_cast<const char*>(args[0]), reinterpret_cast<int8_t*>(const_cast<void*>(addr)));
                return std::bit_cast<uint64_t>(result);
            },
            .ArgTypes = { stringType, outType<NAst::TBoolType>() },
            .ReturnType = floatType,
        },
        {
            .Name = "лит_в_цел",
            .MangledName = "str_to_int",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                const void* addr = std::bit_cast<const void*>(args[1]);
                return std::bit_cast<uint64_t>(NRuntime::str_to_int(reinterpret_cast<const char*>(args[0]), reinterpret_cast<int8_t*>(const_cast<void*>(addr))));
            },
            .ArgTypes = { stringType, outType<NAst::TBoolType>() },
            .ReturnType = integerType,
        },
        {
            .Name = "вещ_в_лит",
            .MangledName = "str_from_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::str_from_double(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = stringType,
        },
        {
            .Name = "цел_в_лит",
            .MangledName = "str_from_int",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::str_from_int(std::bit_cast<int64_t>(args[0])));
            },
            .ArgTypes = { integerType },
            .ReturnType = stringType,
        },
        {
            .Name = "str_input",
            .MangledName = "str_input",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::str_input());
            },
            .ArgTypes = {  },
            .ReturnType = stringType,
        },
        {
            .Name = "int",
            .MangledName = "trunc_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(trunc_double(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = integerType,
        },
        {
            .Name = "rnd",
            .MangledName = "rand_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(rand_double(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "rand",
            .MangledName = "rand_double_range",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(rand_double_range(std::bit_cast<double>(args[0]), std::bit_cast<double>(args[1])));
            },
            .ArgTypes = { floatType, floatType },
            .ReturnType = floatType,
        },
        {
            .Name = "irnd",
            .MangledName = "rand_int64",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(rand_int64(std::bit_cast<int64_t>(args[0])));
            },
            .ArgTypes = { integerType },
            .ReturnType = integerType,
        },
        {
            .Name = "irand",
            .MangledName = "rand_int64_range",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(rand_int64_range(std::bit_cast<int64_t>(args[0]), std::bit_cast<int64_t>(args[1])));
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = integerType,
        },

        // io
        {
            .Name = "input_double",
            .MangledName = "input_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::input_double());
            },
            .ArgTypes = {  },
            .ReturnType = floatType,
        },
        {
            .Name = "input_int64",
            .MangledName = "input_int64",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::input_int64());
            },
            .ArgTypes = {  },
            .ReturnType = integerType,
        },
        {
            .Name = "output_double",
            .MangledName = "output_double",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_double(std::bit_cast<double>(args[0]), static_cast<int>(std::bit_cast<int64_t>(args[1])), static_cast<int>(std::bit_cast<int64_t>(args[2])));
                return 0;
            },
            .ArgTypes = { floatType, integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "output_int64",
            .MangledName = "output_int64",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_int64(std::bit_cast<int64_t>(args[0]), static_cast<int>(std::bit_cast<int64_t>(args[1])));
                return 0;
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "output_string",
            .MangledName = "output_string",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_string(reinterpret_cast<const char*>(args[0]));
                return 0;
            },
            // TODO:
            .ArgTypes = { stringType },
            .ReturnType = voidType,
        },
        {
            .Name = "output_bool",
            .MangledName = "output_bool",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_bool(std::bit_cast<int64_t>(args[0]));
                return 0;
            },
            .ArgTypes = { boolType },
            .ReturnType = voidType,
        },
        {
            .Name = "output_symbol",
            .MangledName = "output_symbol",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_symbol(static_cast<int32_t>(std::bit_cast<int64_t>(args[0])));
                return 0;
            },
            .ArgTypes = { MakeIntrusive<NAst::TSymbolType>() },
            .ReturnType = voidType,
        },

        // strings
        {
            .Name = "str_from_lit",
            .MangledName = "str_from_lit",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* str = NRuntime::str_from_lit(reinterpret_cast<const char*>(args[0]));
                return std::bit_cast<uint64_t>(str);
            },
            .ArgTypes = { stringType },
            .ReturnType = stringType
        },
        {
            .Name = "str_slice",
            .MangledName = "str_slice",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* str = NRuntime::str_slice(reinterpret_cast<const char*>(args[0]), static_cast<int>(std::bit_cast<int64_t>(args[1])), static_cast<int>(std::bit_cast<int64_t>(args[2])));
                return std::bit_cast<uint64_t>(str);
            },
            .ArgTypes = { stringType, integerType, integerType },
            .ReturnType = stringType,
            .RequireArgsMaterialization = true
        },
        {
            .Name = "str_symbol_at",
            .MangledName = "str_symbol_at",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_symbol_at(reinterpret_cast<const char*>(args[0]), static_cast<int>(std::bit_cast<int64_t>(args[1])));
                return static_cast<uint64_t>(ret);
            },
            .ArgTypes = { stringType, integerType },
            .ReturnType = symbolType,
            .RequireArgsMaterialization = true
        },
        {
            .Name = "str_replace_sym",
            .MangledName = "str_replace_sym",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* str = NRuntime::str_replace_sym(reinterpret_cast<char*>(args[0]), static_cast<int32_t>(std::bit_cast<int64_t>(args[1])), std::bit_cast<int64_t>(args[2]));
                return std::bit_cast<uint64_t>(str);
            },
            .ArgTypes = { stringType, symbolType, integerType },
            .ReturnType = stringType,
            .RequireArgsMaterialization = true
        },
        {
            .Name = "str_retain",
            .MangledName = "str_retain",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::str_retain(reinterpret_cast<char*>(args[0]));
                return 0;
            },
            .ArgTypes = { stringType },
            .ReturnType = voidType
        },
        {
            .Name = "str_release",
            .MangledName = "str_release",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::str_release(reinterpret_cast<char*>(args[0]));
                return 0;
            },
            .ArgTypes = { stringType },
            .ReturnType = voidType
        },
        {
            .Name = "str_concat",
            .MangledName = "str_concat",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* str = NRuntime::str_concat(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]));
                return std::bit_cast<uint64_t>(str);
            },
            .ArgTypes = { stringType, stringType },
            .ReturnType = stringType,
        },
        {
            .Name = "str_compare",
            .MangledName = "str_compare",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_compare(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { stringType, stringType },
            .ReturnType = integerType,
            .RequireArgsMaterialization = true
        },
        {
            .Name = "длин",
            .MangledName = "str_len",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_len(reinterpret_cast<const char*>(args[0]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { stringType },
            .ReturnType = integerType,
            .RequireArgsMaterialization = true
        },
        {
            .Name = "str_from_unicode",
            .MangledName = "str_from_unicode",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* str = NRuntime::str_from_unicode(std::bit_cast<int64_t>(args[0]));
                return std::bit_cast<uint64_t>(str);
            },
            .ArgTypes = { symbolType },
            .ReturnType = stringType,
        },
        { // string module
            .Name = "позиция",
            .MangledName = "str_str",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_str(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { stringType, stringType },
            .ReturnType = integerType,
        },
        { // string module
            .Name = "поз", // alias for позиция
            .MangledName = "str_str",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_str(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { stringType, stringType },
            .ReturnType = integerType,
        },
        { // string module
            .Name = "позиция после",
            .MangledName = "str_str_from",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_str_from(std::bit_cast<int64_t>(args[0]), reinterpret_cast<const char*>(args[1]), reinterpret_cast<const char*>(args[2]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { integerType, stringType, stringType },
            .ReturnType = integerType,
        },
        { // string module
            .Name = "поз после", // alias for позиция после
            .MangledName = "str_str_from",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::str_str_from(std::bit_cast<int64_t>(args[0]), reinterpret_cast<const char*>(args[1]), reinterpret_cast<const char*>(args[2]));
                return std::bit_cast<uint64_t>(ret);
            },
            .ArgTypes = { integerType, stringType, stringType },
            .ReturnType = integerType,
        },
        { // string module
            .Name = "удалить",
            .MangledName = "str_delete_symbols",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::str_delete_symbols(reinterpret_cast<char**>(args[0]), std::bit_cast<int64_t>(args[1]), std::bit_cast<int64_t>(args[2]));
                return 0;
            },
            .ArgTypes = { outType<NAst::TStringType>(), integerType, integerType },
            .ReturnType = voidType,
        },
        { // string module
            .Name = "вставить",
            .MangledName = "str_insert_symbols",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::str_insert_symbols(reinterpret_cast<const char*>(args[0]), reinterpret_cast<char**>(args[1]), std::bit_cast<int64_t>(args[2]));
                return 0;
            },
            .ArgTypes = { stringType, outType<NAst::TStringType>(), integerType },
            .ReturnType = voidType,
            .RequireArgsMaterialization = true
        },
        // arrays
        {
            .Name = "array_create",
            .MangledName = "array_create",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* arr = NRuntime::array_create(static_cast<size_t>(args[0]));
                return std::bit_cast<uint64_t>(arr);
            },
            .ArgTypes = { integerType },
            .ReturnType = voidPtrType
        },
        {
            .Name = "array_destroy",
            .MangledName = "array_destroy",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::array_destroy(reinterpret_cast<void*>(args[0]));
                return 0;
            },
            .ArgTypes = { voidPtrType },
            .ReturnType = voidType
        },
        {
            .Name = "array_str_destroy",
            .MangledName = "array_str_destroy",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::array_str_destroy(reinterpret_cast<void*>(args[0]), static_cast<size_t>(args[1]));
                return 0;
            },
            .ArgTypes = { voidPtrType, integerType },
            .ReturnType = voidType
        },

        // files
        {
            .Name = "открыть на чтение",
            .MangledName = "file_open_for_read",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(NRuntime::file_open_for_read(reinterpret_cast<const char*>(args[0])));
            },
            .ArgTypes = { stringType },
            .ReturnType = fileType,
        },
        {
            .Name = "открыть на запись",
            .MangledName = "file_open_for_write",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(NRuntime::file_open_for_write(reinterpret_cast<const char*>(args[0])));
            },
            .ArgTypes = { stringType },
            .ReturnType = fileType,
        },
        {
            .Name = "открыть на добавление",
            .MangledName = "file_open_for_append",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return static_cast<uint64_t>(NRuntime::file_open_for_append(reinterpret_cast<const char*>(args[0])));
            },
            .ArgTypes = { stringType },
            .ReturnType = fileType,
        },
        {
            .Name = "закрыть",
            .MangledName = "file_close",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::file_close(static_cast<int32_t>(args[0]));
                return 0;
            },
            .ArgTypes = { fileType },
            .ReturnType = voidType,
        },
        {
            .Name = "есть данные",
            .MangledName = "file_has_more_data",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::file_has_more_data(static_cast<int32_t>(args[0]));
                return static_cast<uint64_t>(ret);
            },
            .ArgTypes = { fileType },
            .ReturnType = boolType,
        },
        {
            .Name = "конец файла",
            .MangledName = "file_eof",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto ret = NRuntime::file_eof(static_cast<int32_t>(args[0]));
                return static_cast<uint64_t>(ret);
            },
            .ArgTypes = { fileType },
            .ReturnType = boolType,
        },
        {
            .Name = "__ensure",
            .MangledName = "__ensure",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                __ensure(static_cast<bool>(args[0]), reinterpret_cast<const char*>(args[1]));
                return 0;
            },
            .ArgTypes = { boolType, stringType },
            .ReturnType = voidType,
        },
        {
            .Name = "input_set_file",
            .MangledName = "input_set_file",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::input_set_file(static_cast<int32_t>(args[0]));
                return 0;
            },
            .ArgTypes = { fileType },
            .ReturnType = voidType,
        },
        {
            .Name = "output_set_file",
            .MangledName = "output_set_file",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_set_file(static_cast<int32_t>(args[0]));
                return 0;
            },
            .ArgTypes = { fileType },
            .ReturnType = voidType,
        },
        {
            .Name = "input_reset_file",
            .MangledName = "input_reset_file",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::input_reset_file();
                return 0;
            },
            .ArgTypes = {  },
            .ReturnType = voidType,
        },
        {
            .Name = "output_reset_file",
            .MangledName = "output_reset_file",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                NRuntime::output_reset_file();
                return 0;
            },
            .ArgTypes = {  },
            .ReturnType = voidType,
        },

        {
            .Name = "МАКСВЕЩ",
            .ArgTypes = {  },
            .ReturnType = floatType,
            .Inline = [](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeIntrusive<NAst::TNumberExpr>(TLocation{}, std::numeric_limits<double>::max());
            },
        },
        {
            .Name = "МАКСЦЕЛ",
            .ArgTypes = {  },
            .ReturnType = integerType,
            .Inline = [](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeIntrusive<NAst::TNumberExpr>(TLocation{}, std::numeric_limits<int64_t>::max());
            },
        },
        {
            .Name = "юникод",
            .ArgTypes = { symbolType },
            .ReturnType = integerType,
            .Inline = [](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeIntrusive<NAst::TCastExpr>(args[0]->Location, args[0], MakeIntrusive<NAst::TIntegerType>());
            }
        },
        {
            .Name = "юнисимвол",
            .ArgTypes = { integerType },
            .ReturnType = symbolType,
            .Inline = [](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeIntrusive<NAst::TCastExpr>(args[0]->Location, args[0], MakeIntrusive<NAst::TSymbolType>());
            }
        },

        {
            .Name = "ждать",
            .MangledName = "qumir_sleep",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                auto* future = NRuntime::qumir_sleep(std::bit_cast<int64_t>(args[0]));
                return reinterpret_cast<uint64_t>(future);
            },
            .ArgTypes = { integerType },
            .ReturnType = NAst::WrapFutureType(voidType),
        },
        {
            .Name = "время",
            .MangledName = "time_from_daystart_millis",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(NRuntime::time_from_daystart_millis());
            },
            .ArgTypes = {  },
            .ReturnType = integerType,
        }

    };

    ExternalFunctions_.swap(functions);
}

} // namespace NRegistry
} // namespace NQumir
