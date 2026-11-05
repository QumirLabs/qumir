// Insert insertStr (as string handle or C-string) into *strPtrPtr at 1-based symbol position insertSymbolPos
// strPtrPtr: address (number) of the variable holding the handle (int32* in WASM memory)
// insertStrPtr: handle or pointer to string to insert
export function str_insert_symbols(insertStrPtr, strPtrPtr, insertSymbolPos) {
    if (!MEMORY) return;
    const u32 = new Uint32Array(MEMORY.buffer);
    const ptrAddr = Number(strPtrPtr) >>> 0;
    // Read the handle (signed int32) from memory
    let handle = u32[ptrAddr >>> 2] | 0;
    const s = loadString(handle);
    const insertStr = loadString(insertStrPtr);
    if (!s) {
        u32[ptrAddr >>> 2] = allocHandle(insertStr);
        return;
    }
    if (!insertStr) {
        // Nothing to insert
        u32[ptrAddr >>> 2] = allocHandle(s);
        return;
    }
    const arr = Array.from(s);
    const insertArr = Array.from(insertStr);
    const len = arr.length;
    let pos = Number(insertSymbolPos);
    if (!Number.isFinite(pos)) pos = 1;
    if (pos < 1) pos = 1;
    if (pos > len + 1) pos = len + 1;
    // Insert at position (1-based): before symbol at pos, or at end if pos > len
    const out = arr.slice(0, pos - 1).concat(insertArr, arr.slice(pos - 1));
    u32[ptrAddr >>> 2] = allocHandle(out.join(''));
}
// Delete symbols from startSymbol (1-based) for count symbols
// strPtrPtr: address (number) of the variable holding the handle (int32* in WASM memory)
export function str_delete_symbols(strPtrPtr, startSymbol, count) {
    if (!MEMORY) return;
    const u32 = new Uint32Array(MEMORY.buffer);
    const ptrAddr = Number(strPtrPtr) >>> 0;
    // Read the handle (signed int32) from memory
    let handle = u32[ptrAddr >>> 2] | 0;
    const s = loadString(handle);
    if (!s) {
        u32[ptrAddr >>> 2] = allocHandle('');
        return;
    }
    const arr = Array.from(s);
    const len = arr.length;
    let start = Number(startSymbol);
    let cnt = Number(count);
    if (!Number.isFinite(start) || !Number.isFinite(cnt)) {
        u32[ptrAddr >>> 2] = allocHandle(s);
        return;
    }
    if (start < 1) start = 1;
    if (cnt < 1) {
        u32[ptrAddr >>> 2] = allocHandle(s);
        return;
    }
    let end = start + cnt - 1;
    if (end > len) end = len;
    if (start > end) {
        u32[ptrAddr >>> 2] = allocHandle(s);
        return;
    }
    // Remove symbols in [start-1, end-1] (0-based)
    const out = arr.slice(0, start - 1).concat(arr.slice(end));
    u32[ptrAddr >>> 2] = allocHandle(out.join(''));
}
// Replace symbol at 1-based index symIdx with newSym (codepoint)
export function str_replace_sym(strPtr, newSym, symIdx) {
    const s = loadString(strPtr);
    if (!s) return allocHandle('');
    const entry = isJsHandle(strPtr) ? STRING_POOL.get(strPtr) : null;
    const positions = entry ? ensureSymbolPositions(entry) : null;
    const len = positions ? positions.length : Array.from(s).length;
    const idx = Number(symIdx);
    if (!Number.isFinite(idx) || idx < 1 || idx > len) {
        // Out of bounds: return original string (retain if JS handle)
        if (isJsHandle(strPtr)) {
            retainHandle(strPtr);
            return strPtr;
        }
        return allocHandle(s);
    }
    // Find UTF-16 indices for replacement
    let start, end;
    if (positions) {
        start = positions[idx - 1];
        end = (idx < len) ? positions[idx] : s.length;
    } else {
        // Fallback: use Array.from for generic string
        const arr = Array.from(s);
        arr[idx - 1] = String.fromCodePoint(Number(newSym));
        return allocHandle(arr.join(''));
    }
    // Replace using JS string API
    const before = s.slice(0, start);
    const after = s.slice(end);
    const replacement = String.fromCodePoint(Number(newSym));
    return allocHandle(before + replacement + after);
}
"use strict";

