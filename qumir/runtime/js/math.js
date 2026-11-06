// JS runtime shims for math/system functions imported by wasm
// Keep names exactly as in MangledName fields. IO functions moved to io.js

export function sqrt(x) { return Math.sqrt(x); }

export function sin(x) { return Math.sin(x); }
export function cos(x) { return Math.cos(x); }
export function tan(x) { return Math.tan(x); }
export function cotan(x) { return 1.0 / Math.tan(x); }
export function asin(x) { return Math.asin(x); }
export function acos(x) { return Math.acos(x); }
export function atan(x) { return Math.atan(x); }
export function log(x) { return Math.log(x); }
export function log10(x) { return Math.log10 ? Math.log10(x) : Math.log(x) / Math.LN10; }
export function exp(x) { return Math.exp(x); }

export function div_qum(a, b) {
  const ai = BigInt(a), bi = BigInt(b);
  if (bi === 0n) return 0n;
  // Mathematical division: floor-style so that
  // a = div_qum(a,b) * b + mod_qum(a,b) with 0 <= mod < |b|.
  let q = ai / bi; // trunc towards zero for BigInt
  let r = ai % bi;
  if (r !== 0n && ((r > 0n) !== (bi > 0n))) {
    q -= 1n;
  }
  return q;
}
export function mod_qum(a, b) {
  const ai = BigInt(a), bi = BigInt(b);
  if (bi === 0n) return 0n;
  // Remainder consistent with mathematical division above:
  // 0 <= mod_qum(a,b) < |b| and a = div_qum(a,b) * b + mod_qum(a,b).
  let r = ai % bi;
  if (r !== 0n && ((r > 0n) !== (bi > 0n))) {
    r += bi;
  }
  return r;
}

export function fpow(a, n) {
  // a: number (double), n: int
  return Math.pow(a, Number(n));
}

export function pow(a, b) { return Math.pow(a, b); }

export function trunc_double(x) { return (x < 0 ? BigInt(Math.ceil(x)) : BigInt(Math.floor(x))); }

export function rand_double(x) { return Math.random() * x; }
export function rand_double_range(a, b) { return a + Math.random() * (b - a); }
export function rand_int64(x) { return BigInt(Math.floor(Math.random() * Number(x))); }
export function rand_int64_range(a, b) {
  const A = BigInt(a), B = BigInt(b);
  if (B <= A) return A;
  const span = B - A;
  const r = BigInt(Math.floor(Math.random() * Number(span)));
  return A + r;
}

let MEMORY = null; // WebAssembly.Memory, bound by io.js __bindMemory

export function __bindMemory(mem) {
  MEMORY = mem;
}

// compiler-rt builtins that LLVM calls instead of inline code and that are left
// undefined by --allow-undefined.

// 128-bit multiplication, e.g. in the closed form LLVM substitutes for a loop sum
// at -O1 and above. The operands come as (low, high) 64-bit halves; the low 128
// bits of the product are stored at ret as two little-endian 64-bit words.
export function __multi3(ret, aLo, aHi, bLo, bHi) {
  const a = (BigInt.asUintN(64, aHi) << 64n) | BigInt.asUintN(64, aLo);
  const b = (BigInt.asUintN(64, bHi) << 64n) | BigInt.asUintN(64, bLo);
  const product = BigInt.asUintN(128, a * b);
  const dv = new DataView(MEMORY.buffer);
  const at = Number(ret);
  dv.setBigUint64(at, BigInt.asUintN(64, product), true);
  dv.setBigUint64(at + 8, product >> 64n, true);
}
