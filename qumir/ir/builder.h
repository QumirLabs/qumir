#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <variant>
#include <ostream>
#include <unordered_map>
#include <memory>
#include <cstddef>
#include <set>
#include <list>

#include <qumir/location.h>
#include <qumir/ir/debug_options.h>

#include "type.h"

namespace NQumir {
namespace NIR {

struct TOp {
    constexpr TOp(uint64_t code): Code(code) {}
    // +,-,*,/
    constexpr TOp(char code): Code(code) {}
    // <=, >=, ==, !=
    constexpr TOp(const char* code) {
        Code = 0;
        while (*code) {
            Code = (Code << 8) | *code++;
        }
    }

    constexpr operator uint64_t() const {
        return Code;
    }

    std::string ToString() const {
        std::string s;
        int64_t v = Code;
        while (v != 0) {
            s = static_cast<char>(v & 0xFF) + s;
            v >>= 8;
        }
        return s;
    }

    uint64_t Code;
};

// Temporary variable. In VM bytecode, Idx is a byte offset in the register file.
struct TTmp {
    int32_t Idx;
};

// Global variable
struct TSlot {
    int32_t Idx;
};

// Local variable (function parameter or local variable).
// In IR (builder/lower_ast/passes): Idx is the variable ordinal (FunctionLevelIdx).
// In VM bytecode (TVMInstr): Idx is the byte offset in the stack frame, computed by vmcompiler.
struct TLocal {
    int32_t Idx;
};

struct TLabel {
    int32_t Idx;
};

inline bool operator<(const TLabel& a, const TLabel& b) {
    return a.Idx < b.Idx;
}

inline bool operator==(const TLabel& a, const TLabel& b) {
    return a.Idx == b.Idx;
}

// Immediate value (e.g. constant)
struct TImm {
    int64_t Value;
    int TypeId = -1;
};

struct TOperand {
    union {
        TTmp  Tmp;
        TSlot Slot;
        TLocal Local;
        TImm  Imm;
        TLabel Label;
    };

    enum class EType : uint8_t {
        Tmp,
        Slot,
        Local,
        Imm,
        Label
    } Type;

    TOperand() : Type(EType::Tmp), Tmp({-1}) {}
    TOperand(const TTmp& t) : Type(EType::Tmp), Tmp(t) {}
    TOperand(const TSlot& s) : Type(EType::Slot), Slot(s) {}
    TOperand(const TLocal& l) : Type(EType::Local), Local(l) {}
    TOperand(const TImm& i) : Type(EType::Imm), Imm(i) {}
    TOperand(const TLabel& l) : Type(EType::Label), Label(l) {}

    template<typename T>
    void Visit(T&& visitor) const {
        switch (Type) {
        case EType::Tmp:   visitor(Tmp);   break;
        case EType::Slot:  visitor(Slot);  break;
        case EType::Local: visitor(Local); break;
        case EType::Imm:   visitor(Imm);   break;
        case EType::Label: visitor(Label); break;
        }
    }
};

inline bool operator==(const TOperand& a, const TOperand& b) {
    if (a.Type != b.Type) return false;
    switch (a.Type) {
    case TOperand::EType::Tmp:   return a.Tmp.Idx == b.Tmp.Idx;
    case TOperand::EType::Slot:  return a.Slot.Idx == b.Slot.Idx;
    case TOperand::EType::Local: return a.Local.Idx == b.Local.Idx;
    case TOperand::EType::Imm:   return a.Imm.Value == b.Imm.Value && a.Imm.TypeId == b.Imm.TypeId;
    case TOperand::EType::Label: return a.Label.Idx == b.Label.Idx;
    default: return false;
    }
}

// --- User-defined literal suffixes for IR helper types ---
namespace NLiterals {

// TOp from single-character literal: '+'_op, '*'
constexpr TOp operator""_op(char c) noexcept { return TOp(c); }
// TOp from string literal: "=="_op, "<="_op, ">="_op, "!="_op, "&&"_op, "||"_op
constexpr TOp operator""_op(const char* s, std::size_t) noexcept {
    return TOp(s);
}

// Immediate value: 42_imm
constexpr TImm operator""_imm(unsigned long long v) noexcept {
    return TImm{ static_cast<int64_t>(v) };
}

// Temporaries/slots/labels by index: 0_t, 1_sl, 2_lab
constexpr TTmp   operator""_t(unsigned long long v) noexcept   {
    return TTmp{static_cast<int32_t>(v) };
}
constexpr TSlot  operator""_sl(unsigned long long v) noexcept  {
    return TSlot{static_cast<int32_t>(v) };
}
constexpr TLabel operator""_lab(unsigned long long v) noexcept {
    return TLabel{static_cast<int32_t>(v) };
}

} // namespace NLiterals

struct TInstr {
    TOp Op;
    TTmp Dest = {.Idx = -1};
    std::array<TOperand, 4> Operands;
    uint8_t OperandCount = 0;

