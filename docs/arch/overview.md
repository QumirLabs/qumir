# Qumir Compiler — Architecture Overview

Qumir is an educational programming language compiler with Russian keywords,
inspired by the Soviet-era KUMIR/Ershov teaching language.  It supports three
execution backends — LLVM native, WASM, and an IR interpreter (VM) — and an
online playground at <https://qumir.dev>.

---

## 1. Pipeline at a glance

```
Source (.kum / core-lang)
        │
        ▼
    [ Lexer / Parser ]
        │  raw AST
        ▼
 ┌─ Semantic passes ───────────────────────────────┐
 │  1. Source transform fixpoint                   │
 │     (resolve → annotate → rewrite)              │
 │  2. Definite assignment                         │
 │  3. Return normalization                        │
 │  4. Lifetime rewrite                            │
 │  5. Final resolve → annotate → lifetime validate│
 └─────────────────────────────────────────────────┘
        │  finalized AST with explicit lifetime operations
        ▼
  [ IR lowering ]  →  TModule (SSA IR)
        │
        ├── [ IR passes ]  (SSA, const-fold, de-SSA, …)
        │
        ├──────────────────────────────────────┐
        ▼                                      ▼
  [ VM / interpreter ]              [ LLVM codegen ]
  direct execution                  native binary / WebAssembly
```

The CLI tools are:
- **`qumiri`** — interpreter / LLVM JIT
- **`qumirc`** — compiler driver

The web service wraps `qumirc` as a subprocess and serves the playground
frontend.

---

## 2. Two surface languages

### 2.1 Kumir (`.kum`)

The primary user-facing language.  Keywords are Cyrillic:

| Concept       | Keyword(s)                                   |
|---------------|----------------------------------------------|
| Algorithm     | `алг` / `нач` / `кон`                        |
| Types         | `цел` `вещ` `лог` `лит` `сим` `таб`         |
| If / else     | `если` / `то` / `иначе` / `все`             |
| Loop          | `нц` / `кц` / `пока` / `для` / `шаг`       |
| Switch        | `выбор` / `при`                              |
| Param modes   | `арг` (in)  `рез`/`знач` (out)  `аргрез`   |
| Module import | `использовать`                               |

### 2.2 Core lang (oz-lang)

A lower-level surface syntax for the same AST used for tests, golden files, and
debugging.  The playground exposes it as a "core syntax" mode.  Core lang is
useful because Kumir syntax is deliberately small and conservative: new
constructs can be introduced, inspected, and debugged at the AST level first,
before deciding whether they need `.kum` frontend syntax. It also supports
source modules and computational kernels for embedding in host applications.
The [oz-lang guide](core-lang.md) develops complete examples from basic
expressions through structures, generics, and SIMD vectors, followed by a
syntax reference and compiler-output conventions.

---

## 3. AST and type system

### 3.1 Node hierarchy

All AST nodes derive from `TExpr`.  Pattern matching uses `TMaybeNode<T>` —
a wrapper that holds a `shared_ptr<T>` when the dynamic type matches:

```cpp
if (auto call = TMaybeNode<TCallExpr>(expr)) { /* call.Cast() */ }
```

### 3.2 Type hierarchy

Types are `shared_ptr<TType>` subtypes, matched with `TMaybeType<T>`:

| C++ type           | Language concept                       |
|--------------------|----------------------------------------|
| `TIntegerType`     | `цел` — one 64-bit signed integer      |
| `TFloatType`       | `вещ` — IEEE-754 double                |
| `TBoolType`        | `лог` — boolean                        |
| `TStringType`      | `лит` — managed string handle          |
| `TSymbolType`      | `сим` — Unicode code point (i32)       |
| `TVoidType`        | procedure return / no value            |
| `TArrayType`       | `таб` — heap-allocated array           |
| `TNamedType`       | user-defined type alias                |
| `TFunctionType`    | `(param…) → ret`                       |
| `TPointerType`     | internal low-level pointer             |

---

## 4. Semantic passes

The complete sequence runs via `NTransform::Pipeline()`. See
[AST transformation pipeline](transformation-pipeline.md) for the exact pass
order, contracts, and fixpoint behavior.

### 4.1 Source transform fixpoint

Name resolution, type annotation, and source-level AST rewrites repeat until
the tree stops changing. Name resolution builds the symbol table and resolves
every identifier to its declaration. Module imports (`использовать`) are
processed here. Type annotation infers and checks expression types. Rewrites
include array-bound normalization, syntactic-sugar elimination, loop
conversion, and optional coroutine annotation.

