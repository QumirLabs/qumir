# Browser debugger and coroutines

The browser debugger runs on the main thread using the existing coroutine and
Future ABI. The compiler emits calls to `__qumir_debug_point(pointId, address)`
and a `qumir.debug` WASM custom section. No browser suspension extension or new
IR opcode is required.

## Why coroutines instead of JSPI

JSPI (JavaScript Promise Integration) depends on `WebAssembly.Suspending` and
`WebAssembly.promising`. Their browser support is limited, particularly on mobile
browsers, so relying on them makes debugging unavailable to part of the audience.

Qumir already has a coroutine/Future runtime that uses ordinary WebAssembly and
JavaScript. Lowering debug calls to `call` plus `await` in the IR, and propagating
that change through callers, lets execution and debugger pauses use that same
mechanism. This removes the JSPI dependency, separate runtime import variants,
async mode selection and browser feature gates. The browser tests explicitly
disable both JSPI APIs to verify that stepping and suspension still work.

## Compiler options

- `-g` collects instruction, function and local metadata for VM/LLVM.
- `--debug-points` emits browser instrumentation and its descriptors at O0.
- These options are independent; shared metadata is collected when either is on.
- All targets use the coroutine/Future mechanism. There is no async mode flag.

Points support wasm32 and both ordinary and coroutine user functions. Arrays
and structures have opaque value displays. Native DWARF emission still excludes
coroutine functions; browser descriptors do not have that restriction.

## Boundaries

`TDebugPointEmitter` owns instrumentation. Instruction points use the existing
instruction wrapper; declarations use a local wrapper. Synthetic calls use
the builder directly. Calls are instrumented before the entire argument packet
and after the call; no point is inserted between `arg` and `call`, or before a
`phi`. Statement points include loop conditions on every visit. Synthetic module
initialization/cleanup functions do not emit checkpoints themselves.

`LowerDebugAsync` runs on the complete module after AST lowering, including at
O0 and without debug points. Functions containing `await`, including synthetic
module initialization/cleanup, are promoted to coroutines. When debug points are
enabled, the pass also changes the debug import's return type to a Future pointer.
It discovers the transitive caller closure to a fixed point and promotes callers
that need to await newly asynchronous calls. This handles forward calls, recursion and mutually recursive cycles.
Each affected call becomes a call producing a Future followed by `await` with
the original destination and result type. Existing explicit Future calls retain
their semantics. Instruction debug locations are preserved. Optimization passes
that promote locals are skipped for debug points so variable addresses stay live.

The custom section holds functions, lexical scope parents, AST local types,
source identifier bindings and point descriptors. A variable point passes the
address of its live storage, including argument storage in the coroutine frame.
Source bindings identify locals by id so shadowed names remain distinguishable.
Coroutine module constructors/destructors are exported and named in
`qumir.runtime` so the host can await them before/after the entry point. The
entry remains the first user function in declaration order, regardless of its
name; metadata preserves that order across coroutine splitting and linking.
Metadata records whether this specific entry is a coroutine, independently of
other functions in the module. Its result type lets the host read the promise
before destroying the frame.

Ordinary Run and Debug both use `ExecutionSession`, which schedules debug imports
in the same JS future queue as executor operations. It drives WASM through `__qumir_future_*`, owns cancellation,
animation pacing and event-loop yields. Checkpoints can pause on a Promise;
Stop cancels the session and discards the instance. Debugging is available
without a browser feature check.

`Debugger` owns recursive frames, breakpoints and stepping. `ValuePrinter`
reads live WASM memory; `DebuggerUI` binds CodeMirror, controls, stack, locals
and hover. Hover is enabled only while paused, with codepoint positions
converted to CodeMirror UTF16 offsets. Editing is locked during debugging.

## Checks

Build `qumirc`, `server`, `test_debug_async` and `test_llvm_debug_info` with Ninja.
The IR tests check recursive caller propagation, result types, idempotence and
existing Future calls. The LLVM test checks all four debug flag combinations
and argument packet integrity. `test/test_exec.js` compares WASM execution with
existing goldens. Run it with `--runtime service/static/runtime`.

Run `node test/test_browser_debugger.mjs` from the repository root for the browser
test. It starts the C++ server and headless Chrome with suspension extensions
disabled; `QUMIR_TEST_URL` can select a running server. A paused screenshot is
written to `build/browser_debugger.png`.
