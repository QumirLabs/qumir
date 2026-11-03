# Coroutines

This note describes the coroutine pipeline: front-end typing, propagation,
AST `await` insertion, IR lowering, LLVM coroutine lowering, VM eval execution,
and browser/WebAssembly execution.

## Motivation

Robot, Turtle, and Painter commands may suspend so the browser runtime can
render movement step by step, or so the VM can process async events between
steps. In source code this behaves like a normal call. After semantic analysis
the AST makes suspension explicit:

```text
value := function()
```

where `function` is coroutine-typed becomes:

```text
value := await function()
```

Physically, if a function calls a coroutine it also becomes a coroutine
(`co_await` semantics), and the caller is marked `Future<T>`-returning too.

---

## Type model

`Future<T>` is an AST-level type. It marks a function whose execution may
suspend before producing `T`.

Runtime functions that can suspend are declared with `Future<RetType>` return
types. The coroutine annotation pass uses that return type directly.

User functions are not annotated as coroutines during normal type annotation.
First the ordinary type annotator assigns types, then a separate transform
pass rewrites coroutine types and call sites.

---

## Pipeline

Coroutine annotation is part of the transform pipeline:

1. Run name resolution.
2. Run type annotation.
3. Run `PostTypeAnnotationTransform`.
4. Run `CoroutineAnnotationTransform`.
5. If anything changed, repeat name resolution and type annotation.

This keeps the type annotator local: it only understands `TAwaitExpr` once
the transform has inserted it.

## Propagation

`CoroutineAnnotationTransform` builds a call graph and marks direct coroutine
callers:

- a function calls a runtime function returning `Future<T>`;
- a function calls another function returning `Future<T>`.

The mark propagates backwards. If `a` calls `b` and `b` is a coroutine,
`a` becomes a coroutine too, and its return type changes from `T` to `Future<T>`.

## Await insertion

```cpp
TAwaitExpr { TExprPtr Operand; }
```

The transform rewrites `(call f ...)` to `(await (call f ...))` when `f`
returns `Future<T>`. On the next type annotation iteration, `AnnotateAwait`
checks the operand has type `Future<T>` and sets the await expression type to
`T`.

---

## Two-level API

The coroutine system exposes two distinct API layers.

---

### Low-level API — `__qumir_coro_*`

Provided by `EmitCoroutineRuntimeHelpers` (LLVM codegen). These are thin
wrappers over LLVM coroutine intrinsics that cannot be called from outside an
LLVM module. They operate directly on raw coroutine frame pointers.

```c
// 1 if the coroutine is at final suspend (resume function pointer is null)
int   __qumir_coro_done        (void* frame);

// Resume the coroutine from its current suspension point
void  __qumir_coro_resume      (void* frame);

// Free the coroutine frame memory
void  __qumir_coro_destroy     (void* frame);

// Address of the promise / result slot inside the frame (offset 0)
void* __qumir_coro_promise_ptr (void* frame);
```

These implement the **coroutine frame ABI** exposed by LLVM. They are the
foundation on top of which higher-level abstractions such as `ITypeErasedFuture`
and `TWrappedLLVMCoro` are built. Callers outside the runtime should not use
them directly — use the high-level API below instead.

---

### High-level API — `__qumir_future_*`

Built on top of the low-level API. All awaitable objects are represented as
`ITypeErasedFuture*`. This interface is what compiled programs, executor
runtimes, and the event loop all use. It must be implemented for every new
awaitable type (external operations, child coroutines, etc.).

```c
// Lifetime
void  __qumir_future_destroy      (ITypeErasedFuture* future);

// Polling — used by executor event loops (process_events, JIT runner, JS loop)
bool  __qumir_future_done         (ITypeErasedFuture* future);
void  __qumir_future_resume       (ITypeErasedFuture* future);
void* __qumir_future_address      (ITypeErasedFuture* future);

// Await protocol — called from compiled program code (WASM coro / IR eval)
bool  __qumir_future_await_ready  (ITypeErasedFuture* future);
void* __qumir_future_await_suspend(ITypeErasedFuture* future, void* caller);
void  __qumir_future_await_resume (ITypeErasedFuture* future, void* result);

// Wrap a raw LLVM coroutine frame as an ITypeErasedFuture
ITypeErasedFuture* __qumir_wrap_coro(void* frame, size_t result_size);
```

