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

// Common compiler-rt builtins that may be referenced when --allow-undefined is used.
// Minimal shims to satisfy linkage; adjust if exact semantics are needed.
export function __multi3(a, b) {
  // return lower 64 bits of 128-bit product
  const MOD64 = (1n << 64n) - 1n;
  return (BigInt(a) * BigInt(b)) & MOD64;
}
