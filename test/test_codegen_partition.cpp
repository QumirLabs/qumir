#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_codegen.h>
#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/codegen/llvm/llvm_runner.h>
#include <qumir/ir/builder.h>
#include <qumir/ir/type.h>
#include <qumir/ir/eval.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <type_traits>

using namespace NQumir;
using namespace NQumir::NIR;
using namespace NQumir::NIR::NLiterals;

namespace {

// A module with two trivial void functions "A" and "B".
void BuildTwoFuns(NIR::TModule& module) {
    NIR::TBuilder b(module);
    int voidTy = module.Types.I(EKind::Void);
    auto mk = [&](std::string name, int symId) {
        b.NewFunction(std::move(name), {}, symId); // creates the entry block
        b.SetReturnType(voidTy);
        b.Emit0("ret"_op, {});
    };
    mk("A", 1);
    mk("B", 2);
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::vector<std::string> Emit(const NCodeGen::TLLVMCodeGenOptions& opts) {
    NIR::TModule module;
    BuildTwoFuns(module);
    NCodeGen::TLLVMCodeGen cg(opts);
    auto art = cg.Emit(module);
    return art->GetDefinedFunctionNames();
}

void BuildVectorKernel(TModule& module, EKind kind, int count, TOp opcode, int broadcast, EKind castKind = EKind::Void) {
    TBuilder builder(module);
    const int elementType = module.Types.I(kind);
    const int vectorType = module.Types.Vec(elementType, count);
    const int pointerType = module.Types.Ptr(vectorType);
    const bool comparison = opcode == "<"_op || opcode == ">"_op || opcode == "<="_op
        || opcode == ">="_op || opcode == "=="_op || opcode == "!="_op || opcode == "!"_op
        || opcode == "i2b"_op || opcode == "f2b"_op;
    const bool reduction = opcode == "all"_op || opcode == "any"_op;
    const bool unary = opcode == "!"_op || opcode == "~"_op || reduction || castKind != EKind::Void
        || opcode == "i2b"_op || opcode == "f2b"_op;
    int resultType = reduction
        ? module.Types.I(EKind::I1)
        : (comparison ? module.Types.Vec(module.Types.I(EKind::I1), count) : vectorType);
    if (castKind != EKind::Void) {
        resultType = module.Types.Vec(module.Types.I(castKind), count);
    }
    const int outputPointerType = module.Types.Ptr(resultType);
    const int i64 = module.Types.I(EKind::I64);
    builder.NewFunction("kernel", {TLocal{0}, TLocal{1}, TLocal{2}, TLocal{3}}, 1);
    builder.SetReturnType(module.Types.I(EKind::Void));
    builder.SetType(TLocal{0}, outputPointerType, {});
    for (int i = 1; i < 3; ++i) {
        builder.SetType(TLocal{i}, pointerType, {});
    }
    builder.SetType(TLocal{3}, elementType, {});
    builder.SetType(TLocal{4}, resultType, {});
    auto emit = [&](TOp op, std::initializer_list<TOperand> operands, int type) {
        auto tmp = builder.Emit1(op, operands);
        builder.SetType(tmp, type);
        return tmp;
    };
    auto output = emit("load"_op, {TLocal{0}}, outputPointerType);
    auto leftPtr = emit("load"_op, {TLocal{1}}, pointerType);
    auto rightPtr = emit("load"_op, {TLocal{2}}, pointerType);
    auto left = emit("lde"_op, {leftPtr}, vectorType);
    auto right = emit("lde"_op, {rightPtr}, vectorType);
    auto scalar = emit("load"_op, {TLocal{3}}, elementType);
    TTmp result;
    if (opcode == "shuffle"_op) {
        std::vector<int> mask(count);
        for (int i = 0; i < count; ++i) {
            mask[i] = count - i - 1;
        }
        module.VectorShuffleMasks.push_back(std::move(mask));
        result = emit(opcode, {left, TImm{0, i64}}, resultType);
    } else if (unary) {
        result = emit(opcode, {left}, resultType);
    } else {
        result = emit(opcode, {broadcast == 1 ? scalar : left, broadcast == 2 ? scalar : right}, resultType);
    }
    builder.Emit0("stre"_op, {TLocal{4}, result});
    auto reloaded = emit("load"_op, {TLocal{4}}, resultType);
    auto copied = emit("mov"_op, {reloaded}, resultType);
    builder.Emit0("ste"_op, {output, copied});
    auto tail = emit("+"_op, {output, TImm{module.Types.SizeInBytes(resultType), i64}}, outputPointerType);
    auto zero = emit("mov"_op, {TImm{0, resultType}}, resultType);
    builder.Emit0("ste"_op, {tail, zero});
    builder.Emit0("ret"_op, {});
}

template<typename T>
void CheckVectorKernels(EKind kind, int count = 4) {
    SCOPED_TRACE(static_cast<int>(kind));
    std::vector<T> left(count);
    std::vector<T> right(count);
    for (int i = 0; i < count; ++i) {
        left[i] = static_cast<T>(i * 7 + 1);
        right[i] = static_cast<T>(i * 3 + 2);
    }
    if constexpr (std::is_integral_v<T>) {
        left[0] = std::numeric_limits<T>::max();
        left[1] = std::numeric_limits<T>::min();
        right[1] = static_cast<T>(-1);
    } else {
        left[0] = -static_cast<T>(0);
        right[0] = static_cast<T>(0);
        left[1] = std::numeric_limits<T>::denorm_min();
        right[1] = static_cast<T>(-1);
        left[2] = std::numeric_limits<T>::infinity();
        right[2] = -std::numeric_limits<T>::infinity();
        left[3] = std::numeric_limits<T>::quiet_NaN();
    }
    for (int optLevel : {0, 3}) {
        for (auto opcode : {"+"_op, "-"_op, "*"_op}) {
            for (int broadcast : {0, 1, 2}) {
                SCOPED_TRACE(optLevel);
                SCOPED_TRACE(opcode.ToString());
                SCOPED_TRACE(broadcast);
                TModule module;
                BuildVectorKernel(module, kind, count, opcode, broadcast);
                NCodeGen::TLLVMCodeGen codegen;
                NCodeGen::TLlvmRunner runner;
                std::string error;
                auto* address = runner.Lookup(codegen.Emit(module, optLevel), "kernel", &error);
                ASSERT_NE(address, nullptr) << error;
                using TKernel = void (*)(T*, const T*, const T*, T);
                std::vector<T> output(count * 2, static_cast<T>(42));
                reinterpret_cast<TKernel>(address)(output.data(), left.data(), right.data(), static_cast<T>(5));
                using TCalc = std::conditional_t<std::is_floating_point_v<T>, T, uint64_t>;
                for (int i = 0; i < count; ++i) {
                    const TCalc a = (broadcast == 1)
                        ? 5
                        : static_cast<TCalc>(left[i]);
                    const TCalc b = (broadcast == 2)
                        ? 5
                        : static_cast<TCalc>(right[i]);
                    TCalc expected;
                    switch (opcode) {
                        case "+"_op: expected = a + b; break;
                        case "-"_op: expected = a - b; break;
                        case "*"_op: expected = a * b; break;
                        default: FAIL() << "Unexpected opcode";
                    }
                    if constexpr (std::is_floating_point_v<T>) {
                        if (std::isnan(expected)) {
                            EXPECT_TRUE(std::isnan(output[i]));
                        } else {
                            EXPECT_EQ(output[i], expected);
                            if (expected == 0) {
                                EXPECT_EQ(std::signbit(output[i]), std::signbit(expected));
                            }
                        }
                    } else {
                        EXPECT_EQ(output[i], static_cast<T>(expected));
                    }
                    EXPECT_EQ(output[count + i], 0);
                }
            }
        }
    }
}


template<typename T>
void CheckVectorOperations(EKind kind, int count = 4) {
    std::vector<T> left(count);
    std::vector<T> right(count);
    const bool boolean = kind == EKind::I1;
    const T scalar = boolean
        ? 1
        : 5;
    for (int i = 0; i < count; ++i) {
        left[i] = static_cast<T>(i * 7);
        right[i] = static_cast<T>(i * 3 + 1);
        if (boolean) {
            left[i] = static_cast<T>(i % 2);
            right[i] = static_cast<T>((i / 2) % 2);
        }
    }
    if (!boolean) {
        left[0] = std::numeric_limits<T>::max();
        left[1] = std::numeric_limits<T>::lowest();
        right[1] = static_cast<T>(-1);
        right[2] = static_cast<T>(sizeof(T) * 8);
        if constexpr (std::is_floating_point_v<T>) {
            left[0] = std::numeric_limits<T>::quiet_NaN();
            left[1] = -std::numeric_limits<T>::infinity();
            left[2] = -static_cast<T>(0);
            left[3] = std::numeric_limits<T>::infinity();
        }
    }
    std::vector<TOp> ops = {"<"_op, ">"_op, "<="_op, ">="_op, "=="_op, "!="_op, "!"_op, "shuffle"_op};
    ops.push_back(std::is_floating_point_v<T> ? "f2b"_op : "i2b"_op);
    if constexpr (std::is_integral_v<T>) {
        ops.insert(ops.end(), {"&"_op, "|"_op, "^"_op});
        if (boolean) {
            ops.insert(ops.end(), {"&&"_op, "||"_op, "all"_op, "any"_op});
        } else {
            ops.insert(ops.end(), {"<<"_op, ">>"_op, "~"_op});
        }
    }
    for (int optLevel : {0, 3}) {
        for (auto opcode : ops) {
            const bool comparison = opcode == "<"_op || opcode == ">"_op || opcode == "<="_op
                || opcode == ">="_op || opcode == "=="_op || opcode == "!="_op || opcode == "!"_op
                || opcode == "i2b"_op || opcode == "f2b"_op;
            const bool reduction = opcode == "all"_op || opcode == "any"_op;
            const bool unary = opcode == "!"_op || opcode == "~"_op || opcode == "shuffle"_op || reduction
                || opcode == "i2b"_op || opcode == "f2b"_op;
            const int resultCount = reduction
                ? 1
                : count;
            const size_t resultSize = comparison || reduction
                ? resultCount
                : resultCount * sizeof(T);
            for (int broadcast : {0, 1, 2}) {
                if (unary && broadcast != 0) {
                    continue;
                }
                SCOPED_TRACE(static_cast<int>(kind));
                SCOPED_TRACE(optLevel);
                SCOPED_TRACE(opcode.ToString());
                SCOPED_TRACE(broadcast);
                std::vector<uint8_t> expected(resultSize * 2, 0);
                for (int i = 0; i < resultCount; ++i) {
                    const T a = broadcast == 1
                        ? scalar
                        : left[i];
                    const T b = broadcast == 2
                        ? scalar
                        : right[i];
                    bool mask = false;
                    switch (opcode) {
                        case "<"_op: mask = a < b; break;
                        case ">"_op: mask = a > b; break;
                        case "<="_op: mask = a <= b; break;
                        case ">="_op: mask = a >= b; break;
                        case "=="_op: mask = a == b; break;
                        case "!="_op: mask = a != b; break;
                        case "!"_op: mask = a == 0; break;
                        case "i2b"_op:
                        case "f2b"_op: mask = a != 0; break;
                        case "all"_op: mask = std::all_of(left.begin(), left.end(), [](T v) { return v != 0; }); break;
                        case "any"_op: mask = std::any_of(left.begin(), left.end(), [](T v) { return v != 0; }); break;
                        default: break;
                    }
                    if (comparison || reduction) {
                        expected[i] = mask;
                        continue;
                    }
                    T value = left[count - i - 1];
                    if constexpr (std::is_integral_v<T>) {
                        const auto ua = static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(a));
                        const auto shift = static_cast<uint64_t>(b) & (sizeof(T) * 8 - 1);
                        switch (opcode) {
                            case "&"_op:
                            case "&&"_op: value = static_cast<T>(a & b); break;
                            case "|"_op:
                            case "||"_op: value = static_cast<T>(a | b); break;
                            case "^"_op: value = static_cast<T>(a ^ b); break;
                            case "~"_op: value = static_cast<T>(~a); break;
                            case "<<"_op: value = static_cast<T>(ua << shift); break;
                            case ">>"_op:
                                value = std::is_signed_v<T>
                                    ? static_cast<T>(static_cast<int64_t>(a) >> shift)
                                    : static_cast<T>(ua >> shift);
                                break;
                            default: break;
                        }
                    }
                    std::memcpy(expected.data() + i * sizeof(T), &value, sizeof(T));
                }
                TModule module;
                BuildVectorKernel(module, kind, count, opcode, broadcast);
                NCodeGen::TLLVMCodeGen codegen;
                NCodeGen::TLlvmRunner runner;
                std::string error;
                auto* address = runner.Lookup(codegen.Emit(module, optLevel), "kernel", &error);
                ASSERT_NE(address, nullptr) << error;
                using TKernel = void (*)(void*, const T*, const T*, T);
                std::vector<uint8_t> output(resultSize * 2, 42);
                reinterpret_cast<TKernel>(address)(output.data(), left.data(), right.data(), scalar);
                EXPECT_EQ(output, expected) << "LLVM";

                std::fill(output.begin(), output.end(), 42);
                TVMCompiler compiler(module);
                std::ostringstream out;
                std::istringstream in;
                TInterpreter interpreter(module, compiler, out, in);
                int64_t scalarBits;
                if constexpr (std::is_floating_point_v<T>) {
                    scalarBits = sizeof(T) == 4
                        ? std::bit_cast<uint32_t>(static_cast<float>(scalar))
                        : std::bit_cast<int64_t>(static_cast<double>(scalar));
                } else {
                    scalarBits = static_cast<int64_t>(scalar);
                }
                interpreter.EvalRaw(module.Functions[0], {reinterpret_cast<int64_t>(output.data()),
                    reinterpret_cast<int64_t>(left.data()), reinterpret_cast<int64_t>(right.data()), scalarBits}, {});
                EXPECT_EQ(output, expected) << "VM";
            }
        }
    }
}

template<typename TSource, typename TResult>
void CheckVectorCast(EKind sourceKind, EKind resultKind,
    const std::array<TSource, 4>& input, const std::array<TResult, 4>& expected)
{
    SCOPED_TRACE(static_cast<int>(sourceKind));
    SCOPED_TRACE(static_cast<int>(resultKind));
    for (int optLevel : {0, 3}) {
        TModule module;
        const auto opcode = std::is_floating_point_v<TSource> == std::is_floating_point_v<TResult>
            ? "mov"_op
            : (std::is_floating_point_v<TSource> ? "f2i"_op : "i2f"_op);
        BuildVectorKernel(module, sourceKind, 4, opcode, 0, resultKind);
        std::array<TResult, 8> expectedWithTail{};
        std::copy(expected.begin(), expected.end(), expectedWithTail.begin());
        std::array<TResult, 8> output;
        output.fill(42);

        NCodeGen::TLLVMCodeGen codegen;
        NCodeGen::TLlvmRunner runner;
        std::string error;
        auto* address = runner.Lookup(codegen.Emit(module, optLevel), "kernel", &error);
        ASSERT_NE(address, nullptr) << error;
        using TKernel = void (*)(TResult*, const TSource*, const TSource*, TSource);
        reinterpret_cast<TKernel>(address)(output.data(), input.data(), input.data(), 0);
        EXPECT_EQ(output, expectedWithTail) << "LLVM";

        output.fill(42);
        TVMCompiler compiler(module);
        std::ostringstream out;
        std::istringstream in;
        TInterpreter interpreter(module, compiler, out, in);
        interpreter.EvalRaw(module.Functions[0], {reinterpret_cast<int64_t>(output.data()),
            reinterpret_cast<int64_t>(input.data()), reinterpret_cast<int64_t>(input.data()), 0}, {});
        EXPECT_EQ(output, expectedWithTail) << "VM";
    }
}

} // namespace