#### Who calls what

**Executor side** (robot.js, turtle.js, painter.js; C++ `robot_process_events` etc.):

- Creates `ITypeErasedFuture*` objects and returns them to the program when
  an async operation is initiated (`robot_right()` returns a future).
- Calls `__qumir_future_done` / `__qumir_future_resume` to drive the event
  loop (C++ eval path).

**Program side** (the compiled Qumir program — WASM coroutine or IR eval):

- Calls `__qumir_future_await_ready` to check whether the future is complete.
- Calls `__qumir_future_await_suspend(future, caller)` to register the current
  coroutine handle as the continuation and yield.
- After resumption, calls `__qumir_future_await_resume(future, result_ptr)` to
  extract the optional result.
- Calls `__qumir_future_destroy` to release the future.

**LLVM lowering** (`lowerAwaitFuture` / `LowerCoroutineFunction`):

- Emits calls to the await protocol for every `await` IR instruction.
- Calls `__qumir_wrap_coro(frame, size)` immediately after a `call` to a user
  coroutine to wrap the raw frame in `ITypeErasedFuture*`, so the same await
  path is used for both external operations and child coroutines.
- Uses `__qumir_future_address(future)` to recover the child frame pointer
  and read its result via `llvm.coro.promise` in the `afterBB` block.

**Event loop** (C++ JIT runner, browser `app.js`):

- Wraps the top-level entry coro frame with `__qumir_wrap_coro(rawHandle, 0)`.
- Drives the loop through `__qumir_future_done`, `__qumir_future_resume`, and
  `__qumir_future_destroy` exclusively — no direct use of the low-level API.

---

## IR Lowering

`Future<T>` is an AST-only type. During lowering a coroutine function is
represented as a normal IR function with a physical pointer return type:

| Level | Value |
|---|---|
| Source / AST return type | `Future<T>` |
| IR function return type | `ptr<void>` — the coroutine handle |
| `IsCoroutine` flag | `true` |
| `CoroutineResultTypeId` | IR type-id of `T` (void for `Future<void>`) |

The lowerer emits **two separate IR instructions** for every awaited call:
a `call` that captures the returned `ITypeErasedFuture*`, followed by an
`await` that drives the await protocol:

```text
arg ...
%h = call f        ; returns ITypeErasedFuture* (ptr to void)
await %h           ; drives await_ready/suspend/resume/destroy
```

The `await` opcode is illegal in non-coroutine functions. It is consumed by
the LLVM and VM backends.

### IR Example

Source:

```kumir
использовать Робот

алг квадрат
нач
    закрасить
    вправо
    закрасить
кон
```

Robot actions return `Future<void>`, so `квадрат` becomes a coroutine.
Printed IR (shortened):

```text
function квадрат () { ; ptr to void coroutine result void
  block {
    label: label(0)
    call tmp(0,ptr to void) = закрасить
    await tmp(0,ptr to void)
    call tmp(1,ptr to void) = вправо
    await tmp(1,ptr to void)
    call tmp(2,ptr to void) = закрасить
    await tmp(2,ptr to void)
    jmp label(1)
  }
  block {
    label: label(1)
    ret
  }
}
```

The comment `; ptr to void coroutine result void` means:
- physical return type: `ptr to void` (the coroutine handle)
- result type stored in the promise: `void`

For `Future<Int>` the result metadata would be `Int` instead of `void`.

---

## LLVM Lowering

