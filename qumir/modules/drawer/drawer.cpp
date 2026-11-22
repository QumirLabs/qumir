#include "drawer.h"

#include <qumir/runtime/drawer.h>

namespace NQumir {
namespace NRegistry {

using namespace NRuntime;

DrawerModule::DrawerModule()
{
    auto integerType = MakeIntrusive<NAst::TIntegerType>();
    auto floatType = MakeIntrusive<NAst::TFloatType>();
    auto voidType = MakeIntrusive<NAst::TVoidType>();
    auto stringType = MakeIntrusive<NAst::TStringType>();
    auto colorType = MakeIntrusive<NAst::TNamedType>("цвет", integerType);
    auto futureVoidType = NAst::WrapFutureType(voidType);

    std::vector<TExternalFunction> functions = {
        {
            .Name = "поднять перо",
            .MangledName = "drawer_pen_up",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                drawer_pen_up();
                return 0;
            },
            .ArgTypes = {  },
            .ReturnType = voidType,
        },
        {
            .Name = "опустить перо",
            .MangledName = "drawer_pen_down",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                drawer_pen_down();
                return 0;
            },
            .ArgTypes = {  },
            .ReturnType = voidType,
        },
        {
            .Name = "выбрать чернила",
            .MangledName = "drawer_set_color",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                drawer_set_color(static_cast<int64_t>(args[0]));
                return 0;
            },
            .ArgTypes = { colorType },
            .ReturnType = voidType,
        },
        {
            .Name = "сместиться в точку",
            .MangledName = "drawer_move_to",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(drawer_move_to(std::bit_cast<double>(args[0]), std::bit_cast<double>(args[1])));
            },
            .ArgTypes = { floatType, floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "сместиться на вектор",
            .MangledName = "drawer_move_by",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(drawer_move_by(std::bit_cast<double>(args[0]), std::bit_cast<double>(args[1])));
            },
            .ArgTypes = { floatType, floatType },
            .ReturnType = futureVoidType,
        },
        {
            .Name = "написать",
            .MangledName = "drawer_write_text",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(drawer_write_text(std::bit_cast<double>(args[0]), reinterpret_cast<const char*>(args[1])));
            },
            .ArgTypes = { floatType, stringType },
            .ReturnType = futureVoidType,
            .RequireArgsMaterialization = true,
        },
    };

    ExternalFunctions_.swap(functions);
}

} // namespace NRegistry
} // namespace NQumir
