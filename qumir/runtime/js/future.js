'use strict';

// JS-managed future table for the WASM async runtime.
//
// Contract (mirrors string.js pattern):
//   Negative i32 handles  → JS-managed futures in TABLE
//   Non-negative values   → not used; all handles come from here
//
// Two sub-kinds:
//   { caller, done }                           — JS-created (robot/turtle/painter op)
//   { caller, done, coroPtr, resultSize }      — wrapped child WASM coro (__qumir_wrap_coro)

let MEMORY      = null;  // WebAssembly.Memory (set by __bindMemory)
let wasmExports = null;  // WASM instance.exports (set by __bindWasm)

let NEXT_H = -1;
const TABLE = new Map();

// Pending op queue: { h, execute } — JS-created futures waiting to be executed.
const pendingOps = [];

// ---------------------------------------------------------------------------
// Lifecycle bindings (called from app.js after instantiation)

export function __bindMemory(mem) {
    MEMORY = mem;
}

export function __bindWasm(exports) {
    wasmExports = exports;
}

export function __resetFutures() {
    TABLE.clear();
    NEXT_H = -1;
    pendingOps.length = 0;
}

// ---------------------------------------------------------------------------
// Helpers for robot/turtle/painter modules

export function allocFuture() {
    const h = NEXT_H | 0;
    NEXT_H = (NEXT_H - 1) | 0;
    TABLE.set(h, { caller: 0, done: false, result: undefined });
    return h;
}

export function enqueuePendingOp(op) {
    pendingOps.push(op);
}

export function hasPendingOp() {
    return pendingOps.length > 0;
}

export function shiftPendingOp() {
    return pendingOps.shift();
}

// Marks future as done and resumes the waiting WASM coroutine.
export function resolveFuture(h, result = undefined) {
    const e = TABLE.get(h);
    if (!e || e.done) return;
    e.result = result;
    e.done = true;
    if (e.caller && wasmExports && wasmExports.__qumir_coro_resume) {
        wasmExports.__qumir_coro_resume(e.caller);
    }
}

// ---------------------------------------------------------------------------
// WASM imports — exported here so app.js can spread them into env.
// The WASM coroutine calls these as imported functions.

// Wrap a child WASM coroutine handle in a JS-managed future.
export function __qumir_wrap_coro(wasm_ptr, result_size) {
    const h = NEXT_H | 0;
    NEXT_H = (NEXT_H - 1) | 0;
    TABLE.set(h, {
        caller: 0,
        done: false,
        coroPtr: wasm_ptr | 0,
        resultSize: Number(result_size),
    });
    return h;
}

export function __qumir_future_await_ready(h) {
    const e = TABLE.get(h);
    if (!e) return 1;
    if (e.coroPtr !== undefined && wasmExports && wasmExports.__qumir_coro_done) {
        e.done = wasmExports.__qumir_coro_done(e.coroPtr) !== 0;
    }
    return e.done ? 1 : 0;
}

export function __qumir_future_await_suspend(h, caller) {
    const e = TABLE.get(h);
    if (!e) return 0;
    e.caller = caller | 0;
    // For wrapped child coro: drive it one step so the parent can poll.
    if (e.coroPtr !== undefined && wasmExports && wasmExports.__qumir_coro_resume
            && wasmExports.__qumir_coro_done) {
        if (!wasmExports.__qumir_coro_done(e.coroPtr)) {
            wasmExports.__qumir_coro_resume(e.coroPtr);
        }
    }
    return 0; // noop_coroutine address
}

export function __qumir_future_await_resume(h, result_ptr) {
    const e = TABLE.get(h);
    if (!e) return;
    if (e.coroPtr === undefined) {
        if (!result_ptr || !MEMORY || e.result === undefined) return;
        const view = new DataView(MEMORY.buffer);
        view.setBigInt64(result_ptr >>> 0, BigInt(e.result), true);
        return;
    }
    if (!result_ptr || !MEMORY || !e.resultSize) return;
    if (!wasmExports || !wasmExports.__qumir_coro_promise_ptr) return;
    const src = wasmExports.__qumir_coro_promise_ptr(e.coroPtr) >>> 0;
    if (!src) return;
    const dst  = result_ptr >>> 0;
    const size = e.resultSize;
    new Uint8Array(MEMORY.buffer, dst, size)
        .set(new Uint8Array(MEMORY.buffer, src, size));
}

export function __qumir_future_destroy(h) {
    const e = TABLE.get(h);
    if (!e) return;
    if (e.coroPtr !== undefined && wasmExports && wasmExports.__qumir_coro_destroy) {
        wasmExports.__qumir_coro_destroy(e.coroPtr);
    }
    TABLE.delete(h);
}

// Returns the underlying WASM coro frame ptr (used by lowerAwaitFuture via
// llvm.coro.promise to extract non-void child coro results).
export function __qumir_future_address(h) {
    const e = TABLE.get(h);
    return (e && e.coroPtr !== undefined) ? (e.coroPtr | 0) : 0;
}

// These are also imported by WASM for completeness (e.g. process_events path).
export function __qumir_future_done(h) {
    return __qumir_future_await_ready(h);
}

export function __qumir_future_resume(h) {
    const e = TABLE.get(h);
    if (!e || e.done) return;
    if (e.coroPtr !== undefined && wasmExports && wasmExports.__qumir_coro_done
            && wasmExports.__qumir_coro_resume) {
        if (!wasmExports.__qumir_coro_done(e.coroPtr)) {
            wasmExports.__qumir_coro_resume(e.coroPtr);
        }
        e.done = wasmExports.__qumir_coro_done(e.coroPtr) !== 0;
    }
}
