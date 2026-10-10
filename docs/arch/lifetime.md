# Lifetime analysis and explicit ownership AST

Qumir represents lifetime decisions in the finalized AST. IR lowering does not
infer ownership from ordinary assignments, calls, blocks, or returns. It only
lowers explicit lifetime operations inserted and checked by the semantic
pipeline.

This design makes cleanup inspectable before IR generation and gives the VM,
LLVM, and WASM backends the same semantics.

The current analysis is a structural ownership rewrite over typed AST, not a
general-purpose borrow checker or liveness solver. It tracks lexical scopes,
function and loop boundaries, and the ownership category of managed value
producers. Terminal control flow is determined separately; `return` remains a
void-typed exit and never becomes a value of the returned expression's type.

## Lifetime traits

`GetLifetimeTraits()` classifies an AST type using three properties:

| Kind | Current types | Copyable | Needs destruction |
|---|---|---:|---:|
| `Trivial` | integers, floats, bools, symbols, pointers, references | yes | no |
| `RefCounted` | string | yes | yes |
| `Unique` | array | no | yes |
| `Aggregate` | struct | derived from fields | derived from fields |

Named types are recursively classified through their underlying type. A
reference is always a borrowed handle: it does not inherit destruction
responsibility from the referenced value. Struct traits are the conjunction of
field copyability and the disjunction of field destruction requirements.
Cyclic named or struct classification is rejected.

Traits describe what is legal. The current rewrite has complete ownership
materialization for strings and lexical destruction for local/global arrays;
future managed types should extend this centralized model rather than add
ownership branches to ordinary IR-lowering handlers.

## Value ownership used by the rewrite

For string expressions the lifetime pass distinguishes:

| Classification | Expressions | Meaning |
|---|---|---|
| literal | raw string literal | static literal data, not yet an owned runtime string |
| borrowed | identifier, index, field access, explicit `borrow` | a non-consuming view of existing storage |
| owned | call, `await`, `retain`, `own-literal`, `move` | one value that must be consumed exactly once |
| not applicable | non-string expression | no string ownership rule |

For a value-producing block, ownership is the ownership of its final
expression. An unrecognized string-producing form is an error; the pass must
not guess.

When owned storage is required, the pass normalizes the producer:

```text
literal  x  → (own-literal x)
borrowed x  → (retain (borrow x))
owned    x  → (move x)
```

The resulting expression is explicitly owned and must be consumed by storage,
`replace`, `return`, or `destroy`.

## Lifetime AST nodes

| Node | Semantics |
|---|---|
| `(own-literal value)` | create an owned runtime value from supported literal data |
| `(borrow value)` | read a managed value without transferring ownership |
| `(retain value)` | create an owned ref-counted copy from a borrowed value |
| `(move value)` | transfer an existing owned value to its consumer |
| `(replace target value)` | destroy the old target value and install a new owned value |
| `(destroy value)` or `(destroy value aux)` | consume an owner and run its type-specific destructor |
| `(cleanup-exit exit cleanup...)` | run ordered cleanup, then perform `return`, `break`, or `continue` |
| `(cleanup-global cleanup...)` | define the module-level destruction sequence |

`aux` is a separate operand, not a generic array syntax. It carries
destructor-specific metadata; currently `array<string>` uses the saved
allocation byte size.

## Declarations and assignments

A string variable is initialized to a null string before any owned value is
installed. A source initializer is then rewritten through `replace`:

```text
(var result string = "value")
```

becomes conceptually:

```text
(var result = (bitcast 0 string))
(replace result (own-literal "value"))
```

The same rule applies to string variable assignment, string array elements,
and string struct fields. The right-hand side is first converted to an owned
value. `replace` then releases the previous target and stores the new owner.
Target-address evaluation belongs to `replace`; it must not be duplicated for
indexed or field lvalues.

Non-string assignments are not given reference-counting behavior by this pass.

## Calls and temporaries

Non-reference string parameters are borrowed for the duration of a call.
Borrowed arguments can be passed directly as `(borrow value)`. An owned or
materialized-literal argument cannot be discarded after being borrowed, so the
pass creates synthetic storage:

```text
(block
  (var __lifetime_0 = <owned argument>)
  (call f (borrow __lifetime_0))
  (destroy __lifetime_0))
```

