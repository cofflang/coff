# coff

coff is a self-hosted compiler for **c0**, a small systems language I am
building from scratch. c0 looks like a stripped-down C. It has one type,
the 64-bit integer, and everything -- numbers, addresses, strings, function
references -- is a value of that type. coff compiles c0 straight to x86_64
assembly, and the binaries it produces are freestanding: no libc, no
runtime, just `_start` and raw syscalls.

The compiler is written in c0 and compiles itself. Once bootstrapped, you
need nothing except this repository and the system assembler and linker
(gcc is only needed once, for the bootstrap).

## What is in the repository

| Path | What it is |
|------|------------|
| `coff0.c` | The bootstrap compiler, in plain C. It compiles `c0/coff.c0` the first time, and the test suite diffs everything against it. |
| `c0/coff.c0` | The real compiler, written in c0. The binary built from it is called `coff1`. |
| `c0/lex.c0` | A standalone c0 lexer in c0, kept as a differential test stage. |
| `c0/parse.c0` | A standalone c0 parser in c0, same purpose. |
| `tests/` | The test programs, which double as the input set for the differential checks. |
| `run_tests.sh` | Builds coff0 and runs everything. |

## Building

You need Linux on x86_64, `gcc` (bootstrap only), `as` and `ld` from
binutils, and bash for the test script.

```sh
# 1. build the bootstrap compiler
gcc -Wall -Wextra -o coff0 coff0.c

# 2. use it to build the real, self-hosted compiler
./coff0 c0/coff.c0 coff1.s
as coff1.s -o coff1.o
ld coff1.o -o coff1
```

After step 2 you do not need gcc anymore. `coff1` compiles c0, including
its own source.

## Using the compiler

Write a program:

```c
// hello.c0
int main() {
    print("hello from c0\n");
    return 0;
}
```

Compile, assemble, link, run:

```sh
./coff1 hello.c0 hello.s
as hello.s -o hello.o
ld hello.o -o hello
./hello
```

The process exit code is whatever `main` returns. With no arguments,
`coff1` reads c0 source on stdin and writes assembly to stdout instead;
the test harness uses that form.

## The language

c0 has `int` variables and globals, functions with up to 16 parameters
(the first six in registers, the rest on the stack), recursion, `if`/`else`,
`while` with `break`/`continue`, block scoping with shadowing,
short-circuiting `&&`/`||`, string literals, char literals, hex literals,
`buf[i]` indexing (sugar over byte load/store), and function references: a
bare function name is its address, and calling through a variable holding
one is an indirect call. `include "file.c0";` splits a program across
files by plain textual substitution.

There is no type checking, no `%` operator, no optimizer, and nothing is
ever freed. Some of that will change, some of it is the design. The full
grammar is in the header comment of `coff0.c`.

I/O and memory are compiler builtins that map directly to syscalls:
`print`, `write`, `read`, `open_read`, `open_write`, `close`, `alloc`,
`load8`/`store8`, `load64`/`store64`, `exit`, `argc`/`argv`, and
`outb`/`inb` for port I/O in freestanding code. They are all listed with
their signatures in `coff0.c`'s header comment.

## Testing

```sh
./run_tests.sh
```

Every test program goes through the real pipeline: compile, assemble,
link, execute, check the exit code (and the exact stdout, where a test has
an `.expected_stdout` file). Then each c0-written stage is diffed byte for
byte against coff0 over the whole corpus, and last the bootstrap fixpoint
is checked: `coff.c0` compiling itself three generations deep, all
byte-identical.

Those byte-for-byte checks are how I trust the compiler at all, so the
suite has to stay green.

## Contributing

Contributions are welcome. Some things to know before opening a pull
request:

- The language is implemented four times over (`coff0.c`, `c0/lex.c0`,
  `c0/parse.c0`, `c0/coff.c0`). A language change lands in `coff0.c`
  first, then gets mirrored into the c0 implementations, each verified
  byte-identical against coff0 before moving on.
- `./run_tests.sh` must pass, including both fixpoint checks.
- New behavior needs a test in `tests/`. Bugs found the hard way get a
  regression test.
- Keep it minimal. No new dependencies.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Copyright (C) 2026 tavro