    void Clear() {
        Op = TOp("nop");
        Dest = TTmp{ -1 };
        Operands.fill(TOperand{});
        OperandCount = 0;
    }

    int Size() const {
        return OperandCount;
    }
};

struct TPhi {
    TOp Op; // phi or nop
    TTmp Dest = {.Idx = -1};
    std::vector<TOperand> Operands;

    void Clear() {
        Op = TOp("nop");
        Dest = TTmp{-1};
        Operands.clear();
    }

    int Size() const {
        return Operands.size();
    }
};

struct TInstrDebugInfo {
    TLocation Location = {}; // source location (instruction)
    int32_t ScopeId = -1; // owning lexical scope

    operator bool() const {
        return Location.Line != 0 || Location.Byte != 0 || Location.Column != 0 || ScopeId != -1;
    }
};

struct TBlock {
    TLabel Label;
    std::vector<TPhi> Phis;
    std::vector<TInstr> Instrs;
    std::list<TLabel> Succ;
    std::list<TLabel> Pred;

    /* debug {*/
    std::vector<TInstrDebugInfo> InstrDebugInfo;
    /* debug }*/
};

struct TExecFunc;
struct TModule;

struct TExternalFunction {
    std::string Name;
    std::string MangledName;
    std::vector<int> ArgTypes;
    int ReturnTypeId = -1;
    using TPacked = uint64_t(*)(const uint64_t* args, size_t argCount);
    TPacked Packed = nullptr; // packed thunk for built-in functions
    // types not needed so far
    int SymId;
};

// LocalId -> DebugInfo
struct TLocalVarDebugInfo {
    std::string Name; // from ast name
    TLocation Location; // source location (declaration)
    int32_t ScopeId = -1; // owning lexical scope
    NAst::TTypePtr AstType = nullptr;

    operator bool() const {
        return !Name.empty() || Location.Line != 0 || Location.Byte != 0
            || Location.Column != 0 || ScopeId != -1 || AstType != nullptr;
    }
};

struct TFunctionDebugInfo {
    std::string Name;
    TLocation Location;
    int32_t ScopeId = -1;
    NAst::TTypePtr AstType = nullptr;
};

struct TFunction {
    std::string Name;
    std::vector<TLocal> ArgLocals;
    std::vector<TBlock> Blocks;
    std::vector<int> LocalTypes; // LocalIdx -> TypeId
    std::vector<int> TmpTypes; // TmpId -> TypeId
    std::vector<int> Label2Idx; // LabelIdx -> BlockIdx
    int ReturnTypeId = -1;
    bool ReturnTypeIsString = false; // TODO: remove me, clutch: support string returnType
    bool IsCoroutine = false;
    int CoroutineResultTypeId = -1;
    bool CfgBuilt = false;
    bool Cacheable = false; // eligible for the JIT object cache (see TFunDecl::Cacheable)
    bool Inline = false; // explicit inline request, preserved through specialization

    int SymId;
    int UniqueId; // unique within module, updated function will have same SymId and new UniqueId
    int32_t NextTmpIdx;
    int32_t NextLabelIdx;
    TExecFunc* Exec{nullptr};
    std::map<TLabel, int> LabelToBlockIdx;

