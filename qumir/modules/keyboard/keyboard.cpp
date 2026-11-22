#include "keyboard.h"

#include <qumir/runtime/future.h>

#include <cstdint>

namespace {

struct TImmediateKeyboardCodeFuture : NQumir::ITypeErasedFuture {
    bool done() override {
        return true;
    }

    void resume() override {
    }

    void destroy() override {
    }

    void* address() override {
        return nullptr;
    }

    bool await_ready() override {
        return true;
    }

    void* await_suspend(void*) override {
        return nullptr;
    }

    void await_resume(void* result) override {
        if (result) {
            *static_cast<int64_t*>(result) = 0;
        }
    }
};

} // namespace

extern "C" {

bool keyboard_signal() {
    return false;
}

NQumir::ITypeErasedFuture* keyboard_code() {
    return new TImmediateKeyboardCodeFuture();
}

void keyboard_reset() {
}

} // extern "C"

namespace NQumir {
namespace NRegistry {

namespace {

NAst::TExprPtr ScanCodeConst(NAst::TTypeFactory& types, const NAst::TTypePtr& scanCodeType, int64_t value) {
    auto expr = MakeIntrusive<NAst::TNumberExpr>(TLocation{}, value, types);
    expr->Type = scanCodeType;
    return expr;
}

NAst::TExprPtr MakeScanCodeCast(const NAst::TExprPtr& value, const NAst::TTypePtr& targetType) {
    // A plain TCastExpr would be dispatched back to the module's own `cast`
    // operator by type annotation, looping forever; bitcast bypasses operators.
    return MakeIntrusive<NAst::TBitcastExpr>(value->Location, value, targetType);
}

NAst::TExprPtr MakeBinary(const char* op, const NAst::TExprPtr& left, const NAst::TExprPtr& right, const NAst::TTypePtr& type) {
    auto expr = MakeIntrusive<NAst::TBinaryExpr>(left->Location, NAst::TOperator(op), left, right);
    expr->Type = type;
    return expr;
}

} // namespace

KeyboardModule::KeyboardModule(NAst::TTypeFactory& types) {
    auto integerType = types.MakeInteger();
    auto boolType = types.Make<NAst::TBoolType>();
    auto voidType = types.Make<NAst::TVoidType>();
    auto scanCodeType = types.MakeNamed("сканкод", integerType);

    ExternalTypes_ = {
        {
            .Name = "сканкод",
            .Type = integerType,
        },
    };

    auto scanCodeConst = [scanCodeType, &types](int64_t value) {
        return [scanCodeType, value, &types](std::vector<NAst::TExprPtr>) -> NAst::TExprPtr {
            return ScanCodeConst(types, scanCodeType, value);
        };
    };

    auto addScanCode = [&](const std::string& name, const std::string& mangledName, int64_t value) {
        ExternalFunctions_.push_back({
            .Name = name,
            .MangledName = mangledName,
            .ArgTypes = {},
            .ReturnType = scanCodeType,
            .Inline = scanCodeConst(value),
        });
    };

    ExternalFunctions_ = {
        {
            .Name = "сигнал клав",
            .MangledName = "keyboard_signal",
            .Packed = +[](const uint64_t*, size_t) -> uint64_t {
                return keyboard_signal() ? 1 : 0;
            },
            .ArgTypes = {},
            .ReturnType = boolType,
        },
        {
            .Name = "код клав",
            .MangledName = "keyboard_code",
            .Packed = +[](const uint64_t*, size_t) -> uint64_t {
                return reinterpret_cast<uint64_t>(keyboard_code());
            },
            .ArgTypes = {},
            .ReturnType = types.WrapFuture(integerType),
        },
        {
            .Name = "сброс клав",
            .MangledName = "keyboard_reset",
            .Packed = +[](const uint64_t*, size_t) -> uint64_t {
                keyboard_reset();
                return 0;
            },
            .ArgTypes = {},
            .ReturnType = voidType,
        },
        {
            .Name = "cast",
            .MangledName = "keyboard_scan_code_from_int",
            .ArgTypes = { integerType },
            .ReturnType = scanCodeType,
            .IsOp = true,
            .Inline = [scanCodeType, &types](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeScanCodeCast(args[0], scanCodeType);
            },
        },
        {
            .Name = "cast",
            .MangledName = "keyboard_scan_code_to_int",
            .ArgTypes = { scanCodeType },
            .ReturnType = integerType,
            .IsOp = true,
            .Inline = [integerType, &types](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeScanCodeCast(args[0], integerType);
            },
        },
        {
            .Name = "==",
            .MangledName = "keyboard_scan_code_eq",
            .ArgTypes = { scanCodeType, scanCodeType },
            .ReturnType = boolType,
            .IsOp = true,
            .Inline = [integerType, boolType, &types](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeBinary("==",
                    MakeScanCodeCast(args[0], integerType),
                    MakeScanCodeCast(args[1], integerType),
                    boolType);
            },
        },
        {
            .Name = "!=",
            .MangledName = "keyboard_scan_code_ne",
            .ArgTypes = { scanCodeType, scanCodeType },
            .ReturnType = boolType,
            .IsOp = true,
            .Inline = [integerType, boolType, &types](std::vector<NAst::TExprPtr> args) -> NAst::TExprPtr {
                return MakeBinary("!=",
                    MakeScanCodeCast(args[0], integerType),
                    MakeScanCodeCast(args[1], integerType),
                    boolType);
            },
        },
    };

    addScanCode("КЛ_НАЗАД", "keyboard_key_backspace", 8);
    addScanCode("КЛ_BACKSPACE", "keyboard_key_backspace", 8);
    addScanCode("КЛ_TAB", "keyboard_key_tab", 9);
    addScanCode("КЛ_ВВОД", "keyboard_key_enter", 13);
    addScanCode("КЛ_ENTER", "keyboard_key_enter", 13);
    addScanCode("КЛ_RETURN", "keyboard_key_enter", 13);
    addScanCode("КЛ_ПРОБЕЛ", "keyboard_key_space", 32);
    addScanCode("КЛ_SPACE", "keyboard_key_space", 32);
    addScanCode("КЛ_PAGEUP", "keyboard_key_page_up", 33);
    addScanCode("КЛ_PGUP", "keyboard_key_page_up", 33);
    addScanCode("КЛ_PAGEDOWN", "keyboard_key_page_down", 34);
    addScanCode("КЛ_PGDOWN", "keyboard_key_page_down", 34);
    addScanCode("КЛ_END", "keyboard_key_end", 35);
    addScanCode("КЛ_HOME", "keyboard_key_home", 36);
    addScanCode("КЛ_ВЛЕВО", "keyboard_key_left", 37);
    addScanCode("КЛ_ВВЕРХ", "keyboard_key_up", 38);
    addScanCode("КЛ_ВПРАВО", "keyboard_key_right", 39);
    addScanCode("КЛ_ВНИЗ", "keyboard_key_down", 40);
    addScanCode("КЛ_INSERT", "keyboard_key_insert", 45);
    addScanCode("КЛ_DELETE", "keyboard_key_delete", 46);

    for (int i = 1; i <= 12; ++i) {
        addScanCode("КЛ_F" + std::to_string(i), "keyboard_key_f" + std::to_string(i), 111 + i);
    }
    for (int i = 1; i <= 9; ++i) {
        addScanCode("КЛ_" + std::to_string(i), "keyboard_key_digit_" + std::to_string(i), 48 + i);
    }
    addScanCode("КЛ_0", "keyboard_key_digit_0", 48);

    addScanCode("КЛ_Q", "keyboard_key_q", 81);
    addScanCode("КЛ_Й", "keyboard_key_q", 81);
    addScanCode("КЛ_W", "keyboard_key_w", 87);
    addScanCode("КЛ_Ц", "keyboard_key_w", 87);
    addScanCode("КЛ_E", "keyboard_key_e", 69);
    addScanCode("КЛ_У", "keyboard_key_e", 69);
    addScanCode("КЛ_R", "keyboard_key_r", 82);
    addScanCode("КЛ_К", "keyboard_key_r", 82);
    addScanCode("КЛ_T", "keyboard_key_t", 84);
    addScanCode("КЛ_Е", "keyboard_key_t", 84);
    addScanCode("КЛ_Y", "keyboard_key_y", 89);
    addScanCode("КЛ_Н", "keyboard_key_y", 89);
    addScanCode("КЛ_U", "keyboard_key_u", 85);
    addScanCode("КЛ_Г", "keyboard_key_u", 85);
    addScanCode("КЛ_I", "keyboard_key_i", 73);
    addScanCode("КЛ_Ш", "keyboard_key_i", 73);
    addScanCode("КЛ_O", "keyboard_key_o", 79);
    addScanCode("КЛ_Щ", "keyboard_key_o", 79);
    addScanCode("КЛ_P", "keyboard_key_p", 80);
    addScanCode("КЛ_З", "keyboard_key_p", 80);
    addScanCode("КЛ_Х", "keyboard_key_bracket_left", 219);
    addScanCode("КЛ_Ъ", "keyboard_key_bracket_right", 221);

    addScanCode("КЛ_A", "keyboard_key_a", 65);
    addScanCode("КЛ_Ф", "keyboard_key_a", 65);
    addScanCode("КЛ_S", "keyboard_key_s", 83);
    addScanCode("КЛ_Ы", "keyboard_key_s", 83);
    addScanCode("КЛ_D", "keyboard_key_d", 68);
    addScanCode("КЛ_В", "keyboard_key_d", 68);
    addScanCode("КЛ_F", "keyboard_key_f", 70);
    addScanCode("КЛ_А", "keyboard_key_f", 70);
    addScanCode("КЛ_G", "keyboard_key_g", 71);
    addScanCode("КЛ_П", "keyboard_key_g", 71);
    addScanCode("КЛ_H", "keyboard_key_h", 72);
    addScanCode("КЛ_Р", "keyboard_key_h", 72);
    addScanCode("КЛ_J", "keyboard_key_j", 74);
    addScanCode("КЛ_О", "keyboard_key_j", 74);
    addScanCode("КЛ_K", "keyboard_key_k", 75);
    addScanCode("КЛ_Л", "keyboard_key_k", 75);
    addScanCode("КЛ_L", "keyboard_key_l", 76);
    addScanCode("КЛ_Д", "keyboard_key_l", 76);
    addScanCode("КЛ_Ж", "keyboard_key_semicolon", 186);
    addScanCode("КЛ_Э", "keyboard_key_quote", 222);

    addScanCode("КЛ_Z", "keyboard_key_z", 90);
    addScanCode("КЛ_Я", "keyboard_key_z", 90);
    addScanCode("КЛ_X", "keyboard_key_x", 88);
    addScanCode("КЛ_Ч", "keyboard_key_x", 88);
    addScanCode("КЛ_C", "keyboard_key_c", 67);
    addScanCode("КЛ_С", "keyboard_key_c", 67);
    addScanCode("КЛ_V", "keyboard_key_v", 86);
    addScanCode("КЛ_М", "keyboard_key_v", 86);
    addScanCode("КЛ_B", "keyboard_key_b", 66);
    addScanCode("КЛ_И", "keyboard_key_b", 66);
    addScanCode("КЛ_N", "keyboard_key_n", 78);
    addScanCode("КЛ_Т", "keyboard_key_n", 78);
    addScanCode("КЛ_M", "keyboard_key_m", 77);
    addScanCode("КЛ_Ь", "keyboard_key_m", 77);
    addScanCode("КЛ_Б", "keyboard_key_comma", 188);
    addScanCode("КЛ_Ю", "keyboard_key_period", 190);
}

} // namespace NRegistry
} // namespace NQumir
