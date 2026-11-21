// SPDX-FileCopyrightText: 2025 Contributors to TPDE <https://tpde.org>
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Instruction semantics for the TPDE backend; tpde_encodegen turns each function
// into an encode_<name>() method at build time. Most are adapted from
// tpde-llvm/src/encode_template.c of TPDE; the license text is in LICENSE.TPDE.

#include <stdbool.h>
#include <stdint.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef __int128_t i128;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef __uint128_t u128;

// loads / stores
u64 loadi8_zext(u8* p) {
    return *p;
}
u64 loadi16_zext(u16* p) {
    return *p;
}
u64 loadi32_zext(u32* p) {
    return *p;
}
u64 loadi64(u64* p) {
    return *p;
}
float loadf32(float* p) {
    return *p;
}
double loadf64(double* p) {
    return *p;
}

void storei8(u8* p, u32 v) {
    *p = (u8)v;
}
void storei16(u16* p, u32 v) {
    *p = (u16)v;
}
void storei32(u32* p, u32 v) {
    *p = v;
}
void storei64(u64* p, u64 v) {
    *p = v;
}
void storef32(float* p, float v) {
    *p = v;
}
void storef64(double* p, double v) {
    *p = v;
}

// integer arithmetic; narrower types are computed in 32 bits
u32 addi32(u32 a, u32 b) {
    return a + b;
}
u32 subi32(u32 a, u32 b) {
    return a - b;
}
u32 muli32(u32 a, u32 b) {
    return a * b;
}
u32 landi32(u32 a, u32 b) {
    return a & b;
}
u32 lori32(u32 a, u32 b) {
    return a | b;
}
u32 lxori32(u32 a, u32 b) {
    return a ^ b;
}
u32 shli32(u32 a, u32 b) {
    return a << (b & 31);
}
u32 shri32(u32 a, u32 b) {
    return a >> (b & 31);
}
i32 ashri32(i32 a, u32 b) {
    return a >> (b & 31);
}
u32 noti32(u32 a) {
    return ~a;
}
u32 negi32(u32 a) {
    return -a;
}

u64 addi64(u64 a, u64 b) {
    return a + b;
}
u64 subi64(u64 a, u64 b) {
    return a - b;
}
u64 muli64(u64 a, u64 b) {
    return a * b;
}
u64 landi64(u64 a, u64 b) {
    return a & b;
}
u64 lori64(u64 a, u64 b) {
    return a | b;
}
u64 lxori64(u64 a, u64 b) {
    return a ^ b;
}
u64 shli64(u64 a, u64 b) {
    return a << (b & 63);
}
u64 shri64(u64 a, u64 b) {
    return a >> (b & 63);
}
i64 ashri64(i64 a, u64 b) {
    return a >> (b & 63);
}
u64 noti64(u64 a) {
    return ~a;
}
u64 negi64(u64 a) {
    return -a;
}

u128 addi128(u128 a, u128 b) {
    return a + b;
}
u128 subi128(u128 a, u128 b) {
    return a - b;
}
u128 muli128(u128 a, u128 b) {
    return a * b;
}
u128 landi128(u128 a, u128 b) {
    return a & b;
}
u128 lori128(u128 a, u128 b) {
    return a | b;
}
u128 lxori128(u128 a, u128 b) {
    return a ^ b;
}
u128 shli128(u128 a, u64 b) {
    return a << (b & 127);
}
u128 shri128(u128 a, u64 b) {
    return a >> (b & 127);
}
i128 ashri128(i128 a, u64 b) {
    return a >> (b & 127);
}
u128 noti128(u128 a) {
    return ~a;
}
u128 negi128(u128 a) {
    return -a;
}

// i128 division calls __divti3 and friends with the divisor 1 where the divide
// would trap (CreateDefinedDivRem). One argument part per snippet: CallBuilder
// cannot evict a temporary that holds the next argument register.
static inline bool sdivtrap(u64 a0, u64 a1, u64 b0, u64 b1) {
    bool zero = (b0 | b1) == 0;
    bool minusOne = (b0 & b1) == ~(u64)0;
    bool min = (a0 | (a1 ^ ((u64)1 << 63))) == 0;
    return zero | (min & minusOne);
}
u64 sdivisorlo128(u64 a0, u64 a1, u64 b0, u64 b1) {
    return sdivtrap(a0, a1, b0, b1) ? 1 : b0;
}
u64 sdivisorhi128(u64 a0, u64 a1, u64 b0, u64 b1) {
    return sdivtrap(a0, a1, b0, b1) ? 0 : b1;
}
u64 udivisorlo128(u64 b0, u64 b1) {
    return b0 | ((b0 | b1) == 0);
}
// x / 0 == 0; the libcall divided by 1.
u128 divresi128(u128 q, u64 b0, u64 b1) {
    return (b0 | b1) == 0 ? 0 : q;
}