TEST(CodegenPartition, DefaultDefinesEverything) {
    auto defs = Emit({});
    EXPECT_TRUE(Has(defs, "A"));
    EXPECT_TRUE(Has(defs, "B"));
}

TEST(CodegenPartition, RestrictToDefinitionsDefinesOnlyListed) {
    std::unordered_set<std::string> only{"A"};
    NCodeGen::TLLVMCodeGenOptions opts;
    opts.RestrictToDefinitions = &only;
    auto defs = Emit(opts);
    EXPECT_TRUE(Has(defs, "A"));
    EXPECT_FALSE(Has(defs, "B")); // B is an external declaration
}

TEST(CodegenPartition, EmitAsExternalDeclaresListed) {
    std::unordered_set<std::string> ext{"A"};
    NCodeGen::TLLVMCodeGenOptions opts;
    opts.EmitAsExternal = &ext;
    auto defs = Emit(opts);
    EXPECT_FALSE(Has(defs, "A")); // A is an external declaration
    EXPECT_TRUE(Has(defs, "B"));
}

TEST(CodegenPartition, ImportedBodyIsAvailableButDoesNotOwnAnExport) {
    NIR::TModule module;
    BuildTwoFuns(module);
    std::unordered_set<std::string> only{"A"};
    std::unordered_set<std::string> imports{"B"};
    NCodeGen::TLLVMCodeGen codegen({
        .RestrictToDefinitions = &only,
        .InlineDefinitions = &imports,
    });
    auto artifacts = codegen.Emit(module, 0);
    EXPECT_EQ(artifacts->GetDefinedFunctionNames(), (std::vector<std::string>{"A"}));
    std::ostringstream ir;
    artifacts->PrintModule(ir);
    EXPECT_NE(ir.str().find("define available_externally void @B("), std::string::npos);
}

