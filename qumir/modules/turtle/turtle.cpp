#include "turtle.h"

#include <iostream>
#include <cmath>

#include <qumir/runtime/turtle.h>

namespace NQumir {
namespace NRegistry {

using namespace NRuntime;

TurtleModule::TurtleModule()
{
    auto integerType = MakeIntrusive<NAst::TIntegerType>();
    auto floatType = MakeIntrusive<NAst::TFloatType>();
    auto voidType = MakeIntrusive<NAst::TVoidType>();
    auto stringType = MakeIntrusive<NAst::TStringType>();
    auto voidPtrType = MakeIntrusive<NAst::TPointerType>(voidType);
    auto futureVoidType = NAst::WrapFutureType(voidType);

    std::vector<TExternalFunction> functions = {
        {
            .Name = "поднять хвост",
            .MangledName = "turtle_pen_up",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_pen_up());
            },
            .ArgTypes = {  },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "опустить хвост",
            .MangledName = "turtle_pen_down",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_pen_down());
            },
            .ArgTypes = {  },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "вперед",
            .MangledName = "turtle_forward",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_forward(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "назад",
            .MangledName = "turtle_backward",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_backward(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "влево",
            .MangledName = "turtle_turn_left",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_turn_left(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "вправо",
            .MangledName = "turtle_turn_right",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_turn_right(std::bit_cast<double>(args[0])));
            },
            .ArgTypes = { floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "сохранить состояние",
            .MangledName = "turtle_save_state",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_save_state());
            },
            .ArgTypes = {  },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "восстановить состояние",
            .MangledName = "turtle_restore_state",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(turtle_restore_state());
            },
            .ArgTypes = {  },
            .ReturnType = futureVoidType,
        },
    };

    ExternalFunctions_.swap(functions);
}

} // namespace NRegistry
} // namespace NQumir