// JS string runtime with a JS-managed string pool and pointer handles.
// Contract:
// - Non-negative addresses (>= 0) are treated as C-strings in WASM linear memory.
// - Negative addresses (< 0) are JS strings stored in the pool ("handles").
// - str_retain/str_release maintain a refcount only for negative handles.

let MEMORY = null; // WebAssembly.Memory
const encoder = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
const decoder = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8') : null;

// JS string pool: handle (negative int32) -> { value: string, refs: number, len: number|null }
// JS string pool: handle (negative int32) -> {
//   value: string,
//   refs: number,
//   // Cached symbol positions for Unicode-safe operations:
//   // positions[i] is the UTF-16 index of the i-th symbol (0-based),
//   // and positions.length is the symbol-length of the string.
//   positions: Array<number> | null
// }
let NEXT_HANDLE = -1;
const STRING_POOL = new Map();

export function __bindMemory(mem) {
    MEMORY = mem;
    // Any new memory binding invalidates all existing JS string handles
    STRING_POOL.clear();
    NEXT_HANDLE = -1;
}

export function __resetStrings() {
    // Reset all JS strings within the current instance
    STRING_POOL.clear();
    NEXT_HANDLE = -1;
}

function log(...a){
  const msg = a.join(' ') + '\n';
  if (typeof process !== 'undefined' && process.stderr) {
    process.stderr.write(msg);
  } else if (typeof console !== 'undefined') {
    console.error(msg);
  }
}
// === Базовые утилиты ===

function isJsHandle(v) {
    return typeof v === 'number' && v < 0;
}

function allocHandle(str) {
    const h = NEXT_HANDLE | 0; // negative int32 handle
    NEXT_HANDLE = (NEXT_HANDLE - 1) | 0;
    STRING_POOL.set(h, { value: String(str), refs: 1, positions: null });
    return h;
}

function retainHandle(h) {
    const entry = STRING_POOL.get(h);
    if (entry) entry.refs++;
}

function releaseHandle(h) {
    const entry = STRING_POOL.get(h);
    if (!entry) return;
    entry.refs--;
    if (entry.refs <= 0) STRING_POOL.delete(h);
}

function readCString(addr) {
    if (!MEMORY || !decoder) return '';
    const u8 = new Uint8Array(MEMORY.buffer);
    let p = (Number(addr) >>> 0);
    if (p < 0 || p >= u8.length) return '';
    let end = p;
    while (end < u8.length && u8[end] !== 0) end++;
    return decoder.decode(u8.subarray(p, end));
}

function loadString(ptr) {
    if (isJsHandle(ptr)) {
        const entry = STRING_POOL.get(ptr);
        return entry ? entry.value : '';
    }
    return readCString(ptr);
}

// Ensure we have cached symbol positions for the given entry.
// positions[i] is the UTF-16 index of the i-th symbol (0-based).
function ensureSymbolPositions(entry) {
    if (!entry) return [];
    if (entry.positions) return entry.positions;
    const src = entry.value;
    const positions = [];
    let i = 0;
    while (i < src.length) {
        positions.push(i);
        const cp = src.codePointAt(i);
        i += cp > 0xFFFF ? 2 : 1;
    }
    entry.positions = positions;
    return positions;
}
function getSymbolLength(entry) {
    if (entry.len != null) return entry.len;
    const symbols = Array.from(entry.value);
    entry.len = symbols.length;
    return entry.len;
}

// Helper for other runtimes (e.g., IO) that need to interpret a pointer
// using the same rules as string functions: negative handles are JS strings
// from the pool, non-negative addresses are C-strings in linear memory.
export function __loadString(ptr) {
    return loadString(ptr);
}