`TLLVMCodeGen::LowerFunction` dispatches coroutine functions to
`LowerCoroutineFunction`. Coroutine frames are allocated via `array_create`
(same allocator as Qumir arrays), which keeps the JS runtime import list
minimal.

The central piece is `lowerAwaitFuture`, which emits the await protocol for
**both** external futures (robot/turtle/painter) and wrapped child coroutines
using the same `__qumir_future_*` imports. No special cases at the LLVM level.

### `lowerAwaitFuture` — the unified await loop

For every `await %h` instruction, the following LLVM IR is emitted:

```llvm
; %future holds the ITypeErasedFuture* from the preceding call instruction

await.check.N:
  %ready = call i1 @__qumir_future_await_ready(ptr %future)
  br i1 %ready, label %after.await.N, label %await.suspend.N

await.suspend.N:
  call ptr @__qumir_future_await_suspend(ptr %future, ptr %coro.handle)
  %ready.after = call i1 @__qumir_future_await_ready(ptr %future)
  br i1 %ready.after, label %after.await.N, label %await.sleep.N

await.sleep.N:
  %s = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %s, label %suspend [
    i8 0, label %await.check.N     ; resumed → re-check
    i8 1, label %cleanup
  ]

after.await.N:
  ; for non-void result:
  %child.handle  = call ptr  @__qumir_future_address(ptr %future)
  %child.promise = call ptr  @llvm.coro.promise(ptr %child.handle, i32 0, i1 false)
  %result        = load <T>, ptr %child.promise
  ; for void result: nothing to load
  call void @__qumir_future_await_resume(ptr %future, ptr null)
  call void @__qumir_future_destroy(ptr %future)
```

`__qumir_future_await_suspend` stores `%coro.handle` as the continuation
(`Caller`) inside the future. When the executor resolves the future it calls
`__qumir_future_resume` which fires `ResumeCaller`, resuming the coroutine at
`await.check.N`.

### Child coroutine wrapping

When a `call` instruction targets a user coroutine (`IsCoroutine = true`),
`LowerCoroutineFunction` wraps the raw frame pointer immediately after the
call:

```llvm
%raw    = call ptr @child(...)                        ; raw coro frame
%future = call ptr @__qumir_wrap_coro(ptr %raw, i64 <result_bytes>)
; %future is ITypeErasedFuture* — fed to the following await
```

`TWrappedLLVMCoro` (returned by `__qumir_wrap_coro`) implements
`ITypeErasedFuture` using `std::coroutine_handle<>`, which is ABI-compatible
with LLVM coroutine frames. Its `await_suspend` drives the child one step
and returns `noop`. If the child finished in that step, the parent continues
without suspending, so one resume of the root carries a finished leaf through
the whole call chain. Otherwise the parent suspends and polls again from
`await.check.N` on its next resume.

### Coroutine frame helpers (`__qumir_coro_*`)

`EmitCoroutineRuntimeHelpers` generates four thin wrapper functions:

```text
__qumir_coro_done(ptr)        -> i32   ; llvm.coro.done
__qumir_coro_resume(ptr)      -> void  ; llvm.coro.resume
__qumir_coro_destroy(ptr)     -> void  ; llvm.coro.destroy
__qumir_coro_promise_ptr(ptr) -> ptr   ; llvm.coro.promise(h, i32 0, i1 false)
```

These are **not part of the public C API**. They exist solely because
`llvm.coro.*` are LLVM intrinsics that cannot be called directly from outside
the LLVM module — neither from C++ nor from JavaScript. The wrappers bridge
that gap:

- `TWrappedLLVMCoro` (the `__qumir_wrap_coro` result) uses
  `std::coroutine_handle<>` which calls the coro's resume/destroy function
  pointer directly through the C++ coroutine ABI, so it does **not** need
  these wrappers.
- The browser `future.js` uses them when implementing `__qumir_wrap_coro`
  as a JS import: it stores the raw coro frame pointer and must call back
  into WASM to check completion or extract the result.

