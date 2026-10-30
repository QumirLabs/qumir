#pragma once

#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>

#include <qumir/parser/type.h>

namespace NQumir {
namespace NIR {

enum class EKind : uint8_t {
    I1,
    I8, // for char* (strings)
    I16, // unused
    I32, // unused
    I64,
    I128,
    U8, // unused
    U16, // unused
    U32, // unused
    U64, // unused
    U128,
    F32,
    F64,
    Void,
    Undef, // bad type for optimization errors
    Ptr,
    Func,
    Struct,
    Vec,
};

struct TType {
    EKind Kind;
    // Pointer: Kind=Ptr, Aux=points to TType of the pointee
    // Function: Kind=Func, Aux=points to funSigId
    // Struct: Kind=Struct, Aux=points to structId
    int Aux=-1;
};

struct TFuncSig {
    std::vector<int> Params;
    int Result;
};

struct TStructL {
    std::vector<int> FieldTypes; /* TODO: align/packed/hasVptr */
};

struct TVecL {
    int ElementType;
    int Size;
};

class TTypeTable {
public:
    int I(EKind k);
    int Ptr(int to);
    int Func(std::vector<int> args, int ret);
    int Struct(std::vector<int> fields);
    int Vec(int elemType, int count);
    int Unify(int left, int right);

    void Print(std::ostream& out, int typeId) const;
    void Format(std::ostream& out, uint64_t bitRepr, int typeId) const;

    bool IsPrimitive(int typeId) const;
    bool IsFloat(int typeId) const;
    bool IsInteger(int typeId) const;
    bool IsSigned(int typeId) const;
    bool IsUnsigned(int typeId) const;
    bool IsVoid(int typeId) const;
    bool IsPointer(int typeId) const;
    bool IsVector(int typeId) const;
    EKind GetKind(int typeId) const;
    int UnderlyingType(int typeId) const; // for Ptr, Func, Struct, Vec
    const std::vector<int>& GetStructFields(int typeId) const;
    // Size of the type payload in bytes. Stack frames may add their own alignment.
    int SizeInBytes(int typeId) const;
    int AlignInBytes(int typeId) const;
    // Byte offset of a struct field under the same C-compatible alignment as
    // SizeInBytes, so IR, VM and LLVM agree on field placement.
    int FieldOffset(int structTypeId, int fieldIndex) const;
    int VectorSize(int typeId) const; // for Vec only, returns the number of elements

    // Overrides the size/alignment used for Ptr and Func types (default 8,
    // i.e. a 64-bit target). Must be called before any lowering that bakes
    // FieldOffset values into the IR - target width is fixed for the
    // lifetime of the module.
    void SetPointerSize(int bytes);
    int PointerSizeInBytes() const {
        return PointerSize;
    }

private:
    std::vector<TType> Types;
    std::vector<TFuncSig> FuncSigs;
    std::vector<TStructL> Structs;
    std::vector<TVecL> Vectors;

    std::unordered_map<EKind,int> PrimitiveCache;
    std::unordered_map<int, int> PtrCache;
    std::map<std::tuple<std::vector<int>,int>, int> FuncCache;
    std::map<std::vector<int>, int> StructCache;
    std::map<std::pair<int,int>, int> VecCache;

    int PointerSize = 8;
};

int FromAstType(const NAst::TTypePtr& tastType, TTypeTable& tt);

} // namespace NIR
} // namespace NQumir