export function __allocCString(value) {
    // For JS code: create a new JS string handle
    return allocHandle(value || '');
}

function writeI8(ptr, value) {
    if (!MEMORY) return;
    const u8 = new Uint8Array(MEMORY.buffer);
    const p = (Number(ptr) >>> 0);
    if (p >= 0 && p < u8.length) {
        u8[p] = value & 0xFF;
    }
}

// Runtime API (pointer-based)
// Copy a literal C-string from module data segment into the JS string pool
export function str_from_lit(ptr) {
    // Literal is stored as a C-string in linear memory
    const text = readCString(ptr);
    return allocHandle(text);
}
export function str_retain(ptr) {
    if (isJsHandle(ptr)) retainHandle(ptr);
}
export function str_release(ptr) {
    if (isJsHandle(ptr)) releaseHandle(ptr);
}
export function str_concat(a, b) {
    const sa = loadString(a);
    const sb = loadString(b);
    return allocHandle(sa + sb);
}
export function str_slice(strPtr, startSymbol, endSymbol) {
    // 1-indexed, endSymbol is inclusive
    const s = loadString(strPtr);

    if (isJsHandle(strPtr)) {
        const entry = STRING_POOL.get(strPtr);
        const src = entry ? entry.value : s;
        const positions = ensureSymbolPositions(entry || { value: src, positions: null });
        const len = positions.length;

        // Convert from 1-indexed to 0-indexed symbol indices
        const startSym = Math.max(0, Math.min(len, Number(startSymbol) - 1));
        let endSym = Number(endSymbol) - 1;
        if (!Number.isFinite(endSym)) endSym = len - 1;
        endSym = Math.max(-1, Math.min(len - 1, endSym));
        if (startSym > endSym) return allocHandle('');

        const startIndex = positions[startSym] ?? src.length;
        const endIndex = (endSym + 1 < len) ? positions[endSym + 1] : src.length;
        if (startIndex >= src.length || startIndex >= endIndex) return allocHandle('');
        return allocHandle(src.slice(startIndex, endIndex));
    }

    // Fallback for C-strings in linear memory: keep the simpler, generic path
    const symbols = Array.from(s);
    const len = symbols.length;
    // Convert from 1-indexed to 0-indexed
    const start = Math.max(0, Math.min(len, Number(startSymbol) - 1));
    let endInc = Number(endSymbol) - 1;
    if (!Number.isFinite(endInc)) endInc = len - 1;
    endInc = Math.max(-1, Math.min(len - 1, endInc));
    if (start > endInc) return allocHandle('');
    const out = symbols.slice(start, endInc + 1).join('');
    return allocHandle(out);
}
export function str_compare(a, b) {
	const sa = loadString(a);
	const sb = loadString(b);
    if (sa === sb) return BigInt(0);
    return sa < sb ? BigInt(-1) : BigInt(1);
}
export function str_len(ptr) {
    if (isJsHandle(ptr)) {
        const entry = STRING_POOL.get(ptr);
        if (!entry) return BigInt(0);
        const positions = ensureSymbolPositions(entry);
        return BigInt(positions.length);
    }
    const s = readCString(ptr);
    return BigInt(Array.from(s).length);
}
// Return codepoint of symbol at 1-based position `pos` or -1 if out of range (matches C++ str_symbol_at semantics returning int32)
export function str_symbol_at(strPtr, pos) {
    const s = loadString(strPtr);
    if (!s) return -1;
    const p = Number(pos);
    if (!Number.isFinite(p) || p < 1) return -1;

    // Fast path for JS-pool strings: use cached symbol positions
    if (isJsHandle(strPtr)) {
        const entry = STRING_POOL.get(strPtr);
        const src = entry ? entry.value : s;
        const positions = ensureSymbolPositions(entry || { value: src, positions: null });
        const len = positions.length;
        if (p > len) return -1;
        const idx = positions[p - 1];
        if (idx == null || idx >= src.length) return -1;
        return src.codePointAt(idx) ?? -1;
    }

    // Fallback for C-strings: simple generic implementation
    const symbols = Array.from(s);
    if (p > symbols.length) return -1;
    return symbols[p - 1].codePointAt(0);
}
export function str_from_unicode(codepoint) {
    const cp = Number(codepoint);
    if (!Number.isFinite(cp) || cp < 0 || cp > 0x10FFFF) return allocHandle('');
    return allocHandle(String.fromCodePoint(cp));
}
export function str_str(needlePtr, haystackPtr) {
    const needle = loadString(needlePtr);
    const haystack = loadString(haystackPtr);
    const index = haystack.indexOf(needle);
    // need 1-indexed symbol position, 0 if not found
    if (index === -1) return BigInt(0);
    let symbolIndex = 0;
    for (let i = 0; i < index; ) {
        const cp = haystack.codePointAt(i);
        i += cp > 0xFFFF ? 2 : 1;
        symbolIndex++;
    }
    symbolIndex++; // convert to 1-based
    return BigInt(symbolIndex);
}
export function str_str_from(startSymbolPos, needlePtr, haystackPtr) {
    const startPos = Number(startSymbolPos);
    if (!Number.isFinite(startPos) || startPos < 1) return BigInt(0);
    const needle = loadString(needlePtr);
    const haystack = loadString(haystackPtr);
    // Convert symbol position to string index
    let symbolIndex = 1;
    let strIndex = 0;
    // Walk symbols to find byte/JS index that corresponds to startPos (1-based symbol index)
    while (strIndex < haystack.length && symbolIndex < startPos) {
        const cp = haystack.codePointAt(strIndex);
        strIndex += cp > 0xFFFF ? 2 : 1;
        symbolIndex++;
    }
    const index = haystack.indexOf(needle, strIndex);
    // need 1-indexed symbol position (from start of string), 0 if not found
    if (index === -1) return BigInt(0);

    // Count symbols from beginning of haystack up to the found index,
    // same semantics as str_str: position in characters, 1-based.
    let symbolPos = 0;
    for (let i = 0; i < index; ) {
        const cp = haystack.codePointAt(i);
        i += cp > 0xFFFF ? 2 : 1;
        symbolPos++;
    }
    symbolPos++; // convert to 1-based
    return BigInt(symbolPos);
}

