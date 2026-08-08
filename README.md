# coff

coff is a self-hosted compiler for **c0**, a small systems language I am
building from scratch. c0 looks like a stripped-down C. It has one type,
the 64-bit integer, and everything -- numbers, addresses, strings, function
references -- is a value of that type. coff compiles c0 straight to x86_64
assembly, and the binaries it produces are freestanding: no libc, no
runtime, just `_start` and raw syscalls.

It can also skip the assembler entirely and emit machine code directly,
either as a flat binary or as an ELF64 executable for Moonshot, the
operating system I am writing in c0. That backend is what every program
running in ring 3 on that kernel is built through.

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
files by plain textual substitution. `extern int name;` declares a global
that some other object file defines.

`layout` gives a block of memory named offsets:

```c
layout Entity {
    int x;
    int y;
}

int e;
e = alloc(sizeof(Entity));
e.x = 10;
```

A layout allocates nothing by itself. It only maps each field name to an
offset, so `sizeof(Entity)` is a compile-time constant and `e.x` is sugar
over `load64`/`store64`. Because there is no type system, a field name has
one offset across the whole program: two layouts may share a name only if
they agree on its position.

There is no type checking, no `%` operator, no optimizer, and nothing is
ever freed. Some of that will change, some of it is the design. The full
grammar is in the header comment of `coff0.c`.

I/O and memory are compiler builtins that map directly to syscalls:
`print`, `write`, `read`, `open_read`, `open_write`, `close`, `alloc`,
`load8`/`store8`, `load64`/`store64`, `exit`, `argc`/`argv`, and
`outb`/`inb` for port I/O in freestanding code. They are all listed with
their signatures in `coff0.c`'s header comment.

## Output modes

By default coff emits x86_64 assembly for the system assembler. Two flags
change that:

| Flag | What it emits |
|------|---------------|
| (none) | x86_64 assembly text, to be run through `as` and `ld`. |
| `--raw` | A flat binary of machine code, no assembler involved. Linux syscall numbers. |
| `--elf` | An ELF64 executable for Moonshot, my OS kernel. Its own syscall numbers, and the OS builtins below. |

`--elf` adds builtins that are syscalls on that kernel and exist in no
other mode: `win_info` and `win_max` for the window a program was given,
`present` and `blit` and `fill` for drawing, `key_poll` for input, and
`readfile`. Calling one outside `--elf` is a compile error.

The distinction `win_info` versus `win_max` is worth knowing if you write
against this: `win_info` is the window's size right now and changes
whenever the window manager reflows, while `win_max` is the largest window
the machine can ever hand out. Since `alloc` is bump-only and cannot grow
a buffer, a program sizes its frame buffer once from `win_max` and treats
`win_info` as the region it is currently allowed to draw into.

## Testing

```sh
./run_tests.sh
```

Every test program goes through the real pipeline: compile, assemble,
link, execute, check the exit code (and the exact stdout, where a test has
an `.expected_stdout` file). Then each c0-written stage is diffed byte for
byte against coff0 over the whole corpus, and last the bootstrap fixpoint
is checked: `coff.c0` compiling itself three generations deep, all
byte-identical. That is 267 checks, and they all have to stay green.

Those byte-for-byte checks are how I trust the compiler at all. They do
have one real limit worth stating plainly: `coff0.c` implements only the
text backend, so nothing in this suite says anything about `--raw` or
`--elf`. Every check of the machine-code backend is a differential check
against a compiler that does not have one. I have found five
silent-wrong-code bugs in that backend so far, every one of them by eye
after something looked wrong on screen, and the suite was green through
all of them. The backend is instead covered outside this repository, by
compiling small programs whose exit code is only correct if codegen is
correct and running them on the kernel itself.

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