TEST(LLVMVector, DynamicArithmeticCopiesZerosAndBroadcasts) {
    CheckVectorKernels<int8_t>(EKind::I8);
    CheckVectorKernels<uint8_t>(EKind::U8);
    CheckVectorKernels<int16_t>(EKind::I16);
    CheckVectorKernels<uint16_t>(EKind::U16);
    CheckVectorKernels<int32_t>(EKind::I32);
    CheckVectorKernels<uint32_t>(EKind::U32);
    CheckVectorKernels<int64_t>(EKind::I64);
    CheckVectorKernels<uint64_t>(EKind::U64, 32);
    CheckVectorKernels<float>(EKind::F32);
    CheckVectorKernels<double>(EKind::F64);
}

TEST(LLVMVector, NativeArithmeticUsesSIMD) {
#if defined(__aarch64__) || defined(__x86_64__)
    for (auto kind : {EKind::I32, EKind::F64}) {
        for (auto opcode : {"+"_op, "-"_op, "*"_op}) {
            std::string mnemonic;
#if defined(__aarch64__)
            switch (opcode) {
                case "+"_op: mnemonic = "add"; break;
                case "-"_op: mnemonic = "sub"; break;
                case "*"_op: mnemonic = "mul"; break;
                default: FAIL() << "Unexpected opcode";
            }
            if (kind == EKind::F64) {
                mnemonic = "f" + mnemonic;
            }
            const std::string lanes = (kind == EKind::F64)
                ? "2d"
                : "4s";
            const std::string pattern = "\\b" + mnemonic + "(\\." + lanes + "\\s+v|\\s+v[0-9]+\\." + lanes + ")";
#else
            switch (opcode) {
                case "+"_op:
                    mnemonic = kind == EKind::F64
                        ? "addpd"
                        : "paddd";
                    break;
                case "-"_op:
                    mnemonic = kind == EKind::F64
                        ? "subpd"
                        : "psubd";
                    break;
                case "*"_op:
                    mnemonic = kind == EKind::F64
                        ? "mulpd"
                        : "pmul(ld|udq)";
                    break;
                default: FAIL() << "Unexpected opcode";
            }
            const std::string pattern = "\\bv?" + mnemonic + "\\s";
#endif
            for (int broadcast : {0, 1, 2}) {
                SCOPED_TRACE(mnemonic);
                SCOPED_TRACE(broadcast);
                TModule module;
                BuildVectorKernel(module, kind, kind == EKind::F64 ? 2 : 4, opcode, broadcast);
                NCodeGen::TLLVMCodeGen codegen;
                auto artifacts = codegen.Emit(module, 3);
                std::ostringstream assembly;
                artifacts->Generate(assembly, true, false);
                EXPECT_TRUE(std::regex_search(assembly.str(), std::regex(pattern))) << assembly.str();
            }
        }
    }
#else
    GTEST_SKIP() << "SIMD mnemonic checks cover AArch64 and x86-64";
#endif
}

