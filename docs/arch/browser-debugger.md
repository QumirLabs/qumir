# Browser debugger and coroutines

The browser debugger runs on the main thread using the existing coroutine and
Future ABI. The compiler emits calls to the WASM wrapper
`__qumir_debug_point(pointId, address)` and a `qumir.debug` custom section. No browser suspension extension or new
IR opcode is required.

## Why coroutines instead of JSPI

JSPI (JavaScript Promise Integration) depends on `WebAssembly.Suspending` and
`WebAssembly.promising`. Their browser support is limited, particularly on mobile
browsers, so relying on them makes debugging unavailable to part of the audience.

Qumir already has a coroutine/Future runtime that uses ordinary WebAssembly and
JavaScript. Lowering debug calls to `await @symbol` in the IR, and propagating
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
enabled, the pass also treats calls to the synchronous checkpoint factory as
asynchronous operations. The factory itself stays synchronous.
It discovers the transitive caller closure with a reverse call graph and a queue,
visiting each edge once, including forward calls and recursive cycles. Each
changed call becomes `await @symbol` by changing its opcode in place. Its
arguments, destination, result type, instruction storage and debug locations stay
unchanged. `await %future` continues to wait on an already produced Future.
Existing explicit Future calls retain
their semantics. Instruction debug locations are preserved. Optimization passes
that promote locals are skipped for debug points so variable addresses stay live.

LLVM codegen shares one ready/suspend loop and a continuation dispatcher across
all awaits in a function. Void awaits also share their completion and Future
destruction path. Each checkpoint keeps its original position; only the waiting
machinery is shared. This avoids generating a separate coroutine suspension state
and several waiting blocks for every instrumented instruction. Debug artifacts
use machine code optimization level zero as well as unoptimized source functions.

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

## State in WASM

`qumir/modules/debug.oz` is automatically loaded for `--debug-points`, using the
existing source module loader. It is installed with the compiler's other `.oz`
modules. Its functions are excluded from instrumentation, and their internals
can be optimized while user functions keep their debuggable storage.

The library stores its enabled flag, step mode, selected frame id, recursive
frame stack, live variable addresses and per-point breakpoint flags in WASM
globals and buffers. JS initializes point descriptors once. Control exports
`__qumir_debug_control(mode, target)` and `__qumir_debug_breakpoint(line, enabled)`
update this state; modes are continue=0, into=1, over=2, out=3, pause=4, stop=5.
`__qumir_debug_frame_data()` and `__qumir_debug_depth()` expose a snapshot for
the paused UI. The frame buffer grows for deep recursion.

The checkpoint wrapper is a synchronous factory returning the existing Future
ABI (`<ptr void>`). A non-stopping point returns zero. LLVM's common await loop
recognizes that value and continues without calling any JS Future helpers. When
a pause is required, `__qumir_debug_slow(id, address, reason)` synchronously
returns a JS Future handle; the calling user coroutine awaits that handle. The
asynchronous operation is represented by the Future, without allocating a WASM
coroutine frame for the wrapper. Reason 1 requests a pause; reason 2 requests Stop;
reason 3 yields to the browser without pausing.

Ordinary Run and Debug both use `ExecutionSession` to drive the existing Future
queue. The WASM checkpoint also samples `__qumir_debug_now` (the browser's monotonic
clock) every 1024 points, at an instruction or statement. After 10 ms of execution,
it returns a yielding Future. The session yields through a browser task, allowing
Pause/Stop input to run even in a pure computation loop. A pending Pause stops at
that checkpoint before computation resumes. The interval is reset at call entry,
after a session timer fires, and on resuming a debugger pause. Synchronous operations
and already settled Promises preserve the budget because they do not let browser
input run. Other external waits can cause an extra timed yield on return to WASM.
The interval is approximate: a clock sample or a long instruction can delay it.
Stop also cancels an already pending Future, then discards the instance.

`Debugger` reads the WASM stack and variable addresses only when paused; the
WASM library makes breakpoint and stepping decisions. `ValuePrinter` reads live
WASM memory; `DebuggerUI` binds CodeMirror, controls, stack, locals and hover.
Hover is enabled only while paused, with codepoint positions converted to
CodeMirror UTF16 offsets. Editing is locked during debugging.

The spinner beside Run/Debug covers both server compilation and browser runtime
preparation. It is cleared on success, compilation failure and cancellation.
On narrow screens it keeps the spinner and hides its text to leave room for Stop.

## Checks

Build `qumirc`, `server`, `test_debug_async` and `test_llvm_debug_info` with Ninja.
The IR tests check recursive caller propagation, result types, idempotence,
existing Future calls and the synchronous checkpoint factory. The LLVM test checks all four debug flag combinations
and argument packet integrity. `test/test_exec.js` compares WASM execution with
existing goldens. Run it with `--runtime service/static/runtime`.

Run `node test/test_browser_debugger.mjs` from the repository root for the browser
test. It starts the C++ server and headless Chrome with suspension extensions
disabled; `QUMIR_TEST_URL` can select a running server. A paused screenshot is
written to `build/browser_debugger.png`. The test also exercises deep WASM frame
buffer growth, asserts zero stopping JS checkpoints during an uninterrupted Continue,
checks timed yields and Pause/Stop in a loop with no asynchronous operations,
verifies that synchronous and microtask-only graphics operations preserve the budget,
and checks the compilation spinner on success, failure and cancellation at
desktop and mobile sizes.
