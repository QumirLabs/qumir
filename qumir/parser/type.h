#pragma once

#include <memory>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace NQumir {
namespace NAst {

struct TType {
    bool Mutable = true;
    bool Readable = true;

    virtual ~TType() = default;
    virtual std::string ToString() const { return ""; }
    virtual const std::string_view TypeName() const = 0;
};

using TTypePtr = std::shared_ptr<TType>;

template<typename T>
requires std::is_base_of_v<TType, T>
using TConcreteTypePtr = std::shared_ptr<T>;

struct TGenericArg {
    enum class EKind {
        Type,
        Value,
    };

    EKind Kind = EKind::Type;
    TTypePtr Type;
    std::string Value;

    static TGenericArg TypeArg(TTypePtr type) {
        return TGenericArg{
            .Kind = EKind::Type,
            .Type = std::move(type),
        };
    }

    static TGenericArg ValueArg(std::string value) {
        return TGenericArg{
            .Kind = EKind::Value,
            .Value = std::move(value),
        };
    }

    std::string ToString() const {
        if (Kind == EKind::Type) {
            return Type ? Type->ToString() : "unknown";
        }
        return Value;
    }
};

template<typename T>
struct TMaybeType {
    TMaybeType(TTypePtr node)
        : Type(node && std::string_view(T::TypeId) == node->TypeName()
            ? std::static_pointer_cast<T>(std::move(node))
            : nullptr)
    { }

    std::shared_ptr<T> Cast() {
        return std::static_pointer_cast<T>(Type);
    }

    TTypePtr Type;
    operator bool() const { return Type != nullptr; }
};

struct TIntegerType : TType {
    static constexpr const char* TypeId = "Int";

    enum EKind {
        I8,
        I16,
        I32,
        I64,
        I128,
        U8,
        U16,
        U32,
        U64,
        U128,
    } Kind = I64;

    TIntegerType() = default;
    explicit TIntegerType(EKind kind)
        : Kind(kind)
    {}

    int BitWidth() const {
        switch (Kind) {
            case I8:
            case U8:
                return 8;
            case I16:
            case U16:
                return 16;
            case I32:
            case U32:
                return 32;
            case I64:
            case U64:
                return 64;
            case I128:
            case U128:
                return 128;
        }
        return 64;
    }

    bool IsSigned() const {
        switch (Kind) {
            case I8:
            case I16:
            case I32:
            case I64:
            case I128:
                return true;
            case U8:
            case U16:
            case U32:
            case U64:
            case U128:
                return false;
        }
        return true;
    }

    std::string ToString() const override {
        switch (Kind) {
            case I8: return "i8";
            case I16: return "i16";
            case I32: return "i32";
            case I64: return "i64";
            case I128: return "i128";
            case U8: return "u8";
            case U16: return "u16";
            case U32: return "u32";
            case U64: return "u64";
            case U128: return "u128";
        }
        return "i64";
    }

    const std::string_view TypeName() const override {
        return TIntegerType::TypeId;
    }
};

struct TFloatType : TType {
    static constexpr const char* TypeId = "Float";

    enum EKind {
        F32,
        F64,
    } Kind = F64;

    TFloatType() = default;
    explicit TFloatType(EKind kind)
        : Kind(kind)
    {}

    int BitWidth() const {
        return Kind == F32 ? 32 : 64;
    }

    std::string ToString() const override {
        return Kind == F32 ? "f32" : "f64";
    }

    const std::string_view TypeName() const override {
        return TFloatType::TypeId;
    }
};

struct TBoolType : TType {
    static constexpr const char* TypeId = "Bool";

    TBoolType() = default;

    const std::string_view TypeName() const override {
        return TBoolType::TypeId;
    }
};

struct TStringType : TType {
    static constexpr const char* TypeId = "String";

    TStringType() = default;

    const std::string_view TypeName() const override {
        return TStringType::TypeId;
    }
};

struct TSymbolType : TType {
    static constexpr const char* TypeId = "Char";

    TSymbolType() = default;