### 4.2 Definite assignment

Checks source-level control flow for reads of potentially uninitialized
variables. It runs before internal lifetime nodes are introduced.

### 4.3 Return normalization

Makes function termination explicit. Void fallthrough receives an explicit
return, while a non-void final expression becomes a return. Whether a nested
block or conditional terminates is control-flow information; `return` itself
remains a void-typed final expression and is never usable as a value.

### 4.4 Lifetime rewrite

Rewrites ownership into explicit internal AST operations. Assignments become
`replace`, borrowed reads become `borrow`, ownership transfers become `move`,
copies of reference-counted values use `retain`, literals use `own-literal`,
and scope exits contain explicit `destroy` operations. Structured exits use
`cleanup-exit`; module-level destruction uses one `cleanup-global` node.

### 4.5 Final semantic validation

The rewritten tree is resolved and annotated again because the lifetime pass
introduces synthetic locals and expressions. The lifetime validator then
rejects invalid ownership combinations before IR lowering.

### 4.6 Lifetime and ownership

Lifetime policy belongs to the semantic AST, not to IR lowering. Strings,
arrays, aggregate traits, explicit ownership nodes, lexical and structured-exit
cleanup, globals, validation, and the lowering boundary are documented in
[lifetime analysis](lifetime.md).

---

## 5. IR (intermediate representation)

The IR is a custom SSA-like representation.

### 5.1 Structure

```
TModule
├── ExternalFunctions[]   imported runtime symbols
├── Functions[]
│   ├── Blocks[]
│   │   ├── Phis[]        φ-functions (SSA merge points)
│   │   └── Instrs[]      sequential instructions
│   └── LocalTypes[]      local variable type ids
├── GlobalTypes[]         module-level slot types
├── StringLiterals[]      constant pool
└── Types (TTypeTable)    type interning table
```

### 5.2 Instruction format

```cpp
struct TInstr {
    TOp      Op;             // opcode: "add", "call", "ret", "jmp", "cmp", …
    TTmp     Dest;           // destination temporary (-1 = no result)
    TOperand Operands[4];    // Tmp | Imm | Label | Local | Slot
};
```

Opcodes are string-literal hashes (`"add"_op`, `"call"_op`, …) so new
opcodes can be added without touching an enum.

### 5.3 IR passes

| Pass           | Purpose                                   |
|----------------|-------------------------------------------|
| `locals2ssa`   | stack locals → SSA temporaries            |
| `const_fold`   | constant folding                          |
| `de_ssa`       | insert copies at φ-joins before codegen   |
| CFG analysis   | predecessor / successor computation       |

### 5.4 IR type table

`TTypeTable` interns IR-level types by kind: `I1`, `I8`, `I32`, `I64`,
`F64`, `Void`, `Ptr`, `Struct`, `Func`.  AST types map to IR types via
`FromAstType()`.

---

## 6. VM / interpreter

The VM is a register-based bytecode interpreter. The IR is compiled to
`TVMInstr` bytecode by `TVMCompiler`, which assigns stack-frame byte offsets
to locals and aligned register-file byte offsets to temporaries. IR temporary
ids remain unchanged; only VM operands contain the assigned byte offsets.

Each invocation uses one aligned byte buffer for all registers, including
128-bit integers. Existing scalar opcodes use eight-byte slots even for
narrower types, while 128-bit slots use sixteen bytes with sixteen-byte
alignment. Struct registers hold addresses of storage in the call frame.
Function calls save and restore the complete register file as one byte range.

### 6.1 Execution model

Each function call creates a stack frame (a heap-allocated byte buffer).
The interpreter loop dispatches on `EVMOp` enum values.  Frames are linked
(parent frame pointer stored at offset 0).

### 6.2 Calling convention for external (runtime) functions

External functions — robot actions, math helpers, string operations — are
registered by modules.  Each external function has **two representations**:

| Field    | Type                                          | Used by        |
|----------|-----------------------------------------------|----------------|
| `Ptr`    | `void*` — native function pointer             | LLVM backend   |
| `Packed` | `uint64_t(*)(const uint64_t* args, size_t n)` | VM interpreter |

The **`Packed` thunk** is the VM calling convention.  All arguments are
passed as a flat array of `uint64_t` values (integers, floats as bit-casts,
string handles, pointers as integers).  The return value is a single
`uint64_t`.

