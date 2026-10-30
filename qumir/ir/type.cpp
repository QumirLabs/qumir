#include "type.h"
#include "qumir/parser/type.h"
#include "qumir/align.h"

#include <iostream>
#include <sstream>
#include <iomanip>

namespace NQumir {
namespace NIR {

int TTypeTable::I(EKind k) {
    auto it = PrimitiveCache.find(k);
    if (it != PrimitiveCache.end()) return it->second;

    Types.push_back({
        .Kind = k,
        .Aux = -1
    });

    return PrimitiveCache[k] = (int)Types.size()-1;
}

int TTypeTable::Ptr(int to) {
    auto it = PtrCache.find(to);
    if (it != PtrCache.end()) return it->second;

    Types.push_back({
        .Kind = EKind::Ptr,
        .Aux = to
    });

    return PtrCache[to] = (int)Types.size()-1;
}

int TTypeTable::Func(std::vector<int> args, int ret) {
    auto it = FuncCache.find({args, ret});
    if (it != FuncCache.end()) return it->second;

    int id = (int)FuncSigs.size();
    FuncSigs.push_back({
        .Params = args,
        .Result = ret
    });
    Types.push_back({
        .Kind = EKind::Func,
        .Aux = id
    });

    return FuncCache[{args, ret}] = (int)Types.size()-1;
}

int TTypeTable::Struct(std::vector<int> fields) {
    auto it = StructCache.find(fields);
    if (it != StructCache.end()) return it->second;

    int id = (int)Structs.size();
    Structs.push_back({
        .FieldTypes = fields
    });
    Types.push_back({
        .Kind = EKind::Struct,
        .Aux = id
    });

    return StructCache[fields] = (int)Types.size()-1;
}

int TTypeTable::Vec(int elemType, int count) {
    auto it = VecCache.find({elemType, count});
    if (it != VecCache.end()) {
        return it->second;
    }

    int id = (int)Vectors.size();
    Vectors.push_back({
        .ElementType = elemType,
        .Size = count
    });
    Types.push_back({
        .Kind = EKind::Vec,
        .Aux = id,
    });

    return VecCache[{elemType, count}] = (int)Types.size()-1;
}

int TTypeTable::Unify(int leftId, int rightId) {
    if (leftId == rightId) return leftId;
    auto left = Types[leftId];
    auto right = Types[rightId];

    if (IsInteger(leftId) && IsInteger(rightId)) {
        // both integers: unify to the larger one, signedness rules:
        // if one is signed and the other unsigned, unify to signed
        throw std::runtime_error("Integer unification not implemented yet");
    }
    if (IsFloat(leftId) && IsFloat(rightId)) {
        // both floats: promote to larger
        throw std::runtime_error("Float unification not implemented yet");
    }
    if (IsInteger(leftId) && IsFloat(rightId)) {
        // int to float: promote to float
        return rightId;
    }
    if (IsFloat(leftId) && IsInteger(rightId)) {
        // float to int: promote to float
        return leftId;
    }
    auto leftKind = left.Kind;
    auto rightKind = right.Kind;
    throw std::runtime_error("Cannot unify types of different kinds: " + std::to_string((int)leftKind) + " and " + std::to_string((int)rightKind) + " ids: " + std::to_string(leftId) + " and " + std::to_string(rightId) + ")");
}

int FromAstType(const NAst::TTypePtr& t, TTypeTable& tt) {
    if (auto named = NAst::TMaybeType<NAst::TNamedType>(t)) {
        if (!named.Cast()->UnderlyingType) {
            throw std::runtime_error("FromAstType: named type '" + named.Cast()->Name + "' has null UnderlyingType — not resolved by name resolver");
        }
        return FromAstType(named.Cast()->UnderlyingType, tt);
    }
    if (NAst::TMaybeType<NAst::TFutureType>(t)) {
        throw std::runtime_error("AST Future<T> cannot be lowered as a regular IR type");
    }
    if (auto i = NAst::TMaybeType<NAst::TIntegerType>(t)) {
        using K = NAst::TIntegerType::EKind;
        switch (i.Cast()->Kind) {
            case K::I8: return tt.I(EKind::I8);
            case K::I16: return tt.I(EKind::I16);
            case K::I32: return tt.I(EKind::I32);
            case K::I64: return tt.I(EKind::I64);
            case K::I128: return tt.I(EKind::I128);
            case K::U8: return tt.I(EKind::U8);
            case K::U16: return tt.I(EKind::U16);
            case K::U32: return tt.I(EKind::U32);
            case K::U64: return tt.I(EKind::U64);
            case K::U128: return tt.I(EKind::U128);
        }
    }
    if (auto f = NAst::TMaybeType<NAst::TFloatType>(t)) {
        return tt.I(f.Cast()->Kind == NAst::TFloatType::F32 ? EKind::F32 : EKind::F64);
    }

    if (NAst::TMaybeType<NAst::TBoolType>(t)) {
        return tt.I(EKind::I1);
    }
    if (NAst::TMaybeType<NAst::TVoidType>(t)) {
        return tt.I(EKind::Void);
    }
    if (NAst::TMaybeType<NAst::TSymbolType>(t)) {
        return tt.I(EKind::I32);
    }

    if (NAst::TMaybeType<NAst::TStringType>(t)) {
        return tt.Ptr(tt.I(EKind::I8));
    }
    if (auto maybeArray = NAst::TMaybeType<NAst::TArrayType>(t)) {
        return tt.Ptr(FromAstType(maybeArray.Cast()->ElementType, tt));
    }

    if (auto p = NAst::TMaybeType<NAst::TPointerType>(t)) {
        int to = FromAstType(p.Cast()->PointeeType, tt);
        return tt.Ptr(to);
    }
    if (auto p = NAst::TMaybeType<NAst::TReferenceType>(t)) {
        int to = FromAstType(p.Cast()->ReferencedType, tt);
        return tt.Ptr(to);
    }

    if (auto s = NAst::TMaybeType<NAst::TStructType>(t)) {
        std::vector<int> fs;
        for (auto& [name, type] : s.Cast()->Fields) {
            fs.push_back(FromAstType(type, tt));
        }
        return tt.Struct(std::move(fs));
    }

    if (auto fn = NAst::TMaybeType<NAst::TFunctionType>(t)) {
        std::vector<int> ps;
        for (auto& a : fn.Cast()->ParamTypes) {
            ps.push_back(FromAstType(a, tt));
        }
        int r = FromAstType(fn.Cast()->ReturnType, tt);
        return tt.Func(std::move(ps), r);
    }

    if (auto v = NAst::TMaybeType<NAst::TVectorType>(t)) {
        int elemType = FromAstType(v.Cast()->ElementType, tt);
        int count = v.Cast()->Size;
        return tt.Vec(elemType, count);
    }

    return -1;
}

void TTypeTable::Print(std::ostream& out, int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        out << "<invalid type>";
        return;
    }
    auto& type = Types[typeId];
    switch (type.Kind) {
        case EKind::I1: out << "i1"; break;
        case EKind::I8: out << "i8"; break;
        case EKind::I16: out << "i16"; break;
        case EKind::I32: out << "i32"; break;
        case EKind::I64: out << "i64"; break;
        case EKind::I128: out << "i128"; break;
        case EKind::U8: out << "u8"; break;
        case EKind::U16: out << "u16"; break;
        case EKind::U32: out << "u32"; break;
        case EKind::U64: out << "u64"; break;
        case EKind::U128: out << "u128"; break;
        case EKind::F32: out << "f32"; break;
        case EKind::F64: out << "f64"; break;
        case EKind::Void: out << "void"; break;
        case EKind::Undef: out << "undef"; break;
        case EKind::Ptr: {
            out << "ptr to ";
            Print(out, type.Aux);
            break;
        }
        case EKind::Func: {
            auto& sig = FuncSigs[type.Aux];
            out << "func(";
            for (size_t i = 0; i < sig.Params.size(); ++i) {
                Print(out, sig.Params[i]);
                if (i < sig.Params.size() - 1) out << ", ";
            }
            out << ") -> ";
            Print(out, sig.Result);
            break;
        }
        case EKind::Struct: {
            auto& str = Structs[type.Aux];
            out << "struct { ";
            for (size_t i = 0; i < str.FieldTypes.size(); ++i) {
                Print(out, str.FieldTypes[i]);
                if (i < str.FieldTypes.size() - 1) out << "; ";
            }
            out << "}";
            break;
        }
        case EKind::Vec: {
            const auto& vec = Vectors[type.Aux];
            out << "vec<";
            Print(out, vec.ElementType);
            out << ", " << vec.Size << ">";
            break;
        }
    }
}

void TTypeTable::Format(std::ostream& out, uint64_t bitRepr, int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        out << "<invalid type>";
        return;
    }
    auto& type = Types[typeId];
    std::ostringstream ss;
    switch (type.Kind) {
        case EKind::I1: {
            ss << (bitRepr ? "true" : "false");
            break;
        }
        case EKind::I8: {
            ss << static_cast<int>(static_cast<int8_t>(bitRepr));
            break;
        }
        case EKind::I16: {
            ss << static_cast<int16_t>(bitRepr);
            break;
        }
        case EKind::I32: {
            ss << (int32_t)bitRepr;
            break;
        }
        case EKind::I64: {
            ss << (int64_t)bitRepr;
            break;
        }
        case EKind::U8: {
            ss << static_cast<unsigned>(static_cast<uint8_t>(bitRepr));
            break;
        }
        case EKind::U16: {
            ss << static_cast<uint16_t>(bitRepr);
            break;
        }
        case EKind::U32: {
            ss << static_cast<uint32_t>(bitRepr);
            break;
        }
        case EKind::U64: {
            ss << static_cast<uint64_t>(bitRepr);
            break;
        }
        case EKind::I128: {
            ss << static_cast<int64_t>(bitRepr);
            break;
        }
        case EKind::U128: {
            ss << static_cast<uint64_t>(bitRepr);
            break;
        }
        case EKind::F32: {
            float f = std::bit_cast<float>(static_cast<uint32_t>(bitRepr));
            ss << std::fixed << std::setprecision(15) << f;
            break;
        }
        case EKind::F64: {
            double d = std::bit_cast<double>(bitRepr);
            ss << std::fixed << std::setprecision(15) << d;
            break;
        }
        case EKind::Void: {
            ss << "<void>";
            break;
        }
        case EKind::Ptr: {
            if (bitRepr == 0) {
                ss << "null";
            } else {
                ss << "0x" << std::hex << bitRepr << std::dec;
            }
            break;
        }
        case EKind::Func: {
            if (bitRepr == 0) {
                ss << "null";
            } else {
                ss << "<func 0x" << std::hex << bitRepr << std::dec << ">";
            }
            break;
        }
        case EKind::Struct: {
            ss << "<struct 0x" << std::hex << bitRepr << std::dec << ">";
            break;
        }
        case EKind::Vec: {
            ss << "<vec 0x" << std::hex << bitRepr << std::dec << ">";
            break;
        }
    }
    out << ss.str();
}