    const std::string_view TypeName() const override {
        return TSymbolType::TypeId;
    }
};

struct TVoidType : TType {
    static constexpr const char* TypeId = "Void";

    TVoidType() = default;

    const std::string_view TypeName() const override {
        return TVoidType::TypeId;
    }
};

struct TFunctionType : TType {
    static constexpr const char* TypeId = "Fun";

    std::vector<TTypePtr> ParamTypes;
    TTypePtr ReturnType;

    TFunctionType(std::vector<TTypePtr> params, TTypePtr ret)
        : ParamTypes(std::move(params))
        , ReturnType(std::move(ret))
    {}

    std::string ToString() const override {
        std::string s = "(";
        for (size_t i = 0; i < ParamTypes.size(); ++i) {
            s += ParamTypes[i] ? ParamTypes[i]->TypeName() : "unknown";
            if (i < ParamTypes.size() - 1) {
                s += ", ";
            }
        }
        s += ") -> ";
        s += ReturnType ? ReturnType->TypeName() : "unknown";
        return s;
    }

    const std::string_view TypeName() const override {
        return TFunctionType::TypeId;
    }
};

struct TFutureType : TType {
    static constexpr const char* TypeId = "Future";

    TTypePtr ResultType;

    explicit TFutureType(TTypePtr result)
        : ResultType(std::move(result))
    {}

    std::string ToString() const override {
        return "Future<" + (ResultType ? ResultType->ToString() : std::string("unknown")) + ">";
    }

    const std::string_view TypeName() const override {
        return TFutureType::TypeId;
    }
};

struct TArrayType : TType {
    static constexpr const char* TypeId = "Array";

    TTypePtr ElementType;
    int Arity{0}; // 0 - scalar, 1 - 1D, 2 - 2D, etc.

    TArrayType() = default;

    explicit TArrayType(TTypePtr et, int arity)
        : ElementType(std::move(et))
        , Arity(arity)
    {}

    std::string ToString() const override {
        return "[" + (ElementType ? std::string(ElementType->TypeName()) : "unknown") + "; " +
               (Arity > 0 ? std::to_string(Arity) : "?") + "]";
    }

    const std::string_view TypeName() const override {
        return TArrayType::TypeId;
    }
};

struct TPointerType : TType {
    static constexpr const char* TypeId = "Ptr";

    TTypePtr PointeeType;

    explicit TPointerType(TTypePtr pt)
        : PointeeType(std::move(pt))
    {}

    std::string ToString() const override {
        return "*" + (PointeeType ? std::string(PointeeType->TypeName()) : "unknown");
    }

    const std::string_view TypeName() const override {
        return TPointerType::TypeId;
    }
};

struct TReferenceType : TType {
    static constexpr const char* TypeId = "Ref";

    TTypePtr ReferencedType;

    explicit TReferenceType(TTypePtr rt)
        : ReferencedType(std::move(rt))
    {}

    std::string ToString() const override {
        return "&" + (ReferencedType ? std::string(ReferencedType->TypeName()) : "unknown");
    }

    const std::string_view TypeName() const override {
        return TReferenceType::TypeId;
    }
};

struct TNamedType : TType {
    static constexpr const char* TypeId = "Named";
    std::string Name;
    std::vector<TGenericArg> TypeArgs;
    TTypePtr UnderlyingType; // Resolved on name resolution phase, exported by modules
    std::optional<std::string> Reference; // Set = imported from this module; not printed in AST

    explicit TNamedType(
        std::string name,
        TTypePtr underlying,
        std::vector<TGenericArg> typeArgs = {})
        : Name(std::move(name))
        , TypeArgs(std::move(typeArgs))
        , UnderlyingType(std::move(underlying))
    {}

    std::string ToString() const override {
        std::string result = Name;
        if (!TypeArgs.empty()) {
            result += "[";
            for (size_t i = 0; i < TypeArgs.size(); ++i) {
                if (i != 0) {
                    result += ", ";
                }
                result += TypeArgs[i].ToString();
            }
            result += "]";
        }
        if (UnderlyingType) {
            return result + " (" + UnderlyingType->ToString() + ")";
        }
        return result;
    }

