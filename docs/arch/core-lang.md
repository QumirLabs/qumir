# Core Language

Core lang is the internal surface syntax for the Qumir AST. It is used by
tests, golden files, debugging output, and the playground "core syntax" mode.
The syntax is intentionally close to S-expressions: most AST nodes are written
as parenthesized forms, while composite types use angle brackets.

The motivation is practical: the user-facing Kumir syntax is small and
conservative, which makes it awkward to introduce and debug new language
constructs directly in `.kum` first. Core lang lets us start from the AST
shape: add or inspect a node, write it explicitly, run it through semantic
passes and lowering, and only later decide whether and how it should be exposed
in the Kumir frontend syntax.

The implementation lives in `qumir/parser/core/lexer.cpp`,
`qumir/parser/core/parser.cpp`, and `qumir/parser/core/printer.cpp`.

## Lexical Syntax

Whitespace separates tokens and is otherwise ignored.

Delimiters and structural operators:

| Token | Meaning |
|-------|---------|
| `(` `)` | expression forms and nested lists |
| `<` `>` | composite type forms |
| `[` `]` | array bounds, index vectors, slices, and generic parameter lists |
| `:` | type annotation form head |

Core source is UTF-8 text. Simple identifiers start with a letter, `_`, `$`, or
a non-ASCII byte. They may continue with letters, digits, `_`, `$`, `:`, or
non-ASCII bytes:

```core
foo
my_var
$tmp
module:name
имя
```

Symbolic identifiers are made from operator characters:

```core
+ - * / // % = ! & ^
<= >= << >> || |
```

`<`, `>`, `[`, `]`, `(`, `)`, and `:` are delimiter tokens by default. The
lexer has one special case for operator heads: after an opening `(`, two-byte
`<<`, `<=`, `>>`, and `>=` are read as identifiers so forms like `(>> x 1)` are
valid operator calls.

Bar identifiers allow spaces:

```core
|foo bar|
```

String and character literals support the common escapes `\n`, `\t`, `\\`,
`\"`, and `\'`:

```core
"hello\nworld"
'\n'
```

Numeric and boolean literals:

```core
123
-42
1.25
.5
3e-2
#t
#f
```

`nil` is a reserved identifier used as the empty expression/type value in
places where the AST allows a missing child.

## Expressions

An atom expression is one of:

| Syntax | AST node |
|--------|----------|
| `name` | `TIdentExpr`, except reserved `break`/`continue` |
| integer, float, boolean | `TNumberExpr` |
| character | `TNumberExpr` with `char` type |
| string | `TStringLiteralExpr` |
| `break` | `TBreakStmt` |
| `continue` | `TContinueStmt` |
| `nil` | null `TExprPtr` |

Parenthesized forms use the first item as a form name. Unknown two-argument
forms are parsed as binary operator expressions, and unknown one-argument forms
are parsed as unary operator expressions.

## Core Forms

Assignment:

```core
(= name value)
(= name [index1 ... indexN] value)
```

The first form creates `TAssignExpr`; the indexed form creates
`TArrayAssignExpr`.

Unary and binary operators:

```core
(op operand)
(op left right)
```

Associative operators (`+`, `-`, `*`, `/`, `//`, `&`, `|`, `^`, `&&`, `||`,
`xor`) also accept more than two arguments and fold left into a chain of binary
nodes — `(&& a b c)` is exactly `(&& (&& a b) c)`, so `&&`/`||` keep their
short-circuit semantics. The other operators (comparisons, shifts, `%`) stay
strictly binary.

### Division

There are three division operators:

| Operator | Operands | Result |
|----------|----------|--------|
| `/` | numbers | always `f64` |
| `//` | integers only | the common integer type |
| `%` | integers only | the common integer type |

`/` on two integers converts both to `f64` first, so `(/ 7 2)` is `3.5`. For a
quotient that stays an integer, use `//`:

```core
(/ (: 7 i64) (: 2 i64))   ; 3.5
(// (: 7 i64) (: 2 i64))  ; 3
(% (: 7 i64) (: 2 i64))   ; 1
```

`//` truncates toward zero and `%` keeps the sign of the left operand, as in C:
`(// -7 2)` is `-3` and `(% -7 2)` is `-1`.