    // debug info {
    std::optional<TFunctionDebugInfo> DebugInfo;
    std::vector<TLocalVarDebugInfo> LocalDebugInfo; // LocalIdx -> DebugInfo
    std::vector<int> ScopeParents;
    // debug info }

    int GetTmpType(int tmpId) const;
    int GetType(TTmp tmp) const;
    void SetType(TTmp tmp, int typeId);
    void Print(std::ostream& out, const TModule& module) const;
    int GetBlockIdx(const TLabel& label) const {
        return Label2Idx[label.Idx];
    }
};

struct TDebugPoint {
    std::string Kind;
    int FunctionIdx;
    TInstrDebugInfo Info;
    int LocalId = -1;
    std::string Op;
};

struct TDebugBinding {
    int FunctionIdx;
    TLocation Location;
    int LocalId;
    std::string Name;
};

struct TModule {
    std::vector<TFunction> Functions;
    std::vector<TExternalFunction> ExternalFunctions;
    std::unordered_map<int, int> SymIdToFuncIdx;
    std::unordered_map<int, int> SymIdToExtFuncIdx;

    std::vector<TImm> GlobalValues; // SlotId -> value
    std::vector<int> GlobalTypes; // global variables types

    // String literals started from 1, 0 is reserved for nullptr
    // This is termporary hacky solution until we have proper constant pool
    std::map<std::string, int> StringLiteralsSet = {{"", 0}};
    std::vector<std::string> StringLiterals = { "" };
    // shuffle's second operand selects an immutable compile-time mask.
    std::vector<std::vector<int>> VectorShuffleMasks;
    int ModuleConstructorFunctionId = -1;
    int ModuleDestructorFunctionId = -1;

    TTypeTable Types;
    std::string SourceFilePath; // empty for stdin
    bool HasSourceModules = false;
    TDebugOptions DebugOptions;
    std::string AsyncMode;
    std::vector<std::string> AsyncImports;
    std::vector<TDebugPoint> DebugPoints;
    std::vector<TDebugBinding> DebugBindings;

    TFunction* GetFunctionByName(const std::string& name);
    TFunction* GetEntryPoint();
    void Print(std::ostream& out) const;
};

class TBuilder {
public:
    TBuilder(TModule& m);

    int NewFunction(std::string name, std::vector<TLocal> args, int symId); // returns function index
    std::pair<TLabel, int> NewBlock(TLabel label = {-1}); // label,id

    int CurrentFunctionIdx() const;
    int CurrentBlockIdx() const;
    TLabel CurrentBlockLabel() const;
    void SetCurrentBlock(int idx = -1); // -1 = last
    void SetCurrentBlock(TLabel label);
    void SetCurrentFunction(int idx = -1); // -1 = last
    void SetScopeDebugInfo(int scopeId, int parentScopeId);

    TTmp Emit1(TOp op, std::initializer_list<TOperand> operands, const TInstrDebugInfo& debugInfo = {});
    void SetType(TTmp tmp, int typeId);
    int GetType(TTmp tmp) const;
    void SetType(TLocal local, int typeId, const TLocalVarDebugInfo& debugInfo);
    void ReserveLocals(int count);
    TLocal AllocLocal(int typeId, const TLocalVarDebugInfo& debugInfo); // allocates a new unnamed local and returns its index
    void UnifyTypes(TTmp left, TTmp right);
    void SetReturnType(int typeId);
    void Emit0(TOp op, std::initializer_list<TOperand> operands, const TInstrDebugInfo& debugInfo = {});
    int StringLiteral(const std::string& str); // string -> id, adds to current function's StringLiterals

    // Returns true if the last instruction in the current block unconditionally
    // or conditionally transfers control (e.g., jmp, ret, cmp), so no more
    // instructions should be appended to this block.
    bool IsCurrentBlockTerminated() const;
    TLabel NewLabel();

private:
    TTmp NewTmp();

    TModule& Module;
    TFunction* CurrentFunction = nullptr;
    TBlock* CurrentBlock = nullptr;

    int NextUniqueFunctionId = 0;
};

} // namespace NIR
} // namespace NOz