    const std::string_view TypeName() const override {
        return TNamedType::TypeId; // "Named" - consistent with TMaybeType contract
    }
};

struct TStructType : TType {
    static constexpr const char* TypeId = "Struct";

    std::vector<std::pair<std::string, TTypePtr>> Fields;

    explicit TStructType(std::vector<std::pair<std::string, TTypePtr>> fields)
        : Fields(std::move(fields))
    {}

    std::string ToString() const override {
        std::string s = "struct { ";
        for (const auto& [name, type] : Fields) {
            s += name + ": " + (type ? std::string(type->TypeName()) : "unknown") + "; ";
        }
        s += "}";
        return s;
    }

    const std::string_view TypeName() const override {
        return TStructType::TypeId;
    }
};

// simd vector type, e.g. f32x4, i32x8, etc.
// examples: <vec f32 4>, <vec i32 8>
// arithmetic operations:
// - <vec type N> + <vec type N>  => <vec type N>
// - <vec type N> * <vec type N>  => <vec type N>
// - <vec type N> + <scalar type> => <vec type N> (multiply scalar to each element)
// - <vec type N> * <scalar type> => <vec type N> (multiply scalar to each element)
// bit operations:
// - <vec type N> & <vec type N>  => <vec type N>
// - <vec type N> | <vec type N>  => <vec type N>
// - <vec type N> ^ <vec type N>  => <vec type N>
// - <vec type N> & <scalar type> => <vec type N> (bitwise and scalar to each element)
// - <vec type N> | <scalar type> => <vec type N> (bitwise or scalar to each element)
// - <vec type N> ^ <scalar type> => <vec type N> (bitwise xor scalar to each element)
// - <vec type N> << <scalar type> => <vec type N> (shift left each element by scalar)
// - <vec type N> >> <scalar type> => <vec type N> (shift right each element by scalar)
// - <vec type N> << <vec type N>  => <vec type N> (shift left each element by corresponding element)
// - <vec type N> >> <vec type N>  => <vec type N> (shift right each element by corresponding element)
// logical operations:
// - <vec type N> && <scalar type> => <vec i1 N> (logical and scalar to each element)
// - <vec type N> || <scalar type> => <vec i1 N> (logical or scalar to each element)
// - <vec type N> && <vec type N>  => <vec i1 N> (logical and each element)
// - <vec type N> || <vec type N>  => <vec i1 N> (logical or each element)
// comparison operations:
// - <vec type N> == <scalar type> => <vec i1 N>
// - <vec type N> != <scalar type> => <vec i1 N>
// - <vec type N> <  <scalar type> => <vec i1 N>
// - <vec type N> <= <scalar type> => <vec i1 N>
// - <vec type N> >  <scalar type> => <vec i1 N>
// - <vec type N> >= <scalar type> => <vec i1 N>
// - <vec type N> == <vec type N>  => <vec i1 N>
// - <vec type N> != <vec type N>  => <vec i1 N>
// - <vec type N> <  <vec type N>  => <vec i1 N>
// - <vec type N> <= <vec type N>  => <vec i1 N>
// - <vec type N> >  <vec type N>  => <vec i1 N>
// - <vec type N> >= <vec type N>  => <vec i1 N>
struct TVectorType : TType {
    static constexpr const char* TypeId = "Vector";

    TTypePtr ElementType;
    int Size{0};
    std::string SizeParam;

    TVectorType() = default;

    explicit TVectorType(TTypePtr et, int size)
        : ElementType(std::move(et))
        , Size(size)
    {}

    std::string SizeText() const {
        return SizeParam.empty()
            ? std::to_string(Size)
            : SizeParam;
    }