In the C++ JIT runner the public `__qumir_future_*` API is used throughout.
The raw coro frame returned by the entry function is immediately wrapped via
`__qumir_wrap_coro`, and the event loop drives it through
`__qumir_future_done`, `__qumir_future_resume`, and `__qumir_future_destroy`
without ever touching `__qumir_coro_*`.

### Returning values

The physical LLVM function always returns the coroutine handle. On `ret value`,
the lowerer stores `value` into the promise alloca and branches to final
suspend:

```llvm
store <T> %val, ptr %coro.promise
br label %final
final:
  %sf = call i8 @llvm.coro.suspend(token none, i1 true)
  ...
```

After final suspend `__qumir_coro_done(handle)` returns true and the parent
can read the result via `__qumir_coro_promise_ptr(handle)`.

### Coroutine passes

LLVM coroutine intrinsics must be split before code emission:

```text
coro-early, coro-split, coro-elide, coro-cleanup
```

These passes run automatically:
- at `O1+` as part of the full optimization pipeline;
- at `O0` via a dedicated `RunCoroutinePasses()` call whenever the module
  contains coroutine functions.

This ensures the JIT and AOT paths both receive lowered (non-intrinsic) IR.

---

## VM / Eval Path

The IR interpreter (`TInterpreter`) handles coroutines through a C++-coroutine
event loop. `DoEvalAsync` is itself a C++ coroutine: it runs the instruction
loop and, when it encounters `EVMOp::AwaitVoid` or `EVMOp::Await`, suspends
via `co_await AwaitTypeErasedFuture<T>(future)`.

`DoEval` drives it:

```cpp
auto future = DoEvalAsync(function, args, options);
while (!future.done()) {
    size_t processed = ProcessAsyncRuntimeEvents();
    assert(processed > 0 && "coroutine suspended with no pending async events");
}
ProcessAsyncRuntimeEvents(); // flush batched calls
```

`ProcessAsyncRuntimeEvents` calls:

```cpp
robot_process_events()   // resolves pending robot futures, resumes coroutine
turtle_process_events()  // same for turtle
painter_process_events() // same for painter
```

Each `process_events` function calls the action callback, then calls
`__qumir_future_resume(future)` on the associated future, which triggers
`ResumeCaller` and resumes `DoEvalAsync` directly through the C++ coroutine
chain. The eval loop never calls `DoEvalAsync.resume()` explicitly; all
advancement happens inside `process_events`.

---

## WebAssembly / Browser Runtime

### Await protocol imports

In the WASM build, `__qumir_future_*` and `__qumir_wrap_coro` are **JS
imports** (implemented in `service/static/runtime/future.js`). They never
enter the WASM binary as C++ code. The WASM binary only exports the
`__qumir_coro_*` helpers.

`future.js` maintains a JS-managed future table. All handles are negative
`i32` values (analogous to how `string.js` uses negative handles for JS
strings). Two kinds:

| Kind | Entry fields | Created by |
|---|---|---|
| JS-created | `{ caller, done }` | robot/turtle/painter JS imports |
| Wrapped child coro | `{ caller, done, coroPtr, resultSize }` | `__qumir_wrap_coro` |

Robot, turtle, and painter JS functions (e.g. `robot_right()`) now return a
JS future handle instead of `void`. The WASM coroutine calls the await
protocol imports on that handle exactly as on the native side.

### JS-side await protocol

```js
// future.js — exports (become WASM env imports)

__qumir_future_await_ready(h)        // → TABLE.get(h).done (or coro.done for child)
__qumir_future_await_suspend(h, caller) // stores caller; drives child one step
__qumir_future_await_resume(h, ptr)  // copies child result bytes if needed
__qumir_future_destroy(h)            // destroys child coro, removes from TABLE
__qumir_future_address(h)            // returns coroPtr (for llvm.coro.promise)
__qumir_wrap_coro(wasm_ptr, size)    // allocates TABLE entry, returns negative handle
```

