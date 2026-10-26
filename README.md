# mllvm

`mllvm` is a small, clean-room implementation of the **LLVM IR core**: a
parser, a verifier, a constant folder and an interpreter for textual LLVM IR.
It is written in portable C11, has no dependencies and does not link against
LLVM.

The name stands for *my LLVM*.  It is **not** a fork of LLVM and it is not a
drop-in replacement for `clang`, `opt` or `lli`.  It implements a documented
subset of the IR and the whole subset is exercised by the test suite.

```
$ cat hello.ll
define i32 @main() {
entry:
  call i32 @putchar(i32 72)   ; H
  call i32 @putchar(i32 105)  ; i
  call i32 @putchar(i32 10)   ; newline
  ret i32 0
}

$ mllvm run hello.ll
Hi
$ echo $?
0
```

## What it does

| Command | Meaning |
| --- | --- |
| `mllvm run FILE.ll` | parse, verify and execute the program |
| `mllvm run --opt FILE.ll` | constant fold first, then execute |
| `mllvm run --trace FILE.ll` | print every executed instruction to stderr |
| `mllvm check FILE.ll` | parse and verify without executing |
| `mllvm opt FILE.ll` | constant fold and print the resulting IR |
| `mllvm version` | print the version |

The exit status of `mllvm run` is the value returned by `@main` (or the value
passed to `@exit`), reduced to 8 bits the same way a shell does it.

## Supported IR

* **Values** – `i1`, `i8`, `i16`, `i32`, `i64`, `ptr` (and the older
  `i32*`-style pointer spelling).  Constants, `true`, `false`, `null`,
  `undef` and `poison`.
* **Integer instructions** – `add`, `sub`, `mul`, `udiv`, `sdiv`, `urem`,
  `srem`, `and`, `or`, `xor`, `shl`, `lshr`, `ashr`.
* **Comparisons** – `icmp` with all ten conditions (`eq`, `ne`, the `u*` and
  the `s*` family).
* **Selection and casts** – `select`, `zext`, `sext`, `trunc`, `bitcast`,
  `ptrtoint`, `inttoptr`.
* **Control flow** – `br`, conditional `br`, `ret`, `unreachable`, block
  labels and `phi` nodes.
* **Memory** – `alloca` (scalar and constant-size arrays), `load`, `store`,
  module-level `@global` / `@constant` scalars.
* **Calls** – direct calls to defined functions, recursion, and the builtins
  `@putchar`, `@print_i32`, `@print_i64`, `@print_u64`, `@exit`, `@abort`.
* **Optimisation** – constant folding of integer arithmetic, `icmp`,
  `select` and casts.  `mllvm opt` prints the folded module; the output
  parses again and behaves the same.

The exact grammar and the remaining limits are described in
[docs/ir-subset.md](docs/ir-subset.md).

## Build and test

Requirements: a C11 compiler and `make`.  There are no runtime dependencies.

```bash
make            # builds bin/mllvm
make test       # builds and runs the test suite
./build.sh      # the same without make
./build.sh test
sudo make install
```

The test suite runs every program twice (plain and with `--opt`), checks the
exit status and standard output, checks that malformed IR is rejected, and
round-trips the folded IR through the parser.

## Layout

| Path | Contents |
| --- | --- |
| `src/mllvm.c` | parser, verifier, interpreter, constant folder, IR printer |
| `tests/cases/*.ll` | programs with their expected exit status and output |
| `tests/cases/bad/*.ll` | IR that the verifier must reject |
| `tests/run.sh` | the test driver |
| `docs/ir-subset.md` | the grammar, semantics and limits |
| `Makefile`, `build.sh` | build entry points |

## Why a subset

LLVM is a compiler infrastructure with many years of engineering behind it.
A self-contained project can still own the interesting part — the IR — and be
complete, testable and useful if the scope is written down.  `mllvm` does
that: every construct listed above is implemented and tested, and anything
outside the list is rejected with a precise error instead of silently doing
the wrong thing.

## Release assets

The build produces a single host executable, `bin/mllvm`.  It is published in
the GitHub Releases of this repository together with the source archive and
the SHA-256 checksums in the release notes.

## License

Apache License 2.0, see [LICENSE](LICENSE).