    void BindSize(std::string_view value) {
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), Size);
        if (error != std::errc{} || end != value.data() + value.size()) {
            Size = 0;
        }
        SizeParam.clear();
    }

    std::string ToString() const override {
        return (ElementType ? std::string(ElementType->TypeName()) : "unknown") + "x" + SizeText();
    }

    const std::string_view TypeName() const override {
        return TVectorType::TypeId;
    }
};

// simd tensor type, e.g. f32x4x4, i32x8x8, etc.
// examples: <tensor f32 4 4>, <tensor i32 8 8>
struct TTensorType : TType {
    static constexpr const char* TypeId = "Tensor";

    TTypePtr ElementType;
    std::vector<int> Shape;

    TTensorType() = default;

    explicit TTensorType(TTypePtr et, std::vector<int> shape)
        : ElementType(std::move(et))
        , Shape(std::move(shape))
    {}

    std::string ToString() const override {
        std::string s = (ElementType ? std::string(ElementType->TypeName()) : "unknown");
        for (int dim : Shape) {
            s += "x" + std::to_string(dim);
        }
        return s;
    }

    const std::string_view TypeName() const override {
        return TTensorType::TypeId;
    }
};

std::ostream& operator<<(std::ostream& os, const TType& expr);

inline TTypePtr UnwrapReferenceType(TTypePtr type) {
    if (auto maybeRef = TMaybeType<TReferenceType>(type)) {
        return maybeRef.Cast()->ReferencedType;
    }
    return type;
}

inline TTypePtr UnwrapNamedType(TTypePtr type) {
    if (auto named = TMaybeType<TNamedType>(type)) {
        return named.Cast()->UnderlyingType;
    }
    return type;
}

inline bool IsFutureType(TTypePtr type) {
    return static_cast<bool>(TMaybeType<TFutureType>(type));
}

inline TTypePtr FutureResultType(TTypePtr type) {
    if (auto future = TMaybeType<TFutureType>(type)) {
        return future.Cast()->ResultType;
    }
    return nullptr;
}

inline TTypePtr WrapFutureType(TTypePtr type) {
    if (IsFutureType(type)) {
        return type;
    }
    return std::make_shared<TFutureType>(std::move(type));
}

inline std::string TypeDiagnosticName(const TTypePtr& type);

inline std::string GenericArgDiagnosticName(const TGenericArg& arg) {
    if (arg.Kind == TGenericArg::EKind::Type) {
        return TypeDiagnosticName(arg.Type);
    }
    return arg.Value;
}

inline std::string TypeDiagnosticName(const TTypePtr& type) {
    if (!type) {
        return "unknown";
    }
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        return integer.Cast()->ToString();
    }
    if (auto floating = TMaybeType<TFloatType>(type)) {
        return floating.Cast()->ToString();
    }
    if (TMaybeType<TBoolType>(type)) {
        return "bool";
    }
    if (TMaybeType<TStringType>(type)) {
        return "string";
    }
    if (TMaybeType<TSymbolType>(type)) {
        return "char";
    }
    if (TMaybeType<TVoidType>(type)) {
        return "void";
    }
    if (auto future = TMaybeType<TFutureType>(type)) {
        return "Future<" + TypeDiagnosticName(future.Cast()->ResultType) + ">";
    }
    if (auto array = TMaybeType<TArrayType>(type)) {
        return "[" + TypeDiagnosticName(array.Cast()->ElementType) + "; "
            + (array.Cast()->Arity > 0 ? std::to_string(array.Cast()->Arity) : "?") + "]";
    }
    if (auto pointer = TMaybeType<TPointerType>(type)) {
        return "*" + TypeDiagnosticName(pointer.Cast()->PointeeType);
    }
    if (auto reference = TMaybeType<TReferenceType>(type)) {
        return "&" + TypeDiagnosticName(reference.Cast()->ReferencedType);
    }
    if (auto function = TMaybeType<TFunctionType>(type)) {
        std::string result = "(";
        for (size_t i = 0; i < function.Cast()->ParamTypes.size(); ++i) {
            if (i != 0) {
                result += ", ";
            }
            result += TypeDiagnosticName(function.Cast()->ParamTypes[i]);
        }
        result += ") -> ";
        result += TypeDiagnosticName(function.Cast()->ReturnType);
        return result;
    }
    if (auto named = TMaybeType<TNamedType>(type)) {
        auto value = named.Cast()->Name;
        if (!named.Cast()->TypeArgs.empty()) {
            value += "[";
            for (size_t i = 0; i < named.Cast()->TypeArgs.size(); ++i) {
                if (i != 0) {
                    value += ", ";
                }
                value += GenericArgDiagnosticName(named.Cast()->TypeArgs[i]);
            }
            value += "]";
        }
        if (named.Cast()->UnderlyingType) {
            value += " (" + TypeDiagnosticName(named.Cast()->UnderlyingType) + ")";
        }
        return value;
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::string result = "struct { ";
        for (const auto& [name, fieldType] : structure.Cast()->Fields) {
            result += name + ": " + TypeDiagnosticName(fieldType) + "; ";
        }
        result += "}";
        return result;
    }
    if (auto vector = TMaybeType<TVectorType>(type)) {
        return TypeDiagnosticName(vector.Cast()->ElementType) + "x" + vector.Cast()->SizeText();
    }
    if (auto tensor = TMaybeType<TTensorType>(type)) {
        std::string result = TypeDiagnosticName(tensor.Cast()->ElementType);
        for (int dim : tensor.Cast()->Shape) {
            result += "x" + std::to_string(dim);
        }
        return result;
    }
    return std::string(type->TypeName());
}

