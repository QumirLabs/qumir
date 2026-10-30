# oz-lang: a practical guide

**oz-lang**, also called **core lang**, is Qumir's typed, expression-oriented
language for writing programs, reusable modules, and generated computational
kernels. Source files use the `.oz` extension. The Kumir frontend and oz-lang
share semantic analysis, the intermediate representation, the interpreter, and
the LLVM backends for native code and WebAssembly.

This guide starts with complete programs and builds toward structures,
generic functions, SIMD vectors, and integration with a host application.
It assumes basic programming knowledge, but no knowledge of Qumir's compiler
internals. The examples use the runtime supplied by the command-line tools.

- [Your first program](#your-first-program)
- [Reading the syntax](#reading-the-syntax)
- [Values, variables, and conversions](#values-variables-and-conversions)
- [Arithmetic and logic](#arithmetic-and-logic)
- [Functions and control flow](#functions-and-control-flow)
- [Strings and arrays](#strings-and-arrays)
- [Named types and structures](#named-types-and-structures)
- [References and pointers](#references-and-pointers)
- [Overloads and generic functions](#overloads-and-generic-functions)
- [Operators, conversions, and printers](#operators-conversions-and-printers)
- [SIMD vectors](#simd-vectors)
- [Source modules](#source-modules)
- [Host functions and suspension](#host-functions-and-suspension)
- [Builtin operations](#builtin-operations)
- [Syntax reference and compiler output](#syntax-reference-and-compiler-output)

## Your first program

Save this as `hello.oz`:

```core
(block
  (fun square ((var x i64)) -> i64
    (block (* x x)))

  (fun <main> ()
    (block
      (output "The square of 7 is " (call square 7) "\n"))))
```

From the repository root, run it with the interpreter or LLVM JIT:

```sh
build/bin/qumiri --core -i hello.oz
build/bin/qumiri --core --jit -O2 -i hello.oz
```

Both print:

```text
The square of 7 is 49
```

To generate a native object file for integration with a host application:

```sh
build/bin/qumirc --core -O2 -c hello.oz -o hello.o
```

The object file still needs the runtime and a host entry point when linked.
The core-language CLI does not currently supply the C `main` wrapper needed
for a standalone executable; use `qumiri` for the runnable examples here.

For build requirements, see the [repository README](../../README.md#quick-start).
The [compiler guide](../ru/compiler.md) describes the other output modes.

Read the program from the outside in. The outer `block` contains declarations.
`fun` declares a function; its parameter list contains typed `var` declarations.
`-> i64` says that `square` returns a signed 64-bit integer. Its body is another
block whose final expression, `(* x x)`, supplies the result. `<main>` is the
entry point and returns no value. `call` invokes a function, while `output`
writes its arguments consecutively, without inserting spaces or newlines.

## Reading the syntax

oz-lang uses prefix notation: `(+ a b)` means "add `a` and `b`", and
`(* (+ a b) c)` means "multiply their sum by `c`". Parentheses determine the
structure, so operator precedence is not needed. An ordinary function call
uses `(call name arguments...)`; `(name arguments...)` is not shorthand for it.

The three bracket styles have different jobs:

| Delimiters | Purpose | Example |
|---|---|---|
| `(...)` | Expressions and declarations | `(+ x 1)` |
| `<...>` | Composite types | `<array i64 1>` |
| `[...]` | Bounds, multiple indices, generic parameters or arguments | `[0 9]` |

### Lexical syntax

Whitespace and indentation separate tokens but do not affect meaning. A
semicolon starts a comment that continues to the end of the line:

```text
(+ 2 3)  ; the value is 5
```

Identifiers can contain letters, digits after the first character, `_`, `$`,
and non-ASCII characters. A colon can occur inside a name, as in
`builtin::ctpop`. Use a bar-quoted identifier for a name containing spaces:
`|total price|`. Names such as `field_assign` use underscores; a hyphen is an
operator character, not an ordinary identifier character.

Strings use double quotes and characters use single quotes. Escapes include
`\n`, `\t`, `\\`, `\"`, and `\'`. Boolean literals are `#t` and `#f`.
Decimal numbers include `123`, `-42`, `1.25`, `.5`, and `3e-2`.

The special spelling `<main>` names the entry function. `nil` represents a
missing expression or type in compiler-oriented syntax; it is not a general
null value. A null pointer is constructed explicitly, as described under
[pointers](#pointers).

### Program order and scope

Use one outer `block` for a program. Its declarations must follow this order:

```text
(block
  (pragma ...)*
  (use ...)*
  (type ...)*
  functions, global variables, and other statements...)
```

The stars above mean "zero or more" and are not source syntax. Put executable
program logic in `<main>`. Every `block` creates a lexical scope; locals
declared inside it are unavailable outside it. The block's value is its last
expression, or `void` if it ends with a statement that produces no value.

## Values, variables, and conversions

### Scalar types

| Type | Meaning |
|---|---|
| `i8`, `i16`, `i32`, `i64`, `i128` | Signed integers of the stated bit width |
| `u8`, `u16`, `u32`, `u64`, `u128` | Unsigned integers of the stated bit width |
| `f32`, `f64` | Floating-point numbers of the stated bit width |
| `bool` | Boolean value |
| `char` | Unicode character |
| `string` | Managed string |
| `void` | No result, as for a procedure |

An unannotated integer literal starts as `i64`, and a floating-point literal as
`f64`; a literal adapts to the width its context asks for. `f32` widens to
`f64` implicitly, while `f64` to `f32` needs an explicit `cast`. Use
`(: expression type)` to give an expression a type annotation.
This is especially useful for numeric literals and aggregate constructors.
For a conversion of an already typed value, use `(cast expression type)`.

```core
(block
  (fun <main> ()
    (block
      (var count i64)
      (= count 3)
      (var step = (: 2 i32))
      (var total = (+ count (cast step i64)))
      (output total "\n")
      (output (cast 3.75 i64) "\n"))))
```

This prints `5` and `3` on separate lines. `(var count i64)` declares a typed
variable; `(= count 3)` assigns it. `(var step = ...)` declares and initializes
a variable whose type is inferred. Initialize a scalar before reading it:
declaration without an initializer does not promise a default value, and the
compiler checks definite assignment.

### Integer widths

Implicit integer conversion is allowed when every source value fits:

- Signed to signed, or unsigned to unsigned: the destination is at least as
  wide as the source.
- Unsigned to signed: the destination is strictly wider.
- Signed to unsigned: use an explicit `cast`.

For example, `u32` fits into `i64`, but `i32` does not implicitly fit into
`u32`. A narrowing conversion also needs `cast`. Integer literals that fit a
required type can be adapted by contextual typing; explicitly annotated
literals make the intended width clear in examples and low-level code.

### 128-bit integers

`i128` and `u128` support arithmetic, bitwise operations, comparisons, casts,
and storage in arrays and structures. The source literal representation is
limited to 64 bits, so construct larger values from smaller pieces:

```core
(block
  (fun <main> ()
    (block
      (var wide = (* (: 1000000000000 i128) (: 1000000000 i128)))
      (var quotient = (// wide (: 1000000000000 i128)))
      (output (cast quotient i64) "\n"))))
```

The result is `1000000000`. Converting large integers to `f64` can lose
precision above 2^53. The external scalar-call interface does not support
128-bit arguments or results, and SIMD vector elements cannot be 128-bit.

### Numeric conversion versus bit reinterpretation

`cast` converts a value. `bitcast` interprets its existing bits as a different
type. For ordinary numeric data, a bitcast requires equal byte sizes:

```core
(block
  (fun <main> ()
    (block
      (var value = 1.5)
      (var bits = (bitcast value u64))
      (assert (== bits (: 4609434218613702656 u64)))
      (assert (== (bitcast bits f64) value))
      (output (cast value i64) " " (bitcast bits f64) "\n"))))
```

The output is `1 1.5`. Numeric scalar/vector bitcasts are covered in more detail
under [vector conversions](#vector-conversions). Structs and scalar `bool` are
not supported as bitcast operands or targets. Pointer and string-handle
bitcasts are low-level operations with separate representation rules; see
[pointers](#pointers).

## Arithmetic and logic

| Operators | Meaning |
|---|---|
| `+`, `-`, `*` | Addition, subtraction, multiplication; unary `-` negates |
| `/` | Floating-point division, including when both operands are integers |
| `//`, `%` | Integer quotient and remainder |
| `==`, `!=`, `<`, `<=`, `>`, `>=` | Comparisons |
| `!`, `&&`, `\|\|`, `xor` | Logical negation, conjunction, disjunction, exclusive-or |
| `&`, `\|`, `^`, `<<`, `>>` | Integer bit operations and shifts |

### Division

`(/ 7 2)` produces `3.5`; `(// 7 2)` produces `3`; `(% 7 2)` produces `1`.
Integer division truncates toward zero. The remainder has the dividend's
sign: `(// -7 2)` is `-3`, and `(% -7 2)` is `-1`. Integer quotient and
remainder respect the operands' signedness. Division by zero is not checked.

Most binary numeric operations convert operands to an appropriate common
type. `/` produces `f64`, whereas `//` and `%` require integer operands.

The operators `+`, `-`, `*`, `/`, `//`, `&`, `|`, `^`, `&&`, `||`, and `xor`
accept more than two arguments and group from left to right. For example,
`(- 10 3 2)` means `(- (- 10 3) 2)` and produces `5`. Comparisons, shifts,
and `%` take exactly two arguments.

Scalar `&&` and `||` short-circuit: the second operand is evaluated only when
needed. For example, `(&& (!= divisor 0) (> (// value divisor) 2))` skips the
division when `divisor` is zero. Vector logic has different evaluation rules,
explained under [SIMD vectors](#simd-vectors).

## Functions and control flow

### Parameters and return values

A function declaration has a name, a parenthesized parameter list, an optional
return type, and a `block` body:

```text
(fun name ((var parameter Type) ...) -> ResultType
  (block ...))
```

Omitting `-> ResultType` makes the function return `void`. An empty parameter
list is still written as `()`. Function calls use `(call name arg...)`.

A non-void function can return its final expression, or use `(return value)`
to leave early. Every path reaching the end must produce a result of the
declared type. `(return)` is for void functions only.

### Conditionals as expressions

`(if condition then else)` evaluates one branch and can produce a value:

```core
(block
  (fun absolute ((var x i64)) -> i64
    (block (if (< x 0) (- x) x)))

  (fun classify ((var x i64)) -> string
    (block
      (if (< x 0) (return "negative"))
      (if (== x 0) (return "zero"))
      (return "positive")))

  (fun <main> ()
    (block
      (output (call absolute -7) " " (call classify 7) "\n"))))
```

This prints `7 positive`. A branch can be a `block` containing several
statements. Omit the `else` branch when using `if` only for its effects; a
conditional used as a value needs a result on both paths. Use `if`, not the
obsolete `cond` spelling.

### Loops

```core
(block
  (fun <main> ()
    (block
      (var total = 0)
      (var i = 0)
      (for i 1 5 1
        (block (= total (+ total i))))
      (output total "\n")

      (var countdown = 3)
      (while (> countdown 0)
        (block
          (output countdown " ")
          (= countdown (- countdown 1))))
      (output "\n")

      (var attempts = 0)
      (repeat
        (block (= attempts (+ attempts 1)))
        (< attempts 3))
      (output attempts "\n")
      (times 3 (block (output "*")))
      (output "\n"))))
```

This prints `15`, then the countdown `3 2 1`, then `3`, then `***`, each on
its own line. The countdown has a trailing space because `output` writes
exactly the arguments given.

`for` uses inclusive endpoints and an explicit step. Declare its counter
before the loop. A negative step counts down. `while` tests before each
iteration. `repeat` runs its body at least once and **continues while its
condition is true**, like a do-while loop. `times` repeats a body a given
number of times.

The bare statements `break` and `continue` leave the nearest loop or move to
its next iteration. They are not function calls. `return` leaves the function.
Scope cleanup for managed values also takes place on these exits.

### Assertions and formatted output

`(assert condition)` checks a program invariant. Use it in examples to state
what should be true after a computation. `output` accepts formatting wrappers:
`(fmt value width)` and `(fmt value width precision)`. For example,
`(output (fmt 3.14159 8 2) "\n")` reserves eight characters and prints two
fractional digits. `(input variable...)` reads into existing variables.

## Strings and arrays

### Strings

Strings are managed values. Concatenation uses `+`, indexing produces a
`char`, and slicing produces a `string`. String indices start at **1**;
slice endpoints are inclusive.

```core
(block
  (fun <main> ()
    (block
      (var message = (+ "hello" " world"))
      (output (index message 1) "\n")
      (output (slice message [1 5]) "\n")
      (output (slice message [7]) "\n"))))
```

This prints `h`, `hello`, and `w`. A single slice bound selects one character
as a string: `[7]` means `[7 7]`, not "from 7 to the end". Slices apply to
strings, not arrays or raw pointers.

### Arrays with explicit bounds

`<array T rank>` is an array type. Its bounds belong to each declaration,
not to its type. Each `[lower upper]` pair describes one dimension and includes
both endpoints. Bounds can be computed at runtime.

```core
(block
  (fun sum ((var n i64) (var values <array i64 1> [1 n])) -> i64
    (block
      (var total = 0)
      (var i = 0)
      (for i 1 n 1
        (block (= total (+ total (index values i)))))
      total))

  (fun <main> ()
    (block
      (var values <array i64 1> [1 4])
      (var i = 0)
      (for i 1 4 1
        (block (= values [i] (* i i))))
      (output (call sum 4 values) "\n")

      (var grid <array i64 2> [0 1] [1 3])
      (= grid [1 2] 42)
      (output (index grid [1 2]) "\n"))))
```

This prints `30` and `42`. The two array operations are deliberately different
forms: `(index values i)` reads, and `(= values [i] value)` writes. Multiple
indices use brackets for both reading and writing.

Array parameters share the caller's storage; they do not make deep copies of
the elements. Parameter bounds describe how the function indexes that storage.
Use the actual array's bounds when passing it. A zero-based array uses
`[0 (- n 1)]`. Elements may also be structures or other supported value types.
See [array representation](arrays.md) for layout and calling conventions.

## Named types and structures

### Defining and constructing a record

A structure groups named fields. Give a frequently used structure a name with
`type`, then construct it with `struct` and a type annotation:

```core
(block
  (type Point <struct (X f64) (Y f64)>)

  (fun squared_distance ((var p Point)) -> f64
    (block
      (+ (* (field p X) (field p X))
         (* (field p Y) (field p Y)))))

  (fun <main> ()
    (block
      (var p = (: (struct ((X 3.0) (Y 4.0))) Point))
      (output (call squared_distance p) "\n")
      (field_assign p X 6.0)
      (output (field p X) " " (field p Y) "\n"))))
```

This prints `25` and `6 4`. There are four distinct forms to remember:

| Form | Purpose |
|---|---|
| `<struct (X f64) (Y f64)>` | Describes the field names and types |
| `(struct ((X 3.0) (Y 4.0)))` | Constructs field values |
| `(field p X)` | Reads field `X` from object `p` |
| `(field_assign p X 6.0)` | Writes field `X` in object `p` |

The object comes **before** the field name in both field operations. The
double parentheses in `struct` enclose a list of `(name value)` pairs.
`(: constructor Point)` supplies the target type; `(cast constructor Point)`
also supplies a structure-construction context. With a target type, initializers
are matched by field name, so they need not follow declaration order. Unknown
or duplicate field names are errors. Initialize every field you intend to read;
omitting a field does not promise zero initialization.

An unannotated `struct` constructor infers an anonymous structure from its
field names and values. A named type makes signatures and diagnostics clearer.
`Point` and `<named Point>` refer to the same declaration. Named types retain
their identity during semantic analysis even though their underlying
representation is used for machine code. Equal storage layouts alone do not
make separately named types interchangeable.

### Nesting, value parameters, and mutation

Structures can contain other structures. Field access composes:
`(field (field segment Start) X)`. Structures can also be array elements and
function results. The following program contrasts a value parameter with a
reference parameter:

```core
(block
  (type Point <struct (X i64) (Y i64)>)
  (type Segment <struct (Start Point) (End Point)>)

  (fun translated ((var p Point) (var dx i64)) -> Point
    (block
      (field_assign p X (+ (field p X) dx))
      p))

  (fun shift ((var p <ref Point>) (var dx i64))
    (block (field_assign p X (+ (field p X) dx))))

  (fun <main> ()
    (block
      (var p = (: (struct ((X 1) (Y 2))) Point))
      (var q = (call translated p 10))
      (assert (== (field p X) 1))
      (assert (== (field q X) 11))
      (call shift p 3)

      (var segment = (: (struct ((Start p) (End q))) Segment))
      (var points <array Point 1> [0 1])
      (= points [0] (field segment Start))
      (= points [1] (field segment End))
      (call shift (index points 0) 2)
      (output (field (index points 0) X) " "
              (field (index points 1) X) "\n"))))
```

It prints `6 11`. `translated` modifies its local value of `p` and returns a
new value. `shift` receives an addressable object through `<ref Point>` and
modifies the caller's object. This also works for an array element. Passing a
structure by value copies its fields; a pointer field still points to the
same storage, and an array field does not imply a deep copy.

For layout and native call boundaries, see [structures and ABI](structs.md).
For managed fields and destruction, see [lifetime analysis](lifetime.md).

### Parametric types

A type declaration can take type parameters, integer value parameters, or
both. Arguments are written after the name inside `<named Name [Args]>`:

```text
(type Box [T] <struct (Value T)>)
(type Nullable [T] <struct (Value T) (Valid bool)>)
(type Decimal [(const Scale i32) (const Precision i32)] <struct (Value i64)>)

<named Box [i64]>
<named Nullable [Point]>
<named Decimal [2 10]>
```

`Nullable [Point]` has a `Point` field and a validity flag. The name alone does
not implement missing-value propagation: operations must check `Valid`.
Likewise, `Scale` and `Precision` distinguish decimal specializations; their
arithmetic semantics come from functions you define. The section on
[generics](#overloads-and-generic-functions) shows how functions use these types.


## References and pointers

### Reference parameters

`<ref T>` asks a function to operate on the caller's object. It is a
by-reference parameter convention, not a raw address stored as an integer.
Pass an addressable expression: a variable, a field, or an array element.
The function reads or assigns the referent using ordinary expressions.

```core
(block
  (fun bump ((var value <ref i64>))
    (block (= value (+ value 1))))

  (fun <main> ()
    (block
      (var count = 10)
      (call bump count)
      (var values <array i64 1> [0 1])
      (= values [0] 20)
      (= values [1] 30)
      (call bump (index values 1))
      (output count " " (index values 1) "\n"))))
```

This prints `11 31`. Passing a computed value such as `(+ count 1)` is not
equivalent to passing `count`: there is no caller-owned variable to update.

Access modifiers describe whether a type can be read or written. The spellings
include `<i64 (readonly)>`, `<ref string (writeonly)>`, and
`<named Point (readonly)>`. The mutually exclusive modifiers are `readonly`,
`writeonly`, and `mutable`; the default is readable and mutable. An output-only
reference still needs to be assigned before its contents are read.

### Pointers

`<ptr T>` is a first-class raw pointer. It carries no array bounds, ownership,
or lifetime information. Use it when working with host-provided storage or
low-level kernels. A one-dimensional array can be cast to a pointer to its
element type:

```core
(block
  (fun <main> ()
    (block
      (var values <array i64 1> [5 6])
      (= values [5] 11)
      (= values [6] 20)

      (var data = (cast values <ptr i64>))
      (assert (== (index data 0) 11))
      (= data [1] 42)
      (output (index values 6) "\n"))))
```

This prints `42`. The pointer is a zero-based view of the storage: `data[0]`
refers to the array's first element even though the array starts at index 5.
`(index data i)` loads at `data + i * sizeof(T)`, and `(= data [i] value)`
stores there. Dereferencing a single object is `(index data 0)`. Pointer
indexing accepts one index, not multidimensional indices or slices.

Keep the array alive while using the pointer. A pointer obtained from an array
does not own another copy of its storage. Returning a pointer to a local
array's storage leaves the caller without a valid lifetime.

Common pointer expressions are:

```text
(cast 0 <ptr u8>)                          ; null pointer
(== (cast data i64) 0)                     ; null check
(cast bytes <ptr i64>)                    ; reinterpret pointed-to storage
(cast (+ (cast bytes i64) offset) <ptr u8>) ; byte offset
```

Explicit `cast` supports pointer-to-pointer, pointer-to-integer, and
integer-to-pointer conversions. Conversion from a typed pointer to
`<ptr void>` may be implicit; the reverse requires an explicit cast.
These operations do not validate storage, alignment, or bounds.

Pointer and string-handle `bitcast` operations also support `i64` and `u64`
as address carriers, including for a 32-bit target; this is a special rule
separate from equal-size numeric bitcasts. Reinterpreting a string handle does
not convert text to a number or provide a portable string-buffer API.

Bind pointer-producing expressions to locals before indexing them. Current
pointer lowering expects a named collection; for example, write
`(var data = (cast (field column Data) <ptr u8>))` and then
`(index data row)`. There is no general source-level address-of operator.
Use a reference parameter when a function needs to modify an existing object.

## Overloads and generic functions

### Choosing between overloads

Enable user-defined overloads with `(pragma language overloads)` at the start
of the outer block. Functions may share a name when their parameter types
differ; a return type alone cannot distinguish them.

```core
(block
  (pragma language overloads)

  (fun pick ((var x i64)) -> i64
    (block (+ x 10)))

  (fun pick ((var x f64)) -> f64
    (block (+ x 0.5)))

  (fun <main> ()
    (block
      (output (call pick (: 7 i32)) " " (call pick 2.5) "\n"))))
```

This prints `17 3`. Exact parameter matches are preferred. Among conversions,
integer widening is preferred over integer-to-float conversion, so the `i32`
argument selects `pick(i64)`. Equally good candidates produce an ambiguity
error instead of an arbitrary choice.

### Type parameters

A generic function introduces type parameters in brackets after its name.
The compiler infers their concrete types from calls and compiles a specialized
function for each concrete signature. Calls with the same signature reuse the
specialization. Generic functions require the same `language overloads` pragma.

```core
(block
  (pragma language overloads)
  (type Box [T] <struct (Value T)>)

  (fun identity [T] ((var value T)) -> T
    (block value))

  (fun unwrap [T] ((var box <named Box [T]>)) -> T
    (block (field box Value)))

  (fun <main> ()
    (block
      (var number = (: (struct ((Value 42))) <named Box [i64]>))
      (var fraction = (: (struct ((Value 1.5))) <named Box [f64]>))
      (output (call identity (call unwrap number)) " "
              (call unwrap fraction) " " (call identity "hello") "\n"))))
```

This prints `42 1.5 hello`. Type parameters can occur inside composite types,
including `<ref T>`, `<array T 1>`, and `<named Box [T]>`. Multiple parameters
are independent: `[K V]` can describe a key type and a value type.

If a name has both concrete and generic overloads, a matching concrete
overload is preferred over the generic fallback. An external generic
function is not supported: specializations need a core-language body.

### Integer value parameters

`(const Name Type)` declares a compile-time integer parameter. It can
distinguish named types and supply a vector's lane count:

```core
(block
  (pragma language overloads)
  (type Decimal [(const Scale i32)] <struct (Raw i64)>)

  (fun scale_of [(const Scale i32)]
      ((var value <named Decimal [Scale]>)) -> i32
    (block Scale))

  (fun <main> ()
    (block
      (var price = (: (struct ((Raw 1234))) <named Decimal [2]>))
      (output (call scale_of price) " " (field price Raw) "\n"))))
```

The output is `2 1234`. The compiler infers `Scale = 2` from the argument
type, rather than from an extra runtime parameter. Only integer generic values
are supported. Concrete arguments are integer literals; inside a generic
declaration, another generic value parameter can be forwarded by name.
Arbitrary expressions are not generic argument syntax.

### Result-type inference

Normally every generic parameter is determined by the arguments. There is
also a narrower result-inference mechanism: the compiler can inspect return
expressions, casts, conditionals, and structure fields to infer parameters
that occur in the declared result type.

For example, a generic operation on `Nullable<T>` and `U` can return
`Nullable<R>` where `R` is inferred from `(+ value rhs)`. This lets an
`i64` payload combined with `f64` produce an `f64` payload. This is not
unrestricted inference from the surrounding assignment. If the arguments and
return body cannot determine a unique binding, the call is rejected. Prefer
argument-based inference for ordinary functions and use result inference when
the result genuinely depends on another operation's type.

## Operators, conversions, and printers

A named type becomes more useful when normal operators and `output` understand
it. Register a function with an `attrs` list after its signature and before
its body:

```core
(block
  (type Point <struct (X i64) (Y i64)>)

  (fun point_add ((var a Point) (var b Point)) -> Point
      (attrs (operator "+"))
    (block
      (: (struct ((X (+ (field a X) (field b X)))
                  (Y (+ (field a Y) (field b Y))))) Point)))

  (fun point_from_int ((var n i64)) -> Point
      (attrs (operator "cast"))
    (block (: (struct ((X n) (Y n))) Point)))

  (fun point_print ((var p Point)) (attrs print)
    (block
      (output "(" (field p X) ", " (field p Y) ")")))

  (fun <main> ()
    (block
      (var p = (: (struct ((X 1) (Y 2))) Point))
      (var q = (cast 3 Point))
      (output (+ p q) "\n"))))
```

This prints `(4, 5)`. `(operator "+")` associates a binary function with its
operand types. The declared name `point_add` remains callable normally.
Unary operators take one parameter; unary minus uses the key `"neg"`.

`(operator "cast")` registers a one-argument conversion to the function's
return type. Registered conversions participate in implicit coercions and
explicit `cast` expressions. A generic conversion may use the requested
target type when resolving its specialization. `print` registers a
one-argument printer returning `void`, invoked by `output`.

Operator and printer dispatch use types, so distinct function names do not
need the overloading pragma just to register these attributes. Generic
operator implementations still need `(pragma language overloads)`.

### Literal suffixes

`(attrs (literal "i"))` registers a function for a numeric literal suffix.
This is useful in a source module consumed by the Kumir frontend, for example
to turn `2i` into an imaginary number. The **core lexer does not accept
suffixed numeric literals**. In oz-lang, call the constructor function
explicitly. See [the complex-number module](../../qumir/modules/complex.oz)
for a module combining operators, conversions, a printer, and a literal
constructor.


## SIMD vectors

A vector is a fixed number of scalar lanes operated on together. An array has
runtime bounds and separately allocated storage; a vector is a value whose
lane count is part of its type.

### Construction and lane access

Write `<vec T N>` for a vector of `N` elements of type `T`, and
`(vec element...)` for a value. Supported lane counts are **2, 4, 8, 16, and
32**. Elements are signed/unsigned integers up to 64 bits, `f64`, or `bool`.
Strings, pointers, structures, and 128-bit integers are not vector elements.

```core
(block
  (use Vector)

  (fun <main> ()
    (block
      (var lanes = (: (vec 1 2 3 4) <vec i32 4>))
      (output lanes "\n")
      (= lanes (+ lanes 1))
      (output lanes "\n")
      (output (index lanes 2) "\n"))))
```

The output is:

```text
{1, 2, 3, 4}
{2, 3, 4, 5}
4
```

Vector indices start at **0** and can be computed at runtime. A read can also
index a computed vector, such as `(index (+ a b) 2)`. Assign a new vector value
to update the variable. Indexed lane assignment is not currently supported;
use a constructor or `builtin::select` to form a modified value. Keep indices
within `0` through `N - 1`.

`(vec 1 2)` infers `<vec i64 2>`, and `(vec 1.0 2.0)` infers
`<vec f64 2>`. Without an explicit target, the elements must have matching
types; `(vec 1 2.5)` does not automatically choose a common element type.
Annotate the constructor, for example `(: (vec 1 2.5) <vec f64 2>)`,
to provide a type to which each element can convert.

`(use Vector)` imports the source module's vector printer. The vector type,
operators, and `builtin::` vector operations are language features and do
not require this module merely to compute with them. The bundled module is
also available as `vector`.

### Lane-wise arithmetic and broadcasting

Operators apply independently to each lane. Two vector operands must have the
same lane count. When one operand is a scalar, it is broadcast across the
lanes: `(+ (vec 1 2) 10)` produces `(vec 11 12)`, and
`(- 10 (vec 1 2))` produces `(vec 9 8)`.

Arithmetic follows scalar type rules, including signedness, integer quotient,
and floating-point `/`. Vector casts change element types without changing
lane count. A comparison returns `<vec bool N>`, not one `bool` for the
whole vector.

### Masks and selection

Use `builtin::all` or `builtin::any` to reduce a mask for an `if` or loop
condition. Use `builtin::select` to choose a value separately in each lane:

```core
(block
  (use Vector)

  (fun <main> ()
    (block
      (var values = (vec -3 4 -1 8))
      (var positive = (> values 0))
      (var result = (call builtin::select positive values (vec 0 0 0 0)))
      (assert (call builtin::any positive))
      (assert (! (call builtin::all positive)))
      (output result "\n")
      (output (call builtin::movemask positive) "\n")
      (output (call builtin::shuffle result (vec 3 2 1 0)) "\n"))))
```

The output is:

```text
{0, 4, 0, 8}
10
{8, 0, 4, 0}
```

`movemask` returns a `u32` with bit `i` set when lane `i` is true.
Here lanes 1 and 3 are true, so the mask is `2 + 8 = 10`. `shuffle` selects
lanes by index; indices can be repeated or computed at runtime, and its index
vector must have the same length as the input. Each index must be in range.

Vector `!`, `&&`, and `||` compute lane-wise truth values. **Vector `&&` and
`||` evaluate both operands**, unlike scalar short-circuit logic.
`builtin::select` also evaluates both candidate vectors. It selects already
computed values; it cannot protect an invalid load or division in an unselected
lane. Use scalar control flow to avoid such operations.

### Vector intrinsics

The names below are used with `call`. `T` denotes a supported vector element
type and `I` an integer element type.

| Call | Result | Contract |
|---|---|---|
| `builtin::all(mask)` | `bool` | Every lane of `<vec bool N>` is true |
| `builtin::any(mask)` | `bool` | At least one lane is true |
| `builtin::movemask(mask)` | `u32` | Lane `i` becomes bit `i`; unused high bits are zero |
| `builtin::select(mask, a, b)` | `<vec T N>` | Boolean mask and two value vectors of the same type and length |
| `builtin::shuffle(v, indices)` | Same type as `v` | Integer index vector of length `N`; result lane `i` is `v[indices[i]]` |
| `builtin::gather(ptr, indices)` | `<vec T N>` | Load `N` elements from `<ptr T>` using `<vec I N>` indices |
| `builtin::prefetch(ptr)` | `void` | A memory-access hint; does not load a value into the program |

`gather` indices count **elements**, not bytes. Every selected address must
be readable; this operation has no mask argument. `prefetch` is a hint,
not a promise about speed, ordering, or availability of data. Its pointer
expression is still evaluated.

### Vector conversions

Use `cast` for lane-wise numeric conversion and `bitcast` to reinterpret
the bytes of a vector or scalar:

```core
(block
  (use Vector)

  (fun <main> ()
    (block
      (var integers = (: (vec 1 2) <vec i32 2>))
      (output (cast integers <vec f64 2>) "\n")

      (var packed = (bitcast integers i64))
      (var restored = (bitcast packed <vec i32 2>))
      (assert (call builtin::all (== integers restored)))

      (var mask = (vec #t #f #t #f))
      (output (bitcast mask <vec u8 4>) "\n"))))
```

This prints `{1, 2}` and `{1, 0, 1, 0}`. A numeric bitcast requires equal
total byte sizes, but it may change both the element width and lane count.
For example, `<vec i32 2>`, `<vec i16 4>`, and `i64` all occupy eight bytes.
Byte interpretation depends on the target representation; a round trip
preserves the original bits.

A boolean vector can be a bitcast **source**, with each lane represented by
a byte containing 0 or 1. It cannot be a bitcast target. To form a boolean
vector from numeric lanes, use a comparison or a numeric `cast` instead.
Scalar `bool` and structures are outside this bitcast facility.

### Generic vector functions

The lane count can be an integer generic parameter inferred from the argument:

```core
(block
  (pragma language overloads)

  (fun last [T (const N i64)] ((var values <ref <vec T N>>)) -> T
    (block (index values (- N 1))))

  (fun <main> ()
    (block
      (var pair = (vec 1.5 2.5))
      (var four = (: (vec 1 2 3 4) <vec i32 4>))
      (output (call last pair) " " (call last four) "\n"))))
```

This prints `2.5 4`. The two calls specialize both `T` and `N`. A named
generic vector type can be declared as `(type V [T (const N i64)] <vec T N>)`
and instantiated with `<named V [i32 4]>`.

### Putting it together: a filtered sum

The following kernel reads an array through a pointer, processes groups of
four elements, selects values above a threshold, and handles the remaining
elements with a scalar loop. The tail matters because `gather` always reads
all its selected elements.

```core
(block
  (fun sum_above ((var data <ptr i64>) (var count i64)
                 (var threshold i64)) -> i64
    (block
      (var total = 0)
      (var offset = 0)
      (while (<= (+ offset 4) count)
        (block
          (var indices = (+ (vec 0 1 2 3) offset))
          (var values = (call builtin::gather data indices))
          (var selected = (call builtin::select
            (> values threshold) values (vec 0 0 0 0)))
          (var lane = 0)
          (for lane 0 3 1
            (block (= total (+ total (index selected lane)))))
          (= offset (+ offset 4))))
      (while (< offset count)
        (block
          (var value = (index data offset))
          (if (> value threshold)
            (= total (+ total value)))
          (= offset (+ offset 1))))
      total))

  (fun <main> ()
    (block
      (var values <array i64 1> [0 6])
      (var i = 0)
      (for i 0 6 1
        (block (= values [i] (+ i 1))))
      (var result = (call sum_above (cast values <ptr i64>) 7 3))
      (assert (== result 22))
      (output result "\n"))))
```

The result is `4 + 5 + 6 + 7 = 22`. The example demonstrates the memory
contract and scalar tail, not a performance claim. LLVM chooses how the
vector operations map to instructions for a particular target; a vector
expression is not a guarantee of one SIMD instruction.


## Source modules

A source module is a `.oz` file containing an outer block with functions,
types, globals, and imports, but **no `<main>` and no executable top-level
statements**. Import it with `use`; the module's file stem is its name.

Save this as `geometry.oz`:

```core
(block
  (type Point <struct (X i64) (Y i64)>)

  (fun squared_length ((var p Point)) -> i64
    (block
      (+ (* (field p X) (field p X))
         (* (field p Y) (field p Y))))))
```

Save the client beside it as `geometry_demo.oz`:

```core
(block
  (use geometry)

  (fun <main> ()
    (block
      (var p = (: (struct ((X 3) (Y 4))) Point))
      (output (call squared_length p) "\n"))))
```

Run `build/bin/qumiri --core -i geometry_demo.oz` to print `25`.
The loader combines the module and program into one compilation unit.
Imported operators, conversions, and printers are registered as well as
ordinary functions and types.

### Finding modules

A `use` first checks registered runtime modules. Source module files are
resolved in this order:

1. Files registered explicitly with `--module file.oz`.
2. Bundled module directories located relative to the compiler executable.
3. The main source file's directory.
4. Directories supplied through `--module-path`, in registration order.

For example, if `geometry.oz` lives in `modules/`:

```sh
build/bin/qumiri --core --module-path modules -i geometry_demo.oz
build/bin/qumiri --core --module modules/geometry.oz -i geometry_demo.oz
```

The same flags are accepted by `qumirc`. `use` accepts an identifier or a
quoted name, such as `(use "geometry")`.

A module's imports are transitive. Dependency cycles are errors. All top-level
declarations are exported into a shared namespace; source modules do not
currently provide `private`, explicit export lists, or separate namespaces.
Choose names that will not collide with other imports. Global initialization
runs in dependency order and destruction in reverse order.

The language itself does not define an I/O runtime. The CLI tools provide
`System` as a host prelude so that `output` and ordinary runtime functions
are available. An application embedding the compiler controls its own module
set. Kumir-specific aliases such as `Файлы` belong to that frontend rather
than to the core language. The Kumir frontend can also import source modules.

## Host functions and suspension

### External functions

An external declaration describes a function implemented by a native symbol.
It retains a syntactic `(block)` body, but the compiler does not use that body
as the implementation:

```core
(block
  (fun absolute_native ((var value i64)) -> i64
      (attrs (extern "llabs"))
    (block))

  (fun <main> ()
    (block
      (output (call absolute_native -42) "\n"))))
```

This native CLI example prints `42`. `(extern llabs)` and
`(extern "llabs")` select the same symbol. Bare `extern` uses the declared
function name as the native symbol.

An extern declaration does not supply an implementation or load a library.
The host must make the symbol available with the matching argument and result
conventions. A browser target needs an appropriate implementation or import in
its runtime; a symbol available in the native process is not automatically
available in WebAssembly. Aggregate and reference boundaries require the
host's ABI conventions; see [structures and ABI](structs.md) and the
[FFI notes](ffi-template-approach.md).

### Futures and await

`<future T>` describes an operation that may suspend before producing `T`.
`(await expression)` obtains the result of an expression of that type.
Runtime functions can return futures; the coroutine transform propagates
suspension through their callers and inserts awaits at the relevant call sites.

This is why ordinary-looking calls to asynchronous executor operations can
suspend a program. There is no general `async` function declaration or task
scheduler in the source syntax. The host runtime drives completion of pending
operations. Writing a future type alone does not provide that runtime.
See [coroutines](coroutine.md) for propagation, lowering, and the host protocol.

### Function attributes

The optional `(attrs ...)` list follows the return type, if present, and
precedes the body. Multiple attributes can be combined:

```text
(fun twice [T] ((var x T)) -> T (attrs cacheable inline)
  (block (+ x x)))
```

| Attribute | Meaning |
|---|---|
| `extern`, `(extern symbol)` | Bind to a host/native implementation |
| `(operator "OP")` | Register an operator or conversion |
| `print` | Register the parameter type's printer |
| `(literal "suffix")` | Register a numeric suffix constructor for the Kumir frontend |
| `used` | Keep a function reachable even without an explicit call |
| `cacheable` | Mark a function for the compiler's native object-cache mechanism |
| `inline` | Request LLVM `alwaysinline` for an ordinary definition |
| `(expect_after expression)` | Check the function's postcondition on exit |
| `(expect_before expression)` | Parsed for compatibility, but currently not applied |

`inline` requires a body available to LLVM. At optimization level zero the
attribute is retained without guaranteeing inlining. It does not change the
interpreter or turn an external declaration into an inline definition.
Recursive calls cannot always be inlined. Native object caching can import
reachable inline bodies and a bounded set of small helpers so callers can
still optimize through them; marking a function `cacheable` is not a purity
assertion or a promise that compilation will be skipped.

Unknown bare attributes are currently ignored. Use the documented spellings;
a misspelled attribute need not produce a diagnostic.

## Builtin operations

The `builtin::` functions below are supplied by the builtin module. They
complement the [vector intrinsics](#vector-intrinsics).

### Counting bits

```core
(block
  (fun <main> ()
    (block
      (output (call builtin::cttz (: 8 u64)) " "
              (call builtin::ctlz (: 1 u64)) " "
              (call builtin::ctpop (: 255 u64)) "\n")
      (assert (== (call builtin::cttz (: 0 u64)) 64))
      (assert (== (call builtin::ctlz (: 0 u64)) 64)))))
```

This prints `3 63 8`. All three functions accept `u64` and return `i64`.
`cttz` counts trailing zero bits, `ctlz` counts leading zero bits, and
`ctpop` counts set bits. Both zero-counting functions return 64 for zero.

### Byte storage

| Function | Parameters | Result |
|---|---|---|
| `builtin::memcpy` | Destination `<ptr u8>`, source `<ptr u8>`, byte count `i64` | Destination pointer |
| `builtin::memmove` | Same as `memcpy` | Destination pointer |
| `builtin::memcmp` | Two `<ptr u8>` values and byte count `i64` | `i32` comparison result |

`memcpy` requires non-overlapping ranges; `memmove` supports overlap.
`memcmp` returns zero for equal ranges and a negative or positive value for
lexicographic byte ordering. Do not assume its nonzero result is exactly
`-1` or `1`. These functions use raw pointer ranges; they do not acquire
ownership or check array bounds.

### Builtin byte matching

| Call | Result | Match representation |
|---|---|---|
| `builtin::byte_match8(data, byte)` | `u64` | Byte `i` is `0xff` on a match, otherwise zero |
| `builtin::byte_match16(data, byte)` | `u32` | Bit `i` is set on a match; high 16 bits are zero |
| `builtin::byte_match_backend()` | `i64` | 0: portable; 1: NEON byte masks; 2: SSE2 compact masks |

Both match functions take `<ptr u8>` and `u8`. Pointers can be unaligned but
must address at least 8 or 16 readable bytes respectively. Each function
reads only its group. The first byte maps to the least significant byte or
bit of the result, including on big-endian targets.

For a nonzero 8-byte match mask, `(// (call builtin::cttz mask) 8)` identifies
the first matching byte. Before iterating matches by clearing the lowest set
bit, keep only one flag per matching byte with the mask `0x8080808080808080` (write its decimal
value `9259542123273814144` as a `u64` literal in core source).
For the 16-byte compact mask, use `cttz` directly after widening to `u64`.
Check for zero before interpreting a count as a match position.

LLVM selects the instructions for the target and can fold the backend selector
to a constant. The interpreter implements the same comparison semantics.
Both match widths remain available regardless of the backend selector.

## Syntax reference and compiler output

### Types

| Shape | Meaning |
|---|---|
| `<array T rank>` | Array; bounds appear on declarations |
| `<struct (name T) ...>` | Record with named fields |
| `Name` or `<named Name>` | Declared/imported named type |
| `<named Name [Args...]>` | Instantiated generic named type |
| `<vec T N>` | Fixed-size SIMD vector |
| `<ptr T>` | Raw pointer |
| `<ref T>` | Reference, typically a function parameter |
| `<fun Result (ArgumentTypes...)>` | Function signature type |
| `<future T>` | Result of a potentially suspending operation |

The long named-type form may also carry its underlying type, for example
`<named Point <struct (X i64) (Y i64)>>`. Ordinary source should declare the
type once with `type` and refer to it by name. Function signature types
describe call interfaces; they do not introduce lambda or closure syntax.

The parser also recognizes `<tensor T dimensions...>`, but it does not have
a complete execution path. Use arrays for multidimensional storage and vectors
for SIMD computations; parser acceptance alone is not a guarantee of backend
support.

### Expression and statement forms

These are syntax patterns, not a runnable program. Lowercase placeholders
stand for expressions, names, or types as appropriate.

```text
(block expression...)
(var name type)
(var name = expression)
(var name <array type rank> [lower upper]...)
(vars (var ...) ...)
(= name expression)
(= name [indices...] expression)

(if condition then-expression else-expression)
(while condition body)
(repeat body condition)
(for counter first last step body)
(times count body)
break
continue
(return)
(return value)

(fun name [generic-parameters...] (parameters...) -> result (attrs ...) body)
(call function arguments...)
(type name [generic-parameters...] underlying-type)
(use module)
(pragma language overloads)

(: expression type)
(cast expression type)
(bitcast expression type)
(struct ((field value)...))
(field object field-name)
(field_assign object field-name value)
(vec elements...)
(index collection index)
(index collection [indices...])
(slice string [first last])
(await future)

(input variables...)
(output values... (fmt value width precision))
(assert condition)
```

The generic parameter list, return type, and attributes of a function are
optional; its parameter list and block body are required. The else branch of
`if` and the second bound of a string slice are optional, with the meanings
described earlier. Core syntax includes a step position in `for`; `nil` can
represent a missing step in a compiler dump.

### Current boundaries

The guide's complete programs use supported execution paths. Some constructs
need particular care in the current implementation:

- Vector lane reads work, but indexed lane assignment does not.
- A string-valued `if` expression can fail ownership analysis. For string
  results, use explicit `return` statements in the branches instead.
- Returning a managed string field from a generic structure is not currently
  reliable in either the interpreter or JIT. The record examples use numeric
  fields; passing a standalone string to a generic function is supported.
- `tensor` has parser support without a complete execution path.
- The core lexer accepts literal-constructor declarations but not suffixed
  numeric literal expressions.
- Generic value parameters are integers, and generic external declarations
  are unsupported.
- Native object emission works, but linking a standalone core program needs
  a host entry-point wrapper and runtime.

These are implementation boundaries, not alternative syntax to work around
by adding annotations. For a specific program, diagnostics and execution
tests are more informative than a successful parse alone.

### Inspecting the compiler's view

The command-line tools can show each stage:

```sh
build/bin/qumirc --core --ast hello.oz
build/bin/qumirc --core --transformed-ast hello.oz
build/bin/qumirc --core --ir hello.oz
build/bin/qumirc --core --llvm -O2 hello.oz
```

The interpreter tool offers `--print-ast`, `--print-transformed-ast`,
`--print-ir`, and, with `--jit`, `--print-llvm` or `--print-asm`.
These are useful for understanding inferred types, specialization, and the
effect of optimization.

The core printer emits a canonical representation, not necessarily the
original spelling. It escapes strings and characters, quotes names containing
spaces, and prints generic value parameters as `(const Name Type)`. A named
type may appear in its long `<named ...>` form. Annotations depend on the
printer's type mode. External attributes print the selected native symbol;
`used` and the ignored `expect_before` are not reproduced in the printed
attribute list.

Transformed output can contain compiler-generated lifetime operations such as
`borrow`, `move`, `retain`, `own-literal`, `replace`, `destroy`,
`cleanup-exit`, and `cleanup-global`. These explain ownership and cleanup;
ordinary programs use variables, assignment, and structured control flow.
Read [lifetime analysis](lifetime.md) for those internal forms and
[the transformation pipeline](transformation-pipeline.md) for pass ordering.
The [architecture overview](overview.md) connects the language to the IR,
interpreter, LLVM backend, and runtime.