If the call returns a value, that result is saved before argument temporaries
are destroyed, then returned as the block's final expression. Managed results
are moved from the result temporary. Cleanup of multiple argument temporaries
runs in reverse creation order.

An owned expression used only as a statement is rewritten to
`(destroy (move expression))`. Therefore unused managed call results do not
leak.

External functions that accept an unmaterialized literal are the one explicit
literal-call exception; this is controlled by the external function's
`RequireArgsMaterialization` contract.

## Lexical scopes and normal exit

The rewriter maintains a stack of lifetime scopes. Managed locals are
registered in declaration order. A block that can fall through receives
`destroy` nodes in reverse declaration order:

```text
(block
  (var first string ...)
  (var second string ...)
  ...
  (destroy second)
  (destroy first))
```

No normal-exit cleanup is appended after a terminal exit. A block is terminal
when it contains a `cleanup-exit`, or when an `if` has both an `else` branch and
both branches are terminal. This is control-flow information; it does not
change expression types.

## Return, break, and continue

Every structured exit explicitly carries the cleanup for the scopes it leaves.

- `return` cleans all scopes up to the current function boundary;
- `break` and `continue` clean scopes created inside the current loop iteration
  boundary;
- an inner loop never consumes cleanup belonging to an outer loop or function.

For a managed return value, local cleanup must not destroy the returned owner.
The pass first creates a synthetic owner, then cleans locals, then moves the
saved value into the return:

```text
(block
  (var __lifetime_0 = (retain (borrow result)))
  (cleanup-exit
    (return (move __lifetime_0))
    (destroy second)
    (destroy result)))
```

`cleanup-exit` preserves execution order: its return value is already in owned
storage, cleanup runs left-to-right, and the terminal operation occurs last.

## Arrays

Local array declarations are unique owners; array parameters are borrowed and
are not destroyed by the callee. Plain arrays need only their data pointer for
destruction.

For `array<string>`, destruction must release every element before freeing the
buffer. Bounds may be expressions and may change later, so the pass computes
allocation size once at declaration time and saves it in a synthetic integer
local. The size is in elements: the width of one belongs to the target.
Non-reusable bound expressions are similarly saved so allocation and
cleanup use the same evaluated bounds.

```text
(var words <array string 1> [lower upper])
(var __lifetime_size = (+ (- upper lower) 1))
...
(destroy words __lifetime_size)
```

The saved size follows the owner through normal scope cleanup and through
`cleanup-exit`. Zero-length and multidimensional arrays use the same product of
dimension lengths.

## Globals

Managed global declarations are collected separately from function scopes.
The root block receives exactly one `cleanup-global` node whose operations are
in reverse successful initialization order:

```text
(cleanup-global
  (destroy global_words allocation_size)
  (destroy global_text))
```

Lowering translates this node into the module destructor. Global destruction
is not reconstructed by scanning declarations in the lowerer.

## Validation

After lifetime rewriting, final name resolution and type annotation run again.
`TLifetimeValidator` then checks the finalized tree, including:

- synthetic lifetime identifiers resolve to the intended declarations;
- an explicit owned result has an owning consumer;
- `retain` receives a borrowed ref-counted value;
- `move` consumes an owned producer or owned variable storage;
- `borrow` receives a borrowed managed value;
- `destroy` consumes managed owned storage, never a borrowed value;
- `replace` targets managed storage and receives an owned value;
- raw literals cannot enter owned storage without `own-literal`;
- borrowed unique values cannot be copied;
- managed return values in `cleanup-exit` are owned;
- cleanup operands are themselves valid lifetime operations.

The validator recognizes a tree containing lifetime nodes as finalized lifetime
mode. A serialized finalized tree can therefore be validated without being
rewritten again.

## Lowering boundary

IR lowering maps explicit nodes to operations such as `str_from_lit`,
`str_retain`, `str_release`, `array_str_destroy`, and `array_destroy`.
Type-specific destructor selection is confined to lifetime-node lowering.
Ordinary assignment, call, block, and return lowering must not infer ownership,
maintain a destructor stack, or insert hidden reference-counting operations.

## Current limitation: coroutines

An `await` result is currently classified as owned, but lexical scope cleanup
alone does not prove correctness for managed locals stored in a suspended
coroutine frame. Frame ownership, cancellation, and destruction while suspended
require separate analysis. The required work and tests are tracked in
[lifetime-coroutines.md](../issues/lifetime-coroutines.md).