// Stable string key for a type, used in cast/operator maps.
// Named types include their name to distinguish компл from цвет.
inline std::string TypeKey(const TTypePtr& t) {
    if (!t) return "unknown";
    if (auto integer = TMaybeType<TIntegerType>(t)) return integer.Cast()->ToString();
    if (auto floating = TMaybeType<TFloatType>(t)) return floating.Cast()->ToString();
    if (auto named = TMaybeType<TNamedType>(t)) {
        auto value = std::string("Named::") + named.Cast()->Name;
        if (!named.Cast()->TypeArgs.empty()) {
            value += "[";
            for (size_t i = 0; i < named.Cast()->TypeArgs.size(); ++i) {
                if (i != 0) {
                    value += ",";
                }
                const auto& arg = named.Cast()->TypeArgs[i];
                value += arg.Kind == TGenericArg::EKind::Type
                    ? TypeKey(arg.Type)
                    : "Value::" + arg.Value;
            }
            value += "]";
        }
        return value;
    }
    if (auto future = TMaybeType<TFutureType>(t)) return std::string("Future::") + TypeKey(future.Cast()->ResultType);
    if (auto array = TMaybeType<TArrayType>(t)) {
        return "Array::" + std::to_string(array.Cast()->Arity) + "::" + TypeKey(array.Cast()->ElementType);
    }
    if (auto pointer = TMaybeType<TPointerType>(t)) return std::string("Ptr::") + TypeKey(pointer.Cast()->PointeeType);
    if (auto reference = TMaybeType<TReferenceType>(t)) return std::string("Ref::") + TypeKey(reference.Cast()->ReferencedType);
    if (auto function = TMaybeType<TFunctionType>(t)) {
        std::string key = "Fun::(";
        for (size_t i = 0; i < function.Cast()->ParamTypes.size(); ++i) {
            if (i > 0) key += ",";
            key += TypeKey(function.Cast()->ParamTypes[i]);
        }
        key += ")->";
        key += TypeKey(function.Cast()->ReturnType);
        return key;
    }
    if (auto structure = TMaybeType<TStructType>(t)) {
        std::string key = "Struct::{";
        for (const auto& [name, type] : structure.Cast()->Fields) {
            key += name + ":" + TypeKey(type) + ";";
        }
        key += "}";
        return key;
    }
    if (auto vector = TMaybeType<TVectorType>(t)) {
        return "Vector::" + TypeKey(vector.Cast()->ElementType) + "::" + vector.Cast()->SizeText();
    }
    if (auto tensor = TMaybeType<TTensorType>(t)) {
        std::string key = "Tensor::" + TypeKey(tensor.Cast()->ElementType);
        for (int dim : tensor.Cast()->Shape) {
            key += "::" + std::to_string(dim);
        }
        return key;
    }
    return std::string(t->TypeName());
}