// Numeric conversions / formatting (mirror C++ runtime semantics)
export function str_from_double(x) {
    // C++ uses std::ostringstream with precision(15) and default format
    // toPrecision(15) matches that for most values (scientific for extremes)
    const num = Number(x);
    return allocHandle(num.toPrecision(15));
}

export function str_from_int(x) {
    // Accept Number or BigInt and produce decimal string
    if (typeof x === 'bigint') {
        return allocHandle(x.toString());
    }
    const n = Math.trunc(Number(x));
    return allocHandle(String(n));
}

export function str_to_double(strPtr, outOkPtr) {
    const s = loadString(strPtr);
    if (!s) {
        if (outOkPtr) writeI8(outOkPtr, 0);
        return 0.0;
    }
    const v = Number(s.trim());
    const ok = Number.isFinite(v);
    if (outOkPtr) writeI8(outOkPtr, ok ? 1 : 0);
    return ok ? v : 0.0;
}

export function str_to_int(strPtr, outOkPtr) {
    const s = loadString(strPtr);
    if (!s) {
        if (outOkPtr) writeI8(outOkPtr, 0);
        return BigInt(0);
    }
    // Trim and validate integer format (optional leading sign + digits)
    const t = s.trim();
    const m = /^[-+]?\d+$/.test(t);
    if (!m) {
        if (outOkPtr) writeI8(outOkPtr, 0);
        return BigInt(0);
    }
    try {
        const bi = BigInt(t);
        if (outOkPtr) writeI8(outOkPtr, 1);
        return bi;
    } catch {
        if (outOkPtr) writeI8(outOkPtr, 0);
        return BigInt(0);
    }
}