TEST(LLVMVector, BooleanIndexUsesByteLanes) {
    for (int optLevel : {0, 3}) {
        for (int count : {2, 4, 8, 16, 32}) {
            SCOPED_TRACE(optLevel);
            SCOPED_TRACE(count);
            TModule module;
            TBuilder builder(module);
            const int boolean = module.Types.I(EKind::I1);
            const int vector = module.Types.Vec(boolean, count);
            const int pointer = module.Types.Ptr(vector);
            const int i64 = module.Types.I(EKind::I64);
            builder.NewFunction("index", {TLocal{0}, TLocal{1}}, 1);
            builder.SetReturnType(boolean);
            builder.SetType(TLocal{0}, pointer, {});
            builder.SetType(TLocal{1}, i64, {});
            auto address = builder.Emit1("load"_op, {TLocal{0}});
            builder.SetType(address, pointer);
            auto value = builder.Emit1("lde"_op, {address});
            builder.SetType(value, vector);
            auto index = builder.Emit1("load"_op, {TLocal{1}});
            builder.SetType(index, i64);
            auto element = builder.Emit1("index"_op, {value, index});
            builder.SetType(element, boolean);
            builder.Emit0("ret"_op, {element});
            NCodeGen::TLLVMCodeGen codegen;
            NCodeGen::TLlvmRunner runner;
            std::string error;
            auto* entry = runner.Lookup(codegen.Emit(module, optLevel), "index", &error);
            ASSERT_NE(entry, nullptr) << error;
            using TIndex = bool (*)(const uint8_t*, int64_t);
            std::array<uint8_t, 32> lanes{};
            for (int i = 0; i < count; ++i) {
                lanes[i] = static_cast<uint8_t>(i % 3 == 1);
            }
            for (int i = 0; i < count; ++i) {
                EXPECT_EQ(reinterpret_cast<TIndex>(entry)(lanes.data(), i), lanes[i] != 0);
            }
        }
    }
}