Both operators respect the signedness of their type. An unsigned pair divides
without a sign bit, so `(// u 3)` on a `u64` that holds `2^64 - 1` gives
`6148914691236517205`, not `0`.

Operands of different integer types widen to their common type first, under the
rules in [Integer widths](#integer-widths). A division by zero is not checked.

Blocks:

```core
(block stmt1 stmt2 ... stmtN)
```

`block` introduces a nested lexical scope. Its value is the value of its last
statement (or void if the block is empty or the last statement does not
produce a value) — this is how function bodies return a value implicitly, see
"Functions" below.

The outermost `block` (depth 1) enforces a strict statement order:

```
(pragma ...)* → (use ...)* → (type ...)* → other statements
```

**Pragmas** are virtual nodes — not represented in the AST — collected into
`TParser::Pragmas` for the caller to apply after `Parse()` returns.

```core
(block
  (pragma language overloads)
  stmt1
  stmt2 ...)
```

A pragma has the form `(pragma group value1 value2 ...)`. Pragmas must appear
before `use` and `type` declarations.

**`use`** imports a module, making its functions and types available in scope.
`use` must appear after pragmas and before type declarations.

```core
(block
  (use "Цвета")
  ...)
```

Core-lang does not define an I/O runtime and does not import anything
implicitly: a pure core program imports only what it spells with `use`. The
runtime modules (such as `System`) are provided by the host. The `qumiri` and
`qumirc` core modes import `System` as a host prelude so that forms like
`output` resolve; module aliases like the legacy Kumir `Файлы` exist only in
the Kumir frontend.

**`type`** declares a named type alias. It must appear after `use` statements.
Declaring a type whose name is already imported from a module is an error.

Currently defined pragmas:

| Group | Value | Effect |
|-------|-------|--------|
| `language` | `overloads` | Enable user-defined function overloading (off by default) |

Conditionals:

```core
(if condition then)
(if condition then else)
```

`if` creates `TIfExpr`, used both as a statement and as a value-producing
expression — its value is the value of the taken branch. The else branch is
optional. The older `(cond ...)` spelling is rejected with an error pointing
at `(if ...)`.

Loops:

```core
(while condition body)
(repeat body condition)
(for name from to step body)
(times count body)
break
continue
```

`for` always has a step expression in core syntax. Use `nil` if a missing step
must be represented.

Variables and variable blocks:

```core
(var name type)
(var name type [from1 to1] ... [fromN toN])
(var name = value)
(vars var1 var2 ... varN)
```

Array bounds are declaration metadata on `TVarStmt`, not part of the
`TArrayType` identity. The `(var name = value)` form omits the type entirely —
it is inferred from `value`'s type during type annotation.

Functions:

```core
(fun name (param1 ... paramN) body)
(fun name (param1 ... paramN) -> return_type body)
(fun name (param1 ... paramN) -> return_type (attrs (expect_after expr) (expect_before expr) ...) body)
(fun name [generic_param1 ... generic_paramN] (param1 ... paramN) -> return_type body)
```

Each parameter is a `(var ...)` form; `(param1 ... paramN)` is required even
when empty (`()`). The body must be a `block`. The return type defaults to
`void` when `-> return_type` is omitted.

`[generic_param1 ...]` is an optional generic parameter list placed immediately
after the function name. A bare generic parameter name declares a type
parameter, for example `[T]` or `[K V]`. Value generic parameters may be parsed
as `(const Name type)` (or `(Name type)`, which prints canonically with
`const`). Only integer value parameters are semantically supported today.

`(attrs ...)` is an optional attribute list, placed after the return type (if
any) and before the body. Recognized attributes:

```core
(expect_after expr)
(expect_before expr)
(extern native_symbol)
(extern "native_symbol")
(operator "OP")
extern
print
used
cacheable
inline
```

The parser stores `expect_after` on `TFunDecl::LastAssert`. `expect_before` is
parsed for forward compatibility. Bare-identifier attributes other than the
recognized ones above are accepted and silently ignored for
forward compatibility.

`inline` requests LLVM `alwaysinline` for ordinary function definitions.
It is preserved through AST cloning, core printing and generic specialization.
Calls are inlined by the LLVM optimization pipeline; at optimization level
zero the attribute is retained without an inlining guarantee. It does not
change the interpreter, external declarations, coroutines, or functions
kept unoptimized for debug points and the REPL. Recursive calls cannot
always be inlined. For example:

```core
(fun twice [T] ((var x T)) -> T (attrs inline)
  (block (return (+ x x))))
```

Inlining requires a body in the LLVM module; the attribute alone cannot
inline calls across native object boundaries. `cacheable inline` may be
combined, and both attributes survive core printing.

The native object cache imports reachable `inline` definitions and a bounded
set of small helpers with LLVM `available_externally` linkage. The cached
object retains ownership of each exported definition, while its callers can
still inline the imported body on both cache misses and hits. Large helpers
remain out of line unless explicitly marked `inline`; their native objects
are reused without optimizing their bodies in each caller. Each missing
object selects imports from its own call graph. At optimization level zero,
dependency bodies are not imported.

### External function attributes

`extern` marks a function declaration as implemented by a native symbol instead
of a core-lang body. The function still has a syntactic body, usually an empty
`(block)`, but semantic analysis and lowering ignore it.

```core
; native symbol name is the same as the core function name: llabs
(fun llabs ((var x i64)) -> i64 (attrs extern)
  (block))

; core function name is my_abs, native symbol name is llabs
(fun my_abs ((var x i64)) -> i64 (attrs (extern llabs))
  (block))

; explicit symbol may also be written as a string
(fun my_abs2 ((var x i64)) -> i64 (attrs (extern "llabs"))
  (block))
```

- `extern` — bind the function to the native symbol with the same name as the
  core function.
- `(extern native_symbol)` — bind the function to an explicit native symbol
  written as an identifier.
- `(extern "native_symbol")` — same as above, but the symbol is written as a
  string literal.

The selected native symbol is stored in `TFunDecl::MangledName`. The core
printer emits external functions canonically as `(extern native_symbol)`, even
when the source used bare `extern`.

### Operator, cast and print attributes

A function may implement an operator, a cast or a type's printer instead of
being called by name. This lets a source module (or program) provide the same
overloaded-by-type behavior that built-in runtime modules do — without enabling
`language overloads`.

```core
; binary operator: a + b for tuple dispatches here
(fun add ((var a tuple) (var b tuple)) -> tuple (attrs (operator "+"))
  (block ...))

; unary operator (e.g. "neg" for unary minus)
(fun negate ((var a tuple)) -> tuple (attrs (operator "neg"))
  (block ...))

; cast: implicit coercion from i64 to tuple (the magic name is "cast")
(fun from_int ((var n i64)) -> tuple (attrs (operator "cast"))
  (block ...))

; printer: outputting a tuple value via (output ...) dispatches here
(fun show ((var p tuple)) (attrs print)
  (block (output "(" (field p a) ";" (field p b) ")")))
```

- `(operator "OP")` — the function implements operator `OP`. With two
  parameters it is a binary operator (`"+"`, `"=="`, ...); with one parameter, a
  unary operator. The reserved value `"cast"` (one parameter) registers an
  implicit cast from the parameter type to the return type.
- `print` (a bare attribute) — the function is its single parameter type's
  printer; `(output value ...)` of that type dispatches to it. Equivalent to a
  unary operator under the internal key `"print"`.
- `used` (a bare attribute) - keep this function even when no explicit reference makes
  it reachable.

These functions are resolved **by operand/result types**, in the resolver's
dispatch tables (`ImportedBinaryOps` / `ImportedUnaryOps` / `ImportedCasts`) —
not by their declared name. The declared name stays an ordinary symbol: e.g.
naming the printer `print` does not clash with the operator key `"print"`,
since the latter lives in a separate table and is invoked through a synthetic
name. Explicit casts `(cast x T)` are *not* routed through `(operator "cast")`;
only implicit coercions (assignment, arguments) are.

`<main>` is the program's entry point: `(fun <main> () body)`. It is always
void.

## Return

A non-void function returns its result either via an explicit `(return expr)`
or implicitly: if execution falls off the end of the body without hitting a
`return`, the value of the body block's last statement becomes the return
value. Falling off the end without producing a value is a compile error
("Тело функции должно заканчиваться оператором `return` или выражением,
возвращающим значение.").

```core
(return)
(return expr)
```

`(return)` is only valid in `void` functions, `(return expr)` only in
non-void ones. Both work for every return type, including structs and named
types.

```core
(fun square ((var x i64)) -> i64
  (block
    (* x x)))            ; implicit return: value of the last statement

(fun cube ((var x i64)) -> i64
  (block
    (var sq i64)
    (= sq (* x x))
    (return (* sq x))))  ; explicit return
```

`break`/`continue`/`return` release any in-scope locals with pending
destructors (string/array) before transferring control: `break`/`continue`
release locals declared since the start of the nearest enclosing loop body;
`return` releases locals declared since the start of the function body.

Type declarations:

```core
(type name underlying_type)
(type name [generic_param1 ... generic_paramN] underlying_type)
```

Declares a named type alias. `name` becomes available as a bare type identifier
in the surrounding block scope. Named types are transparent to the IR — the IR
type is the same as the underlying type.

`[generic_param1 ...]` is an optional generic parameter list placed immediately
after the type name. It uses the same syntax as function generic parameters: a
bare name declares a type parameter, while `(const Name type)` declares a
compile-time value parameter. Only integer value parameters are supported
today. Generic parameters are introduced into the type declaration's type
scope, so type parameters can be used anywhere a type is expected inside the
underlying type, and value parameters can be used as value arguments to nested
named types:

```core
(block
  (type имя <struct (val i64)>)
  (type Box [T] <struct (Value T)>)
  (type Nullable [T] <struct (Value T) (Valid bool)>)
  (type Decimal [(const Scale i32) (const Precision i32)] <struct (Value i64)>)
  (fun make_name ((var v i64)) -> <named имя>
    (block
      (return (: (struct ((val v))) <named имя>))))
  (fun unwrap_box ((var b <named Box [i64]>)) -> i64
    (block
      (return (field b Value))))
  (fun <main> ()
    (block
      (var n <named имя>)
      (= n (call make_name 42)))))
```

In type position the long form is `<named name>` or
`<named name [type_arg1 ... type_argN]>`. An optional inline underlying type may
still be written after the name/arguments as `<named name underlying_type>` or
`<named name [type_arg1 ...] underlying_type>`, but normal source should prefer
declaring the alias once with `(type ...)` and referring to it by name.

Type arguments are written as types. Integer value arguments are written as
integer literals, or as bare value-parameter names while resolving another
generic type/function. For example, `<named Decimal [2 10]>` is a concrete
decimal type, while `<named Decimal [Scale Precision]>` is valid inside a scope
that declares `(const Scale i32)` and `(const Precision i32)`.

Calls and I/O:

```core
(call callee arg1 ... argN)
(input arg1 ... argN)
(output arg1 ... argN)
(output (fmt expr width) (fmt expr width precision))
```

`fmt` is the output argument wrapper for width and optional precision.

Overloaded functions:

```core
(block
  (fun pick ((var x i64)) -> i64
    (block
      (+ x (: 10 i64))))

  (fun pick ((var x f64)) -> f64
    (block
      (+ x (: 0.5 f64))))

  (fun pick ((var x string)) -> string
    (block
      x))

  (fun <main> ()
    (block
      (output (call pick (: 7 i32)) "\n")
      (output (call pick (: 2.5 f64)) "\n")
      (output (call pick "text") "\n"))))
```

Multiple `fun` forms with the same source name form an overload set when their
parameter type lists differ. Return type alone is not enough to distinguish
overloads.

Overload resolution runs after argument type annotation. Exact matches are
preferred over implicit conversions. Integer widening is preferred over
integer-to-float conversion, so `(call pick (: 7 i32))` selects the `i64`
overload in the example above. If two viable overloads have the same best cost,
the call is rejected as ambiguous.

Generic functions:

```core
(block
  (pragma language overloads)

  (fun identity [K] ((var x K)) -> K
    (block
      (return x)))

  (fun pairFirst [K V] ((var a K) (var b V)) -> K
    (block
      (return a))))
```

Generic functions use a function-level parameter list after the function name.
Bare names in that list are type parameters. They are introduced into the
function's type scope, so they can be used anywhere a type is expected inside
the signature and body:

```core
(fun first [T] ((var values <array T 1>)) -> T
  (block
    (return (index values 0))))
```

Generic functions are enabled by the same `(pragma language overloads)` pragma
as overload sets. At each call site the concrete types bound to type parameters
are inferred from argument types by unification, and a monomorphized clone of
the function is generated for that binding. The clone is registered under a
signature-based synthetic name `__generic_<name>$<ReturnType>__$<ArgType1>...`
(`__generic_identity$i64__$i64`, `__generic_identity$string__$string`, ...)
and substituted for the call. Repeat calls with the same concrete signature
reuse the cached clone — exactly one definition exists per concrete signature,
regardless of how many call sites use it.

Type parameters may appear nested inside composite types (`<array K 1>`,
`<ref K>`, `<fun K (K)>`, ...), and a function may have several independent
type parameter names. If argument types do not bind every type parameter, the
annotator may also infer the remaining parameters from the function's declared
return type and return expressions. This is intentionally narrow: it is used
after the argument-based pass, and it follows `return`, `cast`, `if`, and
struct-constructor fields where the target return type still contains unbound
generic parameters. A call is a typing error when a type parameter's binding
cannot be inferred from the arguments or return body, or when the same type
parameter name would have to bind to two structurally different concrete types.

This makes generic operator overloads usable when the result type is determined
by an operation inside the overload body rather than by a single operand type:

```core
(block
  (pragma language overloads)
  (type Nullable [T] <struct (Value T) (Valid bool)>)

  (fun nullable_add_rhs [T U R] ((var a <named Nullable [T]>) (var b U))
      -> <named Nullable [R]> (attrs (operator "+"))
    (block
      (return (cast
        (if (field a Valid)
          (struct ((Value (+ (field a Value) b)) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [R]>)))))

  (fun <main> ()
    (block
      (var a <named Nullable [i64]>)
      (var c <named Nullable [f64]>)
      (= a (cast (struct ((Value (: 4 i64)) (Valid #t))) <named Nullable [i64]>))
      (= c (+ a (: 0.5 f64))))))
```

Here `T` is inferred as `i64` from `a`, `U` is inferred as `f64` from the
right-hand operand, and `R` is inferred as `f64` from the returned struct's
`Value` field expression `(+ (field a Value) b)`.

When an overload set mixes concrete and generic `fun` forms for the same name,
overload resolution prefers a concrete match over instantiating the generic
fallback — see `(fun f ((var x i64)) -> i64 ...)` vs.
`(fun f [K] ((var x K)) -> i64 ...)`.

Generic functions must be implemented in core-lang; external generic functions
are not supported. Function-level value generic parameters use `(const Name
type)` in the `[...]` list and are currently limited to integer types. Their
values are inferred from parametric named types in the function parameters:

```core
(fun decimal_add [(const Scale i32) (const Precision i32)]
    ((var a <named Decimal [Scale Precision]>)
     (var b <named Decimal [Scale Precision]>))
    -> <named Decimal [Scale Precision]> (attrs (operator "+"))
  (block ...))
```

Casts, indexing, and slicing:

```core
(cast operand type)
(bitcast operand type)
(index collection index)
(index collection [index1 ... indexN])
(slice collection [start])
(slice collection [start end])
```

`bitcast` preserves the operand's representation. Integer, float, and symbol
types and vectors of them convert between each other when their byte sizes
match: `(bitcast v i64)` packs a `<vec i32 2>`, lane 0 into the low bytes. A
vector of `bool` is a source only, each lane reading as a byte 0 or 1; scalar
`bool` and structs are not supported. A pointer or string converts to or from
`i64`, `u64`, or another pointer, whatever the pointer width of the target.
The check runs at lowering, where sizes are known.

Single-index access creates `TIndexExpr`; vector index access creates
`TMultiIndexExpr`. `(slice collection [start])` with a single bound is a
single-element slice, equivalent to `(slice collection [start start])`.

### Pointers

`<ptr T>` is a raw machine pointer to `T`. Core lang does not attach ownership,
lifetime, nullability, alignment, or bounds metadata to pointer values. It is
mainly used by runtime/source modules and QumirDB kernels to describe ABI
objects and manually managed buffers:

```core
(fun qdb_alloc ((var size i64)) -> <ptr i8> (attrs extern) (block))
(fun qdb_free ((var ptr <ptr i8>)) -> void (attrs extern) (block))
```

The usual low-level idioms are explicit casts:

```core
; null pointer
(cast (: 0 i64) <ptr i8>)

; null check
(== (cast ptr i64) (: 0 i64))

; reinterpret byte storage as typed storage
(var words = (cast bytes <ptr i64>))

; pointer arithmetic by bytes
(var tail = (cast (+ (cast bytes i64) byte_offset) <ptr u8>))
```

Explicit `cast` currently supports pointer-to-pointer, pointer-to-integer, and
integer-to-pointer conversions. `T* -> void*` may be implicit; `void* -> T*`
requires an explicit cast. These casts are representation-level operations and
do not validate the pointed-to storage.

`(index ptr i)` reads one element from a pointer. If `ptr` has type `<ptr T>`,
the expression has type `T` and lowers as a load from:

```core
ptr + i * sizeof(T)
```

Pointer indices are zero-based and have no array lower-bound adjustment. The
index must be an integer or implicitly castable to `i64`. Pointer indexing has
only the single-index form; multi-indexing is for arrays. Slices are only for
strings, not pointers. A pointer to a single object is dereferenced as
`(index ptr 0)`.

Indexed assignment stores through a pointer:

```core
(= words [i] (: 42 i64))
(= bytes [b] (index other_bytes b))
```

This lowers as a store to `ptr + i * sizeof(T)`. For struct element types,
assignment copies the whole struct value.

Current lowering has an important restriction: pointer indexing and indexed
assignment expect the collection to be a named local/global symbol. If the
pointer is produced by a field access, cast, or arithmetic expression, bind it
to a variable before indexing:

```core
(var data = (cast (field col Data) <ptr u8>))
(var byte = (index data row))

(var pair_work = (cast (+ (cast pairs i64) (* n (: 16 i64))) <ptr u64>))
(= pair_work [i] (index pairs i))
```

One-dimensional arrays can be explicitly cast to a pointer to their element
type. This is the current way to pass core array storage to lower-level helper
functions:

```core
(var counts <array u32 1> [0 255])
(call sort_count_pass (cast counts <ptr u32>))
```

The resulting pointer is a zero-based view of the array storage; the array's
declared lower bound is not carried with the pointer.

There is no first-class address-of expression for arbitrary values. When code
needs the address of an existing object rather than a raw pointer value, spell
the callee parameter as `<ref T>` and pass an addressable lvalue. During
lowering the call passes the lvalue address:

```core
(fun bump ((var x <ref i64>))
  (block
    (= x (+ x (: 1 i64)))))

(var value i64)
(call bump value)

(var a <array i64 1> [0 2])
(call bump (index a 1))
```

Addressable reference arguments include identifiers, field accesses, and array
element accesses. This is distinct from `<ptr T>`: `<ref T>` is a source-level
by-reference parameter convention, while `<ptr T>` is a first-class raw pointer
value.

During IR lowering the indexed expression is converted to the element address,
not loaded as a value.

Modules and assertions:

```core
(use module_name)
(use "module name")
(assert expr)
```

`use` accepts either an identifier-like name or a string token.

### Source modules (`.oz`)

A `use` name that does not name a registered runtime module but resolves to a
`<name>.oz` file is loaded as a **source module**: it is parsed, validated and
inlined into the compilation unit, so its top-level functions, types and globals
become part of the program (one combined AST, one set of constructors). Module
name = file stem.

Resolution order for `<name>.oz`: explicitly registered modules (`--module`),
then the directory of the main source file, then `--module-path` directories in
registration order; the first match wins. (Programmatically the runners take
`ModuleSearchPaths` / `ModuleFiles`.)

A source module's `use` declarations are its transitive dependencies; the loader
detects cycles and reports the chain. All top-level declarations are exported
into the shared namespace (no explicit `export`/`private` yet). A source module
may **not** declare `<main>` or executable top-level statements; it may declare
functions, types and globals. Globals are initialized in dependency order
(dependencies first, the main program last) and destroyed in reverse.

Operator/cast/print functions (see the attributes section) carried by a source
module are registered into the operator/cast dispatch tables on import, so
`a + b`, implicit casts and `(output value)` work for the module's types.

The same mechanism is available to the Kumir frontend: `использовать <name>`
loads `<name>.oz`; its exported type names are registered so the Kumir
lexer/parser recognizes them at parse time.

Struct operations:

```core
(field field_name object)
(struct ((name1 value1) (name2 value2) ... (nameN valueN)))
(field_assign object field_name value)
```

The `struct` expression creates a `TStructConstructExpr`. Field types are
usually supplied later through type annotation.

Type annotations:

```core
(: expr type)
```

This sets `TExpr::Type` on any AST node. The printer emits annotations in
`All` mode, and in required places such as named types and struct constructors
in the default mode.

## Types

Primitive types:

| Core | AST type |
|------|----------|
| `i8`, `i16`, `i32`, `i64`, `i128` | `TIntegerType`, signed |
| `u8`, `u16`, `u32`, `u64`, `u128` | `TIntegerType`, unsigned |
| `f64` | `TFloatType` |
| `bool` | `TBoolType` |
| `string` | `TStringType` |
| `char` | `TSymbolType` |
| `void` | `TVoidType` |

### Integer widths

A bare integer literal has type `i64`. To pick another width, annotate the
literal with `(: value type)`:

```core
(: 127 i8)
(: 4294967295 u32)
```

An implicit conversion to a wider type is allowed when it keeps every value:
signed to signed and unsigned to unsigned need `dst >= src` bits, and unsigned
to signed needs `dst > src` bits. Signed to unsigned is never implicit. Any
other change of width needs an explicit `cast`.

### 128-bit integers

`i128` and `u128` behave like the narrower widths. Arithmetic, bitwise
operators, shifts, comparisons and casts all work on them, and structs and
arrays store them like any other field or element:

```core
(var wide i128)
(= wide (* (: 1000000000000 i128) (: 1000000000 i128)))
(var arr <array u128 1> [0 3])
(var rec <struct (tag i32) (wide i128)>)
```

Two limits follow from the representation:

- A literal holds 64 bits. `(: 1000000000000000000000 i128)` does not fit, so
  build a large constant from smaller ones, as in the example above.
- An external function cannot take or return a 128-bit value. The FFI packs
  every scalar argument into one 64-bit slot.

A cast to `f64` loses precision above 2^53, as it does for `i64`.

Inside a struct a 128-bit field aligns to 16 bytes, which matches the C ABI.
The LLVM backend maps both types to `i128`. The IR interpreter keeps 128-bit
values in a register file of their own, next to the 64-bit one, so a program
that never uses them pays nothing.

Composite types:

```core
<fun return_type (param_type1 ... param_typeN)>
<array element_type arity>
<ptr pointee_type>
<ref referenced_type>
<named name underlying_type>
<named name [type_arg1 ... type_argN]>
<named name [type_arg1 ... type_argN] underlying_type>
<struct (field_name1 field_type1) ... (field_nameN field_typeN)>
<future result_type>
```

`<future result_type>` is `TFutureType`, the type of an awaitable produced by
a coroutine; see `arch/coroutine.md`.

Examples:

```core
i64
<array i64 1>
<array <ref f64> 2>
<ptr <struct (x f64) (y f64)>>
<ref string>
<named color i64>
<named Nullable [i64]>
<ptr <named Nullable [f64]>>
<struct (x i64) (values <array f64 1>)>
<fun bool (i64 f64)>
<future i64>
```

Unknown bare type identifiers are parsed as short named types:

```core
color
```

which is equivalent to a `TNamedType` without an underlying type attached. Name
resolution later decides whether the name is a generic type parameter currently
in scope or a declared/imported named type.

Named type arguments use the same `[...]` delimiter as declarations, but in the
canonical order `<named Name [Args]>`: the keyword `named` must immediately
follow `<`, then the type name, then the optional argument list. Bare scalar
types and generic type parameters may be used directly as type arguments;
integer literals and generic value parameters may be used as value arguments:

```core
<named Box [i64]>
<named Pair [string T]>
<named Decimal [2 10]>
<named Decimal [Scale Precision]>
<array <named Nullable [f64]> 1>
```

## Type Attributes

Types carry `Readable` and `Mutable` access flags. By default a type is both
readable and mutable. Core syntax can spell non-default access modes by wrapping
a scalar or composite type in angle brackets:

```core
<i64 (readonly)>
<ref string (writeonly)>
<named buffer <array i64 1> (readonly)>
```

The access modifiers `mutable`, `readonly`, and `writeonly` are mutually
exclusive. `mutable` explicitly selects the default readable and mutable state,
which the printer omits in canonical output. The fully immutable, unreadable
state cannot be spelled — there is no attribute for it.

## Builtin Byte Matching

The always-loaded `builtin` module provides fixed-width byte comparisons:

| Call | Return type | Mask layout |
|---|---|---|
| `builtin::byte_match8(data: <ptr u8>, byte: u8)` | `u64` | Byte i is `0xff` on a match and `0x00` otherwise. |
| `builtin::byte_match16(data: <ptr u8>, byte: u8)` | `u32` | Bit i is set on a match; bits 16 through 31 are zero. |
| `builtin::byte_match_backend()` | `i64` | 0: portable; 1: NEON byte masks; 2: SSE2 compact masks. |

Both comparisons accept unaligned pointers. The pointer must address at least
8 or 16 readable bytes respectively; neither primitive reads beyond its group.
The byte at `data[0]` always maps to the least significant byte or bit, including
on big-endian targets. All 256 byte values can be compared without special cases.

LLVM emits target-independent vector IR and chooses the comparison instructions.
The backend selector becomes a constant derived from the target machine and its
enabled features, so callers can choose group geometry without a runtime branch:
backend 1 favors `byte_match8`, backend 2 favors `byte_match16`, and backend 0
lets the caller use a portable algorithm. Both primitives remain available with
the same semantics on every target. The IR interpreter uses scalar comparisons
and reports its host backend.

For the 8-byte result, `cttz(mask) / 8` gives the first matching index. Before
iterating with `mask &= mask - 1`, keep one flag per byte by applying
`mask &= 0x8080808080808080`; otherwise a matching byte contains eight set bits.
For the 16-byte result, use `cttz(mask)` and clear the lowest set bit directly.
Check that the mask is nonzero before treating the index as a match.

```core
(fun group_matches ((var data <ptr u8>) (var byte u8)) -> u64
  (block (return (call builtin::byte_match8 data byte))))
```

## Printer Conventions

The core printer is the canonical form used by tests and AST goldens.

- Identifiers containing spaces are printed as bar identifiers.
- String and character literals are escaped with `\n`, `\t`, `\\`, `\"`, and
  `\'`.
- `nil` is printed for null expressions and null types.
- Function attributes print `expect_after`, `operator`/`print`, `literal`, and
  `extern`. `expect_before` and `used` are parsed/consumed but are not emitted
  by the printer.
- Function generic parameters are printed immediately after the function name
  in `[...]`; value generic parameters print canonically as `(const Name type)`.
- Type generic parameters are printed immediately after the type name in
  `[...]`, with the same canonical spelling as function generic parameters.
- Type annotations are printed depending on `TPrintOptions::TypeMode`.
- Scalar types are printed bare unless they need non-default attributes.
- Named types may be printed short when listed in `ShortNamedTypes`, but only
  when they have no type arguments, no inline underlying type, and no
  non-default attributes. Otherwise they print as `<named name>`,
  `<named name [args]>`, or with the inline underlying type when one is present.

## Example

```core
(fun sum
  ((var n i64)
   (var a <array i64 1> [0 (- n 1)]))
  -> i64
  (block
    (vars
      (var i i64)
      (var s i64))
    (= s 0)
    (for i 0 (- n 1) 1
      (block
        (= s (+ s (index a i)))))
    s))
```