bool TTypeTable::IsVoid(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::Void;
}

bool TTypeTable::IsPrimitive(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        return false;
    }
    auto k = Types[typeId].Kind;
    return k != EKind::Ptr && k != EKind::Func && k != EKind::Struct && k != EKind::Vec;
}

bool TTypeTable::IsFloat(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::F32 || k == EKind::F64;
}

bool TTypeTable::IsInteger(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::I1 || k == EKind::I8 || k == EKind::I16 || k == EKind::I32 || k == EKind::I64
        || k == EKind::I128
        || k == EKind::U8 || k == EKind::U16 || k == EKind::U32 || k == EKind::U64
        || k == EKind::U128;
}

bool TTypeTable::IsSigned(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::I1 || k == EKind::I8 || k == EKind::I16 || k == EKind::I32 || k == EKind::I64
        || k == EKind::I128;
}

bool TTypeTable::IsUnsigned(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::U8 || k == EKind::U16 || k == EKind::U32 || k == EKind::U64
        || k == EKind::U128;
}

bool TTypeTable::IsPointer(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return false;
    auto k = Types[typeId].Kind;
    return k == EKind::Ptr;
}

bool TTypeTable::IsVector(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        return false;
    }
    auto k = Types[typeId].Kind;
    return k == EKind::Vec;
}

