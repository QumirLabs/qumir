# String Representation and Calling Convention

## Native `TString` (C++ / WASM heap)

Every mutable string is a heap-allocated `TString` object:

```cpp
struct TString {
    int*    Utf8Indices;  // lazy: symbol index -> byte offset in Data
    int64_t Symbols;      // Unicode codepoint count (filled by Utf8Indices build)
    int64_t Rc;           // reference count
    int64_t Length;       // byte length of the UTF-8 payload
    char    Data[0];      // flexible array: the UTF-8 bytes, NUL-terminated
};
```

The value that is passed around at runtime is `char* TString::Data`: a pointer
into the middle of the struct, just past the header. To reach the header from a
data pointer: `(TString*)(ptr - offsetof(TString, Data))`.

This means a string value looks like a plain C string to any code that just
reads bytes, while the reference count and metadata live at negative offsets
before it.

`str_retain(char*)` increments `Rc`; `str_release(char*)` decrements it and
frees the whole allocation (header + data) when it reaches zero.

The lowering code tracks string ownership with an internal `EOwnership` flag:

| Ownership | Meaning |
|-----------|---------|
| `Borrowed` | Pointer aliases storage owned somewhere else |
| `Owned` | Current expression owns one RC reference and must transfer or release it |
| `Unknown` | Non-string or not relevant for string lifetime handling |

Important sources of ownership:

| Expression/source | Ownership result |
|-------------------|------------------|
| string literal | raw read-only pointer, no `TString` header and no RC |
| string variable / parameter read | `Borrowed` |
| string array element read | `Borrowed` |
| runtime string constructor result | `Owned`, usually RC=1 |
| string-returning user call | `Owned` |

Kumir strings are 1-indexed sequences of Unicode codepoints. `Utf8Indices` is
built lazily on the first symbol-position access (`str_symbol_at`,
`str_slice`, etc.). Once built, it maps `Utf8Indices[i]` to the byte offset of
the i-th codepoint start inside `Data`.

## String Literals

Compile-time string literals are emitted as read-only LLVM globals containing
raw UTF-8 bytes. Their pointers are used directly: no `TString` header, no
reference count. All string runtime functions that accept a `const char*`
argument check for this case: a read-only literal is never passed to
`str_release`.

When a literal must be stored into a mutable `лит` variable/array slot, or
passed to a function that needs a real managed string object, lowering first
calls `str_from_lit(literal)`. The result is a `TString::Data` pointer with
RC=1.

## Function Call Convention for Strings

At the ABI level a `лит` value is passed and returned as `char*` / `ptr i8`.
The ownership convention is the part that matters.

### String Arguments

Arguments are normally passed as borrowed pointers. A call does not retain
ordinary string arguments just because they are passed to a function.

If the argument expression is an owned temporary created only for this call,
lowering releases it immediately after the call returns. Example cases:

- a string literal materialized through `str_from_lit` for a non-external user
  call;
- a literal passed to an external function marked `RequireArgsMaterialization`;
- a temporary string expression such as concat/slice/cast result used only as
  an argument.

In IR shape, this is:

```text
tmp = call str_from_lit(literal)   ; RC=1, owned temporary
arg tmp
call f
arg tmp
call str_release                  ; drop caller's temporary reference
```

External runtime functions may opt into argument materialization with
`RequireArgsMaterialization`. This is used by string functions that inspect
`TString` metadata (`Length`, `Utf8Indices`, etc.) and therefore cannot safely
operate on a raw string literal pointer.

### String Return Values

A function returning `лит` returns an owned pointer. The callee transfers one
RC reference to the caller. Runtime constructors such as `str_concat`,
`str_slice`, `str_from_unicode`, `str_input`, and `str_from_lit` return new
managed strings with RC=1.

The caller then either stores/transfers that owned value or releases it when it
is only a temporary expression. Block/sequence lowering releases owned string
expression results that are not consumed by an assignment, return, or other
ownership-taking operation.

The LLVM runner has a special `ReturnTypeIsString` marker for top-level JIT
execution: after converting a returned `char*` to text for display, it calls
`str_release` to drop the result owned by the caller.

### Assignment and RC Updates

Assigning to a string variable or string array element follows a replace
protocol:

1. If RHS is a raw literal, materialize it with `str_from_lit` and treat it as
   owned.
2. If RHS is borrowed, call `str_retain(rhs)` because the destination will now
   own a reference.
3. Load the old destination value and call `str_release(old)`.
4. Store the RHS pointer into the destination.

Owned RHS values do not need an extra retain: the destination takes over the
reference already held by the expression. Borrowed RHS values must be retained
before the old value is released; this also handles self-assignment safely.

Reference parameters (`рез лит` / `арг рез лит`) use the same replace protocol
through the referenced address: retain the new borrowed value when needed,
release the previous value behind the reference, then store the new pointer.

### Local and Array Destruction

Local `лит` variables get a pending destructor that calls `str_release` on
scope exit. The synthetic `$$return` variable is excluded because its value is
owned by the caller after function return.

Arrays of strings use `array_str_destroy(ptr, elements)`. It walks the flat
array of `char*` elements, calls `str_release` for each stored pointer, and
then frees the array buffer.

## Browser / WASM Dual-Handle Scheme

The JavaScript `string.js` runtime splits string values into two namespaces by
sign of the `int32` handle:

| Handle value | Meaning |
|--------------|---------|
| >= 0 | Pointer into WASM linear memory: a `TString::Data` address or a literal `const char*` |
| < 0 | Index into a JavaScript-managed `STRING_POOL` Map |

The JS pool stores entries of the form
`{ value: string, refs: number, positions: Array|null }`. `positions` is the
JS-side equivalent of `Utf8Indices`: a lazy array of UTF-16 index offsets for
Unicode-safe character operations.

Negative handles arise when JS-side string operations, such as reading from an
`<input>`, need to produce a string that the WASM code can use as a `лит`
value. Reference counting for negative handles is managed entirely in JS.

`result.js` interprets handles with the same rules as `string.js` so that both
the interpreter and WASM paths display string return values consistently in the
playground.
