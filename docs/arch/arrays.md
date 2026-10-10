# Array Representation

Kumir arrays are written with `таб`:

```kumir
цел таб v[1:n]
вещ таб a[0:n-1, 0:m-1]
```

The AST type is `TArrayType(elementType, arity)`. The bounds are not part of
the type identity itself; they live on the declaration (`TVarStmt` / function
parameter) as one `[left:right]` pair per dimension. In core/AST goldens this
shows up as a type plus a separate bounds list:

```text
(var a <array f64 2> [0 (- n 1)] [0 (- m 1)])
```

## Runtime Value

At IR/LLVM level an array value is just a pointer to the first element:

```cpp
FromAstType(TArrayType(T, n)) == Ptr(FromAstType(T))
```

There is no array header in the runtime allocation: no length, no bounds, no
rank, and no stride table are stored next to the elements. Allocation is done
through the System runtime:

```cpp
void* array_create(size_t sizeInBytes);
void  array_destroy(void* ptr);
void  array_str_destroy(void* ptr, size_t elements);
```

`array_create` allocates an 8-byte-aligned zero-filled byte buffer. Ordinary
arrays are destroyed with `array_destroy`. Arrays of `лит` are destroyed with
`array_str_destroy`, which releases every stored string pointer before freeing
the buffer.

## Bounds and Layout Metadata

Because the runtime pointer has no header, the compiler creates separate
hidden storage for array layout metadata. During IR lowering,
`LowerArrayLayout` evaluates each declared bound and stores, per dimension:

| Field | Meaning |
|-------|---------|
| lower bound | declared left bound, e.g. `1` in `[1:n]` |
| dim size | `right - left + 1` |
| stride | cumulative element count from this dimension |

The same layout lowering runs for local/global array declarations and for
array parameters inside the callee. For function parameters, the declared
parameter bounds are expressions in the callee scope, commonly using scalar
size parameters:

```kumir
алг matvec(цел n, вещ таб x[0:n-1], рез вещ таб y[0:n-1])
```

Here `n` is passed separately, and the callee reconstructs the layout metadata
for `x` and `y` from its parameter declarations.

## Element Addressing

Arrays are flattened into one row-major allocation: the last dimension is
contiguous. For an access `a[i0, i1, ..., ik]`, lowering computes a byte offset
equivalent to:

```text
linear =
    (i0 - lb0) * size1 * size2 * ... * sizek
  + (i1 - lb1) * size2 * ... * sizek
  + ...
  + (ik - lbk)

byteOffset = linear * sizeof(element)
address    = basePointer + byteOffset
```

For example, `вещ таб a[0:n-1, 0:m-1]` stores `a[i,j]` at:

```text
base + ((i * m) + j) * 8
```

The implementation emits normal pointer arithmetic in IR:

```text
offset = LowerIndices(symbol, indices, elemByteSize)
addr   = arrayPtr + offset
```

Loads use `lde`; scalar stores use `ste`; struct elements are copied with
`copy` because the element value may be address-backed.

The same address calculation is used when an array element is passed to a
reference parameter. For example, a core call like `(call bump (index i a))`
lowers the indexed expression as an lvalue address:

```text
addr = arrayPtr + LowerIndices(symbol, [i], elemByteSize)
arg addr
call bump
```

For one-dimensional pointer values, the offset is simply `i * elemByteSize`.
LLVM codegen lowers the IR pointer addition to a byte-addressed GEP.

## Passing Arrays to Functions

Array arguments are passed by pointer. The callee receives the same backing
allocation that the caller owns; no element copy is made at the call boundary.
This is why procedures can fill output arrays efficiently:

```kumir
алг fill(цел n, рез цел таб a[0:n-1])
нач
  цел i
  нц для i от 0 до n-1
    a[i] := i
  кц
кон
```

Parameter modes are enforced by semantic type flags:

| Mode | Effect for arrays |
|------|-------------------|
| `арг` | input array; elements may be read according to type flags |
| `рез` | output array; reading elements is rejected |
| `арг рез` | in/out array; elements may be read and written |

The ABI is still pointer-based in all three cases. The difference is semantic
checking and generated read/write permissions, not a different runtime
representation.