EKind TTypeTable::GetKind(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        throw std::runtime_error("Invalid typeId in GetKind");
    }
    return Types[typeId].Kind;
}

int TTypeTable::UnderlyingType(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        throw std::runtime_error("Invalid typeId in UnderlyingType");
    }
    auto& type = Types[typeId];
    if (type.Kind == EKind::Ptr || type.Kind == EKind::Func || type.Kind == EKind::Struct) {
        return type.Aux;
    }
    if (
        type.Kind == EKind::Vec
        && type.Aux >= 0 && type.Aux < (int)Vectors.size())
    {
        return Vectors[type.Aux].ElementType;
    }
    throw std::runtime_error("Type is not Ptr, Func, Struct, or Vec in UnderlyingType");
}

int TTypeTable::SizeInBytes(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return 8;
    switch (Types[typeId].Kind) {
    case EKind::I1:
    case EKind::I8:
    case EKind::U8:
        return 1;
    case EKind::I16:
    case EKind::U16:
        return 2;
    case EKind::I32:
    case EKind::U32:
    case EKind::F32:
        return 4;
    case EKind::I64:
    case EKind::U64:
    case EKind::F64:
        return 8;
    case EKind::I128:
    case EKind::U128:
        return 16;
    case EKind::Ptr:
    case EKind::Func:
        return PointerSize;
    case EKind::Void:
        return 0;
    case EKind::Struct: {
        // Compute size with C-compatible field alignment so that qumir struct
        // layout matches the C ABI (required for external structs like TColumn).
        int offset = 0;
        int maxAlign = 1;
        for (int f : Structs[Types[typeId].Aux].FieldTypes) {
            int fieldSize = SizeInBytes(f);
            int fieldAlign = AlignInBytes(f);
            offset = AlignUp(offset, fieldAlign);
            maxAlign = std::max(maxAlign, fieldAlign);
            offset += fieldSize;
        }
        int structAlign = std::min(maxAlign, 16);
        return AlignUp(offset, structAlign);
    }
    case EKind::Vec: {
        const auto& vec = Vectors[Types[typeId].Aux];
        int elemSize = SizeInBytes(vec.ElementType);
        return elemSize * vec.Size;
    }
    }
    return 8;
}

