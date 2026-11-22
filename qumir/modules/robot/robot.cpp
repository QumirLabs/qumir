#include "robot.h"

#include <qumir/runtime/robot.h>

namespace NQumir {
namespace NRegistry {

using namespace NRuntime;

RobotModule::RobotModule()
{
    auto boolType = MakeIntrusive<NAst::TBoolType>();
    auto floatType = MakeIntrusive<NAst::TFloatType>();
    auto voidType = MakeIntrusive<NAst::TVoidType>();
    auto futureVoidType = NAst::WrapFutureType(voidType);

    std::vector<TExternalFunction> functions = {
        {
            .Name = "влево",
            .MangledName = "robot_left",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(robot_left());
            },
            .ArgTypes = {},
            .ReturnType = futureVoidType,
            .NeedsLocator = true,
        },
        {
            .Name = "вправо",
            .MangledName = "robot_right",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(robot_right());
            },
            .ArgTypes = {},
            .ReturnType = futureVoidType,
            .NeedsLocator = true,
        },
        {
            .Name = "вверх",
            .MangledName = "robot_up",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(robot_up());
            },
            .ArgTypes = {},
            .ReturnType = futureVoidType,
            .NeedsLocator = true,
        },
        {
            .Name = "вниз",
            .MangledName = "robot_down",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(robot_down());
            },
            .ArgTypes = {},
            .ReturnType = futureVoidType,
            .NeedsLocator = true,
        },
        {
            .Name = "закрасить",
            .MangledName = "robot_paint",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return reinterpret_cast<uint64_t>(robot_paint());
            },
            .ArgTypes = {},
            .ReturnType = futureVoidType,
        },
        {
            .Name = "слева свободно",
            .MangledName = "robot_left_free",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_left_free() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "справа свободно",
            .MangledName = "robot_right_free",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_right_free() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "сверху свободно",
            .MangledName = "robot_top_free",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_top_free() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "снизу свободно",
            .MangledName = "robot_bottom_free",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_bottom_free() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "слева стена",
            .MangledName = "robot_left_wall",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_left_wall() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "справа стена",
            .MangledName = "robot_right_wall",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_right_wall() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "сверху стена",
            .MangledName = "robot_top_wall",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_top_wall() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "снизу стена",
            .MangledName = "robot_bottom_wall",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_bottom_wall() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "клетка закрашена",
            .MangledName = "robot_cell_painted",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_cell_painted() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "клетка чистая",
            .MangledName = "robot_cell_clean",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return robot_cell_clean() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "радиация",
            .MangledName = "robot_radiation",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(robot_radiation());
            },
            .ArgTypes = {},
            .ReturnType = floatType,
        },
        {
            .Name = "температура",
            .MangledName = "robot_temperature",
            .Packed = +[](const uint64_t* args, size_t argCount) -> uint64_t {
                return std::bit_cast<uint64_t>(robot_temperature());
            },
            .ArgTypes = {},
            .ReturnType = floatType,
        },
    };

    ExternalFunctions_.swap(functions);
}

} // namespace NRegistry
} // namespace NQumir