TEST(LLVMVector, InvalidIndexRaisesRuntimeError) {
    for (int optLevel : {0, 3}) {
        for (int64_t index : std::array<int64_t, 3>{-1, 2, std::numeric_limits<int64_t>::max()}) {
            SCOPED_TRACE(optLevel);
            SCOPED_TRACE(index);
            TModule module;
            TBuilder builder(module);
            const int i64 = module.Types.I(EKind::I64);
            const int vector = module.Types.Vec(i64, 2);
            builder.NewFunction("bounds", {}, 1);
            builder.SetReturnType(i64);
            auto value = builder.Emit1("mov"_op, {TImm{0, vector}});
            builder.SetType(value, vector);
            auto element = builder.Emit1("index"_op, {value, TImm{index, i64}});
            builder.SetType(element, i64);
            builder.Emit0("ret"_op, {element});
            NCodeGen::TLLVMCodeGen codegen;
            NCodeGen::TLlvmRunner runner;
            EXPECT_THROW(runner.Run(codegen.Emit(module, optLevel), "bounds"), std::runtime_error);
        }
    }
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(LLVMVector, BitwiseMasksReductionsAndShuffleAgreeWithVM) {
    CheckVectorOperations<int8_t>(EKind::I8);
    CheckVectorOperations<uint8_t>(EKind::U8);
    CheckVectorOperations<int16_t>(EKind::I16);
    CheckVectorOperations<uint16_t>(EKind::U16);
    CheckVectorOperations<int32_t>(EKind::I32);
    CheckVectorOperations<uint32_t>(EKind::U32);
    CheckVectorOperations<int64_t>(EKind::I64);
    CheckVectorOperations<uint64_t>(EKind::U64, 32);
    CheckVectorOperations<float>(EKind::F32);
    CheckVectorOperations<double>(EKind::F64);
    for (int count : {2, 4, 8, 16, 32}) {
        CheckVectorOperations<uint8_t>(EKind::I1, count);
    }
}

TEST(LLVMVector, NumericCastsAgreeWithVM) {
    CheckVectorCast<int8_t, int64_t>(EKind::I8, EKind::I64, {-128, -1, 0, 127}, {-128, -1, 0, 127});
    CheckVectorCast<uint8_t, uint64_t>(EKind::U8, EKind::U64, {0, 128, 255, 1}, {0, 128, 255, 1});
    CheckVectorCast<int64_t, int8_t>(EKind::I64, EKind::I8, {-129, -1, 256, 257}, {127, -1, 0, 1});
    CheckVectorCast<uint64_t, uint16_t>(EKind::U64, EKind::U16,
        {UINT64_MAX, 0, 65536, 65537}, {65535, 0, 0, 1});
    CheckVectorCast<int8_t, uint16_t>(EKind::I8, EKind::U16, {-128, -1, 0, 127}, {65408, 65535, 0, 127});
    CheckVectorCast<uint8_t, int16_t>(EKind::U8, EKind::I16, {0, 128, 255, 1}, {0, 128, 255, 1});
    CheckVectorCast<int32_t, double>(EKind::I32, EKind::F64,
        {INT32_MIN, -1, 0, INT32_MAX}, {-2147483648.0, -1, 0, 2147483647.0});
    CheckVectorCast<uint32_t, double>(EKind::U32, EKind::F64,
        {0, 1, 2147483648U, UINT32_MAX}, {0, 1, 2147483648.0, 4294967295.0});
    CheckVectorCast<double, int32_t>(EKind::F64, EKind::I32,
        {-17.75, -0.0, 0.5, 2147483647.0}, {-17, 0, 0, INT32_MAX});
    CheckVectorCast<double, uint32_t>(EKind::F64, EKind::U32,
        {0, 0.5, 4294967295.0, 2147483648.0}, {0, 0, UINT32_MAX, 2147483648U});
    CheckVectorCast<float, double>(EKind::F32, EKind::F64, {-1.5f, 0, 1.25f, 10}, {-1.5, 0, 1.25, 10});
    CheckVectorCast<double, float>(EKind::F64, EKind::F32, {-1.5, 0, 1.25, 10}, {-1.5f, 0, 1.25f, 10});
}

TEST(LLVMVector, NativeBitwiseMasksAndShuffleUseSIMD) {
#if defined(__aarch64__) || defined(__x86_64__)
    for (auto opcode : {"&"_op, "|"_op, "^"_op, "<<"_op, ">>"_op, "<"_op, "shuffle"_op}) {
        SCOPED_TRACE(opcode.ToString());
        TModule module;
        BuildVectorKernel(module, EKind::I32, 4, opcode, 0);
        NCodeGen::TLLVMCodeGen codegen;
        auto artifacts = codegen.Emit(module, 3);
        std::ostringstream assembly;
        artifacts->Generate(assembly, true, false);
        std::string mnemonic;
#if defined(__aarch64__)
        switch (opcode) {
            case "&"_op: mnemonic = "and"; break;
            case "|"_op: mnemonic = "orr"; break;
            case "^"_op: mnemonic = "eor"; break;
            case "<<"_op: mnemonic = "ushl|sshl"; break;
            case ">>"_op: mnemonic = "sshl"; break;
            case "<"_op: mnemonic = "cmgt"; break;
            case "shuffle"_op: mnemonic = "rev64|tbl"; break;
            default: FAIL() << "Unexpected opcode";
        }
        const auto pattern = "\\b(" + mnemonic + ")(\\.[0-9]+[bhsd]\\s+v|\\s+v[0-9]+\\.[0-9]+[bhsd])";
#else
        switch (opcode) {
            case "&"_op: mnemonic = "pand|andps|andpd"; break;
            case "|"_op: mnemonic = "por|orps|orpd"; break;
            case "^"_op: mnemonic = "pxor|xorps|xorpd"; break;
            case "<"_op: mnemonic = "pcmpgtd"; break;
            case "shuffle"_op: mnemonic = "pshufd|shufps"; break;
            default: continue; // Per-lane dynamic shifts can scalarize without AVX2.
        }
        const auto pattern = "\\bv?(" + mnemonic + ")\\s";
#endif
        EXPECT_TRUE(std::regex_search(assembly.str(), std::regex(pattern))) << assembly.str();
    }
#else
    GTEST_SKIP() << "SIMD mnemonic checks cover AArch64 and x86-64";
#endif
}