`resolveFuture(h)` is called by the executor (robot.js etc.) when an
operation completes: it sets `done = true` and calls
`wasm.__qumir_coro_resume(entry.caller)` to resume the waiting WASM coroutine.

### Browser event loop

The event loop follows exactly the same pattern as the C++ JIT runner: the raw
coro frame returned by `entryFn` is immediately wrapped, and all further
operations go through the public `__qumir_future_*` API.

```js
futureEnv.__resetFutures();

const rawHandle = entryFn(...args);
const future    = futureEnv.__qumir_wrap_coro(rawHandle, 0);

while (!futureEnv.__qumir_future_done(future) && !stopRequested) {
  if (futureEnv.hasPendingOp()) {
    // Execute the next JS-side action, then resolve its future.
    // resolveFuture() calls __qumir_coro_resume(caller) internally,
    // advancing the WASM coro to the next await or completion.
    const { h, execute } = futureEnv.shiftPendingOp();
    execute();
    futureEnv.resolveFuture(h);
  } else {
    // No pending external op: parent may be polling a child coro.
    futureEnv.__qumir_future_resume(future);
  }

  renderStep();       // robot field / turtle canvas / painter canvas
  await sleep(delay); // animation pacing; 0 = batch mode
}

futureEnv.__qumir_future_destroy(future);
```

For child coroutines (`__qumir_wrap_coro`), `__qumir_future_await_suspend`
drives the child one step. The parent suspends. When the child's own async
operations are resolved via `resolveFuture`, the child advances. The parent
polls by re-entering via `__qumir_future_resume(future)` in the `else` branch.

### Sentinel and WASM exports

The sentinel global `__qumir_is_coroutine` is emitted whenever the module
contains at least one coroutine function. `runWasmCoroutine` checks for it to
decide the execution path.

WASM exports available to JS:

```text
__qumir_is_coroutine       ; exported i32 constant = 1
__qumir_coro_done(ptr)     → i32    ; 1 if coroutine is at final suspend
__qumir_coro_resume(ptr)   → void   ; resume from current suspension point
__qumir_coro_destroy(ptr)  → void   ; free the coroutine frame
__qumir_coro_promise_ptr(ptr) → ptr ; address of the promise/result slot
```

---

## Summary

### High-level API — use this everywhere

| Function | Purpose | Called by |
|---|---|---|
| `__qumir_future_await_ready` | Is the future complete? | Compiled program |
| `__qumir_future_await_suspend` | Store caller handle, yield | Compiled program |
| `__qumir_future_await_resume` | Extract optional result | Compiled program |
| `__qumir_future_destroy` | Release the future object | Compiled program |
| `__qumir_future_address` | Get underlying coro frame ptr (for result extraction) | LLVM lowering |
| `__qumir_future_done` | Poll for completion | Executor event loops |
| `__qumir_future_resume` | Drive future one step / resolve | Executor event loops |
| `__qumir_wrap_coro` | Wrap raw coro frame in `ITypeErasedFuture*` | LLVM lowering, event loops |

### Low-level API — LLVM-provided, for implementors only

These are thin wrappers over LLVM coroutine intrinsics generated by
`EmitCoroutineRuntimeHelpers`. They implement the raw **coroutine frame ABI**
and are used to build `ITypeErasedFuture` implementations such as
`TWrappedLLVMCoro`. Direct callers outside the runtime should not exist —
use the high-level API instead.

| Function | Purpose |
|---|---|
| `__qumir_coro_done(frame)` | 1 if frame is at final suspend (resume fn ptr is null) |
| `__qumir_coro_resume(frame)` | Resume coro from current suspension point |
| `__qumir_coro_destroy(frame)` | Free the coro frame allocation |
| `__qumir_coro_promise_ptr(frame)` | Address of the result/promise slot in the frame |

Currently `__qumir_coro_*` are only called from inside `future.js` (the
WASM/JS bridge that implements `__qumir_wrap_coro` and the await protocol as
JS functions).
