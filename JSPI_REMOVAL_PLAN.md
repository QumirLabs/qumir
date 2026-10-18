# Coroutine-only browser execution

1. Remove JSPI module adaptation, async mode flags/headers and browser feature gates.
   Preserve the CLI from before JSPI and the first-declared-function entry rule.
2. Add a mandatory module IR pass after lowering: give debug imports the Future ABI,
   discover synchronous callers to a fixed point (including recursive cycles), change
   their physical return types and insert call/await pairs preserving result types.
   Existing explicit Future calls keep their meaning.
3. Schedule debug checkpoints through the same JS future queue as executor operations.
   Preserve stepping, cancellation, live variable addresses and event-loop yields.
4. Remove alternate executor imports and update architecture documentation.
5. Test recursive propagation, IR idempotence, result values, LLVM verification,
   WASM regressions and browser debugging with suspension extensions unavailable.

Completed and validated:

- Built qumirc, server, test_debug_async and test_llvm_debug_info.
- Passed all six selected CTest suites, including 146 WASM regression cases,
  core lifetime/module cases and the wasm32 struct layout regression.
- Passed browser stepping, recursive stack, locals, hover, Pause/Stop, keyboard,
  graphics, async imports, coroutine global initialization, typed return values
  and first-declared-function selection with both JSPI APIs disabled.
- Compared CLI parsing with 7f606ade^: no old flags removed; the only added flag
  is --debug-points. No async mode option or request header remains.
- Passed text hygiene and git diff whitespace checks.