```cpp
// Typical registration
{
    .Name        = "вверх",
    .MangledName = "robot_up",
    .Ptr         = reinterpret_cast<void*>(static_cast<void(*)()>(robot_up)),
    .Packed      = [](const uint64_t*, size_t) -> uint64_t {
                       robot_up(); return 0;
                   },
}
```

The VM's `ECall` handler casts `addr` to `TPacked` and calls it directly,
collecting pre-pushed arguments from `Runtime.Args`.

Additional flags on external functions:

- **`RequireArgsMaterialization`** — string arguments are dereferenced from
  their handle before the call, so functions that expect `const char*` work
  without extra wrapping.
- **`Inline`** — an optional `TInlineFactory` that the type-annotation pass
  can use to replace the call with a different AST subtree.  Only the VM
  uses `Inline`; the LLVM/WASM backends continue to use `Ptr`.

### 6.3 LLVM JIT

The LLVM JIT runner compiles the IR module through the LLVM backend and runs
it in-process via LLVM ORC JIT.  External functions are resolved to their
`Ptr` addresses at link time.

---

## 7. LLVM backend

Translates `TModule` to LLVM IR and then to native code or WebAssembly.

### 7.1 Function lowering

IR basic blocks map directly to LLVM basic blocks.  IR temporaries become
LLVM values (`alloca`-free after the de-SSA pass).  Struct arguments and
returns are modelled as ordinary LLVM values.

### 7.2 Target

| Flag                | Triple                   | Notes                                    |
|---------------------|--------------------------|-------------------------------------------|
| *(none)*            | host triple              | native AOT or JIT                        |
| `--wasm`, `--wasm32` | `wasm32-unknown-unknown` | requires `wasm-ld` for linking           |
| `--wasm64`          | `wasm64-unknown-unknown` | requires `wasm-ld -mwasm64` for linking  |

### 7.3 Optimization

`cg.Emit(module, optLevel)` runs the standard LLVM pass pipeline at the
requested level (0–3).

---

## 8. Module system

User programs import modules with `использовать <Name>`.  Each module
implements `IModule` and registers external functions and types.

Built-in modules:

| Module             | Purpose                                   |
|--------------------|-------------------------------------------|
| System (default)   | I/O, file I/O, math, string operations    |
| Робот              | Grid robot executor                       |
| Черепаха           | Turtle graphics executor                  |
| Рисователь         | Canvas drawing                            |
| Чертежник          | Vector drawing                            |
| Комплексные числа  | Complex number support                    |
| Цвета              | Named color constants                     |

Runtime implementations exist in two forms: C++ (native / WASM) and
JavaScript (browser playground).

---

## 9. Runtime Data Representation

Arrays, structs, and strings have non-trivial ABI and lifetime conventions, so
the details live in separate notes: [arrays](arrays.md) describes `таб` as a
pointer to a flat heap buffer with compiler-maintained bounds/strides,
row-major multidimensional layout, and by-pointer function passing;
[structs](structs.md) describes struct layout, address-backed temporaries,
LLVM value ABI for arguments/returns, and reference-parameter copies; and
[strings](strings.md) describes `лит` as `TString::Data` / browser handles with
reference counting, ownership tracking, literal materialization, and
retain/release rules around calls, returns, assignments, and destruction. The
cross-type semantic ownership model is described in
[lifetime analysis](lifetime.md).

---

## 10. Inspirations

The compiler's design and the way its implementation is explained were shaped
by several books, production-language systems, and lecture series:

- Bjarne Stroustrup,
  [*Programming: Principles and Practice Using C++*](https://www.stroustrup.com/programming.html)
  — the engineering and teaching principles behind building programs from
  explicit abstractions and invariants.
- [YQL reference and architecture](https://ydb.tech/docs/en/yql/reference/?version=v25.4)
  — an example of a production query language with a typed expression model,
  staged transformations, and multiple execution environments.
- Robert Nystrom, [*Crafting Interpreters*](https://craftinginterpreters.com/)
  — a concrete end-to-end treatment of language implementation, from syntax
  and semantic analysis to execution.
- Matthias Braun et al.,
  [*Simple and Efficient Construction of Static Single Assignment Form*](https://c9x.me/compile/bib/braun13cc.pdf)
  — the basis for constructing SSA directly from structured control flow and
  resolving incomplete phi nodes without a separate dominance-frontier pass.
- Konstantin Vladimirov's
  [compiler and systems programming lectures](https://www.youtube.com/@tilir)
  — lectures that influenced the compiler's implementation approach and its
  treatment of low-level language mechanics.
