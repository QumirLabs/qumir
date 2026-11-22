#include "painter.h"

#include <qumir/runtime/painter.h>

namespace NQumir {
namespace NRegistry {

using namespace NRuntime;

PainterModule::PainterModule() {
    auto integerType  = MakeIntrusive<NAst::TIntegerType>();
    auto boolType     = MakeIntrusive<NAst::TBoolType>();
    auto voidType     = MakeIntrusive<NAst::TVoidType>();
    auto stringType   = MakeIntrusive<NAst::TStringType>();
    auto intArrayType = MakeIntrusive<NAst::TArrayType>(integerType, 1);

    auto colorType = MakeIntrusive<NAst::TNamedType>("цвет", integerType);

    auto makeOutInt = [&]() -> NAst::TTypePtr {
        auto t = MakeIntrusive<NAst::TIntegerType>();
        return MakeIntrusive<NAst::TReferenceType>(t);
    };

    ExternalFunctions_ = {

        // -- Sheet info --------------------------------------------------------
        {
            .Name = "высота листа",
            .MangledName = "painter_sheet_height",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_sheet_height());
            },
            .ArgTypes = {},
            .ReturnType = integerType,
        },
        {
            .Name = "ширина листа",
            .MangledName = "painter_sheet_width",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_sheet_width());
            },
            .ArgTypes = {},
            .ReturnType = integerType,
        },
        {
            .Name = "центр x",
            .MangledName = "painter_center_x",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_center_x());
            },
            .ArgTypes = {},
            .ReturnType = integerType,
        },
        {
            .Name = "центр y",
            .MangledName = "painter_center_y",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_center_y());
            },
            .ArgTypes = {},
            .ReturnType = integerType,
        },
        {
            .Name = "ширина текста",
            .MangledName = "painter_text_width",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_text_width(reinterpret_cast<const char*>(args[0])));
            },
            .ArgTypes = { stringType },
            .ReturnType = integerType,
            .RequireArgsMaterialization = true,
        },
        {
            .Name = "значение в точке",
            .MangledName = "painter_get_pixel",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                return static_cast<uint64_t>(painter_get_pixel(args[0], args[1]));
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = colorType,
        },

        // -- Drawing parameters ------------------------------------------------
        {
            .Name = "перо",
            .MangledName = "painter_pen",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_pen(args[0], args[1]);
                return 0;
            },
            .ArgTypes = { integerType, colorType },
            .ReturnType = voidType,
        },
        {
            .Name = "кисть",
            .MangledName = "painter_brush",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_brush(args[0]);
                return 0;
            },
            .ArgTypes = { colorType },
            .ReturnType = voidType,
        },
        {
            .Name = "убрать кисть",
            .MangledName = "painter_no_brush",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_no_brush();
                return 0;
            },
            .ArgTypes = {},
            .ReturnType = voidType,
        },
        {
            .Name = "плотность",
            .MangledName = "painter_density",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_density(args[0]);
                return 0;
            },
            .ArgTypes = { integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "шрифт",
            .MangledName = "painter_font",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_font(reinterpret_cast<const char*>(args[0]),
                             static_cast<int64_t>(args[1]),
                             static_cast<bool>(args[2]),
                             static_cast<bool>(args[3]));
                return 0;
            },
            .ArgTypes = { stringType, integerType, boolType, boolType },
            .ReturnType = voidType,
            .RequireArgsMaterialization = true,
        },

        // -- Drawing commands --------------------------------------------------
        {
            .Name = "в точку",
            .MangledName = "painter_move_to",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_move_to(args[0], args[1]);
                return 0;
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "линия",
            .MangledName = "painter_line",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_line(args[0], args[1], args[2], args[3]);
                return 0;
            },
            .ArgTypes = { integerType, integerType, integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "линия в точку",
            .MangledName = "painter_line_to",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_line_to(args[0], args[1]);
                return 0;
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "многоугольник",
            .MangledName = "painter_polygon",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_polygon(static_cast<int64_t>(args[0]),
                                reinterpret_cast<int64_t*>(args[1]),
                                reinterpret_cast<int64_t*>(args[2]));
                return 0;
            },
            .ArgTypes = { integerType, intArrayType, intArrayType },
            .ReturnType = voidType,
        },
        {
            .Name = "пиксель",
            .MangledName = "painter_pixel",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_pixel(args[0], args[1], args[2]);
                return 0;
            },
            .ArgTypes = { integerType, integerType, colorType },
            .ReturnType = voidType,
        },
        {
            .Name = "прямоугольник",
            .MangledName = "painter_rect",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_rect(args[0], args[1], args[2], args[3]);
                return 0;
            },
            .ArgTypes = { integerType, integerType, integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "эллипс",
            .MangledName = "painter_ellipse",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_ellipse(args[0], args[1], args[2], args[3]);
                return 0;
            },
            .ArgTypes = { integerType, integerType, integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "окружность",
            .MangledName = "painter_circle",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_circle(args[0], args[1], args[2]);
                return 0;
            },
            .ArgTypes = { integerType, integerType, integerType },
            .ReturnType = voidType,
        },
        {
            .Name = "надпись",
            .MangledName = "painter_text",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_text(args[0], args[1], reinterpret_cast<const char*>(args[2]));
                return 0;
            },
            .ArgTypes = { integerType, integerType, stringType },
            .ReturnType = voidType,
            .RequireArgsMaterialization = true,
        },
        {
            .Name = "залить",
            .MangledName = "painter_fill",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_fill(args[0], args[1]);
                return 0;
            },
            .ArgTypes = { integerType, integerType },
            .ReturnType = voidType,
        },

        // -- Sheet management --------------------------------------------------
        {
            .Name = "новый лист",
            .MangledName = "painter_new_sheet",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                auto* future = painter_new_sheet(
                    static_cast<int64_t>(args[0]),
                    static_cast<int64_t>(args[1]),
                    static_cast<int64_t>(args[2]));
                return reinterpret_cast<uint64_t>(future);
            },
            .ArgTypes = { integerType, integerType, colorType },
            .ReturnType = NAst::WrapFutureType(voidType),
        },
        {
            .Name = "загрузить лист",
            .MangledName = "painter_load_sheet",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_load_sheet(reinterpret_cast<const char*>(args[0]));
                return 0;
            },
            .ArgTypes = { stringType },
            .ReturnType = voidType,
            .RequireArgsMaterialization = true,
        },
        {
            .Name = "сохранить лист",
            .MangledName = "painter_save_sheet",
            .Packed = +[](const uint64_t* args, size_t) -> uint64_t {
                painter_save_sheet(reinterpret_cast<const char*>(args[0]));
                return 0;
            },
            .ArgTypes = { stringType },
            .ReturnType = voidType,
            .RequireArgsMaterialization = true,
        },
    };
}

} // namespace NRegistry
} // namespace NQumir
