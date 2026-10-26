# The mllvm IR subset

This document is the contract between `mllvm` and the programs it runs.
Anything that is not described here is rejected by the parser or the
verifier; nothing outside the subset is silently miscompiled.

## Types

| Type | Width | Notes |
| --- | --- | --- |
| `i1` | 1 bit | `true` is 1, `false` is 0 |
| `i8` | 8 bits | |
| `i16` | 16 bits | |
| `i32` | 32 bits | the type of a C `int` |
| `i64` | 64 bits | |
| `ptr` | 64 bits | also written `i8*`, `i32*`, ... |

`void` is accepted as a return type.  A `*` after any type is read as a
pointer, so `i32*` and `i32 *` both mean `ptr`.  There is one pointer type;
the pointee type is only used by the instruction that performs the access.

## Module

```
@g = global i32 41          ; a writable global scalar
@c = constant i64 -7        ; a constant scalar
@e = external global i32    ; declared but not defined

declare i32 @putchar(i32)   ; a declaration
declare void @print_i64(i64)

define i32 @main() {
entry:
  ...
}
```

`target`, `attributes`, `module asm` and metadata lines are ignored.  Global
aggregates, strings and initialisers that are not integer constants are not
supported.

## Blocks and instructions

A block label is an identifier followed by `:`.  A function body is executed
from its first block.  Every block must end with `ret`, `br` or `unreachable`;
the verifier reports a block that does not.

All instructions use the textual LLVM form.  The result name (`%x =`) is
optional for instructions that produce a value.

### Arithmetic

```
%r = add  i32 %a, %b
%r = sub  i64 %a, 1
%r = mul  i32 %a, %b
%r = udiv i32 %a, %b
%r = sdiv i32 %a, %b
%r = urem i32 %a, %b
%r = srem i32 %a, %b
%r = and  i32 %a, %b
%r = or   i32 %a, %b
%r = xor  i32 %a, %b
%r = shl  i32 %a, %b
%r = lshr i32 %a, %b
%r = ashr i32 %a, %b
```

The second operand may repeat its type (`add i32 %a, i32 %b`) or omit it
(`add i32 %a, %b`).  Results are truncated to the instruction type, so
arithmetic wraps like LLVM's integer arithmetic.  `sdiv` and `srem` interpret
their operands as signed; `udiv` and `urem` as unsigned.  Division by zero is
a runtime error.  Shift amounts are taken modulo the bit width.

### Comparison and selection

```
%c = icmp eq  i32 %a, %b
%c = icmp ne  i32 %a, %b
%c = icmp ugt i32 %a, %b      ; also uge, ult, ule
%c = icmp sgt i32 %a, %b      ; also sge, slt, sle
%v = select i1 %c, i32 %x, i32 %y
```

`icmp` produces an `i1`.  `select` returns the second value when the
condition is non-zero and the third value otherwise.

### Casts

```
%r = zext  i8  %a to i32
%r = sext  i8  %a to i64
%r = trunc i64 %a to i32
%r = bitcast i32 %a to i32
%r = ptrtoint ptr %p to i64
%r = inttoptr i64 %n to ptr
```

`zext` zero-extends, `sext` sign-extends, `trunc` keeps the low bits.  The
three pointer/integer casts are bit-preserving (there is only one pointer
representation).

### Control flow

```
br label %next
br i1 %cond, label %then, label %else

%v = phi i32 [ %a, %entry ], [ %b, %loop ]

ret void
ret i32 %v
```

`phi` selects the incoming value whose label is the block the interpreter
came from.  A `phi` in the entry block has no predecessor and is an error.

### Memory

```
%p = alloca i32
%q = alloca i32, i64 4
store i32 40, ptr %p
%v = load i32, ptr %p
```

`alloca` returns a pointer to zeroed memory aligned to 16 bytes.  The
optional count must be a constant.  `load` and `store` use the little-endian
layout and the width of their value type.  Every access is bounds-checked
against the interpreter arena (8 MiB, starting at `0x1000`; globals are
allocated first).  The legacy `i32*` spelling is accepted for the pointer
operand of `load` and `store`.

### Calls

```
%r = call i32 @f(i32 %a, i64 %b)
call void @print_i32(i32 %v)
```

Only direct calls to a named function are supported.  Arguments are evaluated
in the caller, bound to the parameter names of the callee and executed with a
recursive interpreter call.  Recursion is limited to 10000 frames.

Declared functions that are not defined are looked up as builtins:

| Builtin | Signature | Effect |
| --- | --- | --- |
| `@putchar` | `i32 (i32)` | writes the low byte to stdout, returns it |
| `@putc` | `i32 (i32)` | alias of `@putchar` |
| `@print_i32` | `void (i32)` | prints the signed value and a newline |
| `@print_i64` | `void (i64)` | prints the signed value and a newline |
| `@print_u64` | `void (i64)` | prints the unsigned value and a newline |
| `@exit` | `void (i32)` | stops the program with that exit status |
| `@abort`, `@llvm.trap` | `void ()` | stops the program with status 134 |

Any other undefined function is a runtime error.

## Optimization

`mllvm opt` (and `mllvm run --opt`) folds an instruction when all of its
operands are known constants:

* arithmetic and bitwise instructions, including the division guards;
* `icmp`;
* `zext`, `sext`, `trunc`, `bitcast`, `ptrtoint`, `inttoptr`;
* `select` with a constant condition.

Folded results are propagated to later instructions.  The printer emits a
folded value as `add <ty> <value>, 0`, which is valid IR for every integer
type and lets the output be parsed and executed again.

The folder does not attempt dead-code elimination, inlining, loop transforms
or alias analysis.

## Limits

These are deliberate, and rejected rather than approximated:

* floating point (`float`, `double`, `half`, ...);
* vectors and aggregate types (`[N x T]`, `{...}`, `<N x T>`);
* `getelementptr`, `extractvalue`, `insertvalue`;
* `switch`, `invoke`, `landingpad`, exceptions and unwinding;
* `atomic` / `volatile` memory operations and fences;
* variadic calls and function pointers;
* global arrays and string initialisers;
* alignment, TBAA and other metadata are accepted syntactically but have no
  effect;
* the verifier checks that every referenced value and label exists, but it
  does not prove SSA dominance for every path.  The interpreter still faults
  if a value is used before it has been produced on the executed path.
