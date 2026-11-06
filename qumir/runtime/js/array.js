"use strict";

// Minimal array runtime for WASM linear memory
// - array_create(size: i32|i64) -> i32 pointer to zeroed block
// - array_destroy(ptr: i32|i64) -> no-op (bump-only allocator)
// Strategy: grow memory per allocation and place the block at the previous end.
// This avoids coordination with other arenas and guarantees non-overlap.

let MEMORY = null; // WebAssembly.Memory
const PAGE = 65536; // 64 KiB
// Track allocated blocks: ptr -> size
const ALLOC_MAP = new Map();
// Sorted free-list: array of {n, ptr}, ordered by (n, ptr)
const FREE_LIST = [];

// Import string runtime so we can call str_release when freeing array elements
import * as StringRuntime from './string.js';

export function __bindMemory(mem) {
  MEMORY = mem;
  __resetArrays();
}
export function __resetArrays() { ALLOC_MAP.clear(); FREE_LIST.length = 0; }

// Binary search helpers for FREE_LIST sorted by (n, ptr)
function comparePair(aN, aPtr, bN, bPtr) {
  if (aN < bN) return -1;
  if (aN > bN) return 1;
  if (aPtr < bPtr) return -1;
  if (aPtr > bPtr) return 1;
  return 0;
}

function freeListInsert(n, ptr) {
  let lo = 0, hi = FREE_LIST.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    const e = FREE_LIST[mid];
    const cmp = comparePair(n, ptr, e.n, e.ptr);
    if (cmp <= 0) hi = mid; else lo = mid + 1;
  }
  // insert at lo
  FREE_LIST.splice(lo, 0, { n, ptr });
}

function freeListRemove(n, ptr) {
  // find exact pair via binary search
  let lo = 0, hi = FREE_LIST.length - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >>> 1;
    const e = FREE_LIST[mid];
    const cmp = comparePair(n, ptr, e.n, e.ptr);
    if (cmp === 0) {
      // remove by shifting (splice)
      FREE_LIST.splice(mid, 1);
      return true;
    }
    if (cmp < 0) hi = mid - 1; else lo = mid + 1;
  }
  return false;
}

// Find first index in FREE_LIST with e.n >= n (lower_bound). Returns index or -1.
function freeListLowerBound(n) {
  let lo = 0, hi = FREE_LIST.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (FREE_LIST[mid].n < n) lo = mid + 1; else hi = mid;
  }
  return lo < FREE_LIST.length ? lo : -1;
}

function toU32(v) {
  return (typeof v === 'bigint') ? Number(v & 0xFFFFFFFFn) >>> 0 : Number(v) >>> 0;
}

function pagesFor(bytes) {
  return Math.ceil(bytes / PAGE);
}

// Allocate a zeroed block of given size (in bytes) and return its pointer.
export function array_create(size) {
  if (!MEMORY) {
    throw new Error("array_create called before __bindMemory");
  }
  const n = toU32(size);
  if (n === 0) return 0;
  // Try to reuse a freed block: find lower_bound by size.
  const reuseIdx = freeListLowerBound(n);
  if (reuseIdx !== -1) {
    const entry = FREE_LIST.splice(reuseIdx, 1)[0];
    const reusePtr = entry.ptr;
    console.log(`array_create: reusing freed block of size ${entry.n} at ptr ${reusePtr} for request ${n}`);
    // mark allocated again
    ALLOC_MAP.set(reusePtr, entry.n);
    // zero the requested portion to emulate fresh allocation
    let u8 = new Uint8Array(MEMORY.buffer);
    const end = reusePtr + n;
    if (end > u8.length) {
      // should not normally happen; try to grow memory to accommodate
      const needBytes = end - u8.length;
      const extraPages = pagesFor(needBytes);
      if (extraPages > 0) MEMORY.grow(extraPages);
      u8 = new Uint8Array(MEMORY.buffer);
    }
    u8.fill(0, reusePtr, end);
    return reusePtr;
  }
  const oldBytes = MEMORY.buffer.byteLength >>> 0;
  const need = n;
  const extra = pagesFor(need);
  // Grow memory by the required number of pages; new buffer will be larger.
  if (extra > 0) MEMORY.grow(extra);
  // After grow, buffer is reallocated; zero the allocated region.
  const u8 = new Uint8Array(MEMORY.buffer);
  // Newly grown area starts at oldBytes and is zero-initialized per spec in most engines,
  // but we explicitly zero to be safe and to support engines that don't.
  // Note: If engine already zeroes, this is still correct and cheap in JS.
  u8.fill(0, oldBytes, oldBytes + need);
  const ptr = oldBytes >>> 0;
  // remember allocation size by pointer
  ALLOC_MAP.set(ptr, n);
  return ptr;
}

// Free is a no-op for bump-only allocator.
// Free is a no-op for bump-only allocator, but remove tracking entry.
export function array_destroy(_ptr) {
  const p = toU32(_ptr);
  const size = ALLOC_MAP.get(p);
  if (typeof size === 'number' && size > 0) {
    // insert freed block into sorted free-list
    freeListInsert(size, p);
  }
  ALLOC_MAP.delete(p);
}

export function array_str_destroy(ptr, elements) {
  // Walk the cells from ptr, one pointer wide each (a BigInt ptr means wasm64),
  // read an int32 at each cell and call string.str_release(handle).
  const stride = (typeof ptr === 'bigint') ? 8 : 4;
  const p = toU32(ptr);
  const n = toU32(elements) * stride;
  if (n === 0) return;
  const i32 = new Int32Array(MEMORY.buffer);
  const start = p >>> 0;
  const end = (start + n) >>> 0;
  for (let off = start; off + 4 <= end; off += stride) {
    const idx = off >>> 2;
    if (idx >= 0 && idx < i32.length) {
      const handle = i32[idx];
      StringRuntime.str_release(handle);
    }
  }

  array_destroy(ptr);
}