inline std::string TypeMangleKey(const TTypePtr& t);

inline std::string GenericArgMangleKey(const TGenericArg& arg) {
    if (arg.Kind == TGenericArg::EKind::Type) {
        return TypeMangleKey(arg.Type);
    }
    return "Value_" + arg.Value;
}

inline std::string TypeMangleKey(const TTypePtr& t) {
    if (!t) return "unknown";
    if (auto integer = TMaybeType<TIntegerType>(t)) return integer.Cast()->ToString();
    if (auto floating = TMaybeType<TFloatType>(t)) return floating.Cast()->ToString();
    if (TMaybeType<TBoolType>(t)) return "bool";
    if (TMaybeType<TStringType>(t)) return "string";
    if (TMaybeType<TSymbolType>(t)) return "char";
    if (TMaybeType<TVoidType>(t)) return "void";
    if (auto named = TMaybeType<TNamedType>(t)) {
        auto src = named.Cast();
        std::string key = src->Name;
        for (const auto& arg : src->TypeArgs) {
            key += "_" + GenericArgMangleKey(arg);
        }
        return key;
    }
    if (auto future = TMaybeType<TFutureType>(t)) {
        return std::string("Future_") + TypeMangleKey(future.Cast()->ResultType);
    }
    if (auto array = TMaybeType<TArrayType>(t)) {
        return "Array" + std::to_string(array.Cast()->Arity) + "_" + TypeMangleKey(array.Cast()->ElementType);
    }
    if (auto pointer = TMaybeType<TPointerType>(t)) {
        return std::string("Ptr_") + TypeMangleKey(pointer.Cast()->PointeeType);
    }
    if (auto reference = TMaybeType<TReferenceType>(t)) {
        return std::string("Ref_") + TypeMangleKey(reference.Cast()->ReferencedType);
    }
    if (auto function = TMaybeType<TFunctionType>(t)) {
        std::string key = "Fun_" + TypeMangleKey(function.Cast()->ReturnType) + "__";
        for (const auto& paramType : function.Cast()->ParamTypes) {
            key += "_" + TypeMangleKey(paramType);
        }
        return key;
    }
    if (auto structure = TMaybeType<TStructType>(t)) {
        std::string key = "Struct";
        for (const auto& [_, type] : structure.Cast()->Fields) {
            key += "_" + TypeMangleKey(type);
        }
        return key;
    }
    if (auto vector = TMaybeType<TVectorType>(t)) {
        return "Vector_" + TypeMangleKey(vector.Cast()->ElementType) + "_" + vector.Cast()->SizeText();
    }
    if (auto tensor = TMaybeType<TTensorType>(t)) {
        std::string key = "Tensor_" + TypeMangleKey(tensor.Cast()->ElementType);
        for (int dim : tensor.Cast()->Shape) {
            key += "_" + std::to_string(dim);
        }
        return key;
    }
    return std::string(t->TypeName());
}

inline std::string MangleFunctionSignature(
    std::string_view prefixName,
    const TTypePtr& returnType,
    const std::vector<TTypePtr>& paramTypes)
{
    std::string mangled(prefixName);
    mangled += "$";
    mangled += TypeMangleKey(returnType);
    mangled += "__";
    for (const auto& paramType : paramTypes) {
        mangled += "$";
        mangled += TypeMangleKey(paramType);
    }
    return mangled;
}

inline std::string MangleGenericInstance(
    std::string_view name,
    const TTypePtr& returnType,
    const std::vector<TTypePtr>& paramTypes)
{
    std::string prefix = "__generic_";
    prefix += name;
    return MangleFunctionSignature(prefix, returnType, paramTypes);
}

} // namespace NAst
} // namespace NQumir