int TTypeTable::AlignInBytes(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) return 8;
    auto kind = Types[typeId].Kind;
    // 128-bit integers are the only payload wider than a machine word, and the
    // C ABI aligns them to their size.
    int cap = (kind == EKind::I128 || kind == EKind::U128) ? 16 : 8;
    return std::min(SizeInBytes(typeId), cap);
}

int TTypeTable::FieldOffset(int structTypeId, int fieldIndex) const {
    const std::vector<int>& fields = GetStructFields(structTypeId);
    int offset = 0;
    for (int i = 0; i <= fieldIndex && i < (int)fields.size(); ++i) {
        int fieldSize = SizeInBytes(fields[i]);
        int fieldAlign = AlignInBytes(fields[i]);
        offset = AlignUp(offset, fieldAlign);
        if (i == fieldIndex) {
            return offset;
        }
        offset += fieldSize;
    }
    return offset;
}

int TTypeTable::VectorSize(int typeId) const {
    if (typeId < 0 || typeId >= (int)Types.size()) {
        throw std::runtime_error("Invalid typeId in VectorSize");
    }
    auto& type = Types[typeId];
    if (type.Kind != EKind::Vec) {
        throw std::runtime_error("Type is not Vec in VectorSize");
    }
    return Vectors[type.Aux].Size;
}

void TTypeTable::SetPointerSize(int bytes) {
    PointerSize = bytes;
}

const std::vector<int>& TTypeTable::GetStructFields(int typeId) const {
    auto& type = Types[typeId];
    if (type.Kind != EKind::Struct) {
        throw std::runtime_error("Type is not a Struct in GetStructFields");
    }
    return Structs[type.Aux].FieldTypes;
}

} // namespace NIR
} // namespace NQumir