// bit counting (builtin::cttz/ctlz/ctpop, zero is not poison)
u64 cttzi64(u64 a) {
    return !a ? 64 : __builtin_ctzll(a);
}
u64 ctlzi64(u64 a) {
    return !a ? 64 : __builtin_clzll(a);
}
u64 ctpopi64(u64 a) {
    return __builtin_popcountll(a);
}

// float arithmetic
float addf32(float a, float b) {
    return a + b;
}
float subf32(float a, float b) {
    return a - b;
}
float mulf32(float a, float b) {
    return a * b;
}
float divf32(float a, float b) {
    return a / b;
}
float fnegf32(float a) {
    return -a;
}
double addf64(double a, double b) {
    return a + b;
}
double subf64(double a, double b) {
    return a - b;
}
double mulf64(double a, double b) {
    return a * b;
}
double divf64(double a, double b) {
    return a / b;
}
double fnegf64(double a) {
    return -a;
}

// conversions
float f64tof32(double a) {
    return (float)a;
}
double f32tof64(float a) {
    return (double)a;
}
i32 f32toi32(float a) {
    return (i32)a;
}
u32 f32tou32(float a) {
    return (u32)a;
}
i64 f32toi64(float a) {
    return (i64)a;
}
u64 f32tou64(float a) {
    return (u64)a;
}
i32 f64toi32(double a) {
    return (i32)a;
}
u32 f64tou32(double a) {
    return (u32)a;
}
i64 f64toi64(double a) {
    return (i64)a;
}
u64 f64tou64(double a) {
    return (u64)a;
}
float i32tof32(u32 a) {
    return (float)(i32)a;
}
float i64tof32(u64 a) {
    return (float)(i64)a;
}
float u32tof32(u32 a) {
    return (float)a;
}
float u64tof32(u64 a) {
    return (float)a;
}
double i32tof64(u32 a) {
    return (double)(i32)a;
}
double i64tof64(u64 a) {
    return (double)(i64)a;
}
double u32tof64(u32 a) {
    return (double)a;
}
double u64tof64(u64 a) {
    return (double)a;
}
// llvm.fptosi.sat over the result r of __fixdfti or __fixsfti, as in the LLVM
// backend: NaN gives 0, values out of range give the nearest bound.
i128 satf64toi128(double a, i128 r) {
    i128 max = (i128)(~(u128)0 >> 1);
    r = a < -0x1p127 ? -max - 1 : r;
    r = a >= 0x1p127 ? max : r;
    return a != a ? 0 : r;
}
i128 satf32toi128(float a, i128 r) {
    i128 max = (i128)(~(u128)0 >> 1);
    r = a < -0x1p127f ? -max - 1 : r;
    r = a >= 0x1p127f ? max : r;
    return a != a ? 0 : r;
}

// bit casts between register banks
u32 bitcastf32toi32(float a) {
    u32 r;
    __builtin_memcpy(&r, &a, 4);
    return r;
}
u64 bitcastf64toi64(double a) {
    u64 r;
    __builtin_memcpy(&r, &a, 8);
    return r;
}
float bitcasti32tof32(u32 a) {
    float r;
    __builtin_memcpy(&r, &a, 4);
    return r;
}
double bitcasti64tof64(u64 a) {
    double r;
    __builtin_memcpy(&r, &a, 8);
    return r;
}

i64 fill_with_sign64(i64 a) {
    return a >> 63;
}

// builtin::byte_match8/16 in SWAR, the same results as the NEON code of the LLVM backend.
// The masks are parameters: tpde_encodegen at 050927e decodes 64-bit logical
// immediates with a repeating element (ORRXri 0x0101...) as 32-bit ones.

// 0x80 marks each zero byte of x, without false positives.
static inline u64 zero_bytes(u64 x, u64 low7) {
    return ~(((x & low7) + low7) | x | low7);
}
// 0xff in every byte of p[0..8) equal to b; ones = 0x0101..., low7 = 0x7f7f...
u64 byte_match8(const u8* p, u32 b, u64 ones, u64 low7) {
    u64 w;
    __builtin_memcpy(&w, p, 8);
    return (zero_bytes(w ^ (ones * (u8)b), low7) >> 7) * 0xff;
}
// bit i set if p[i] == b, i < 16; gather = 0x0102040810204080
u32 byte_match16(const u8* p, u32 b, u64 ones, u64 low7, u64 gather) {
    u64 lo, hi;
    __builtin_memcpy(&lo, p, 8);
    __builtin_memcpy(&hi, p + 8, 8);
    u64 rep = ones * (u8)b;
    u64 zl = zero_bytes(lo ^ rep, low7) >> 7;
    u64 zh = zero_bytes(hi ^ rep, low7) >> 7;
    return (u32)((zl * gather) >> 56) | (u32)((zh * gather) >> 56) << 8;
}
