# coff

coff is a self-hosted compiler for **c0**, a small systems language I am
building from scratch. c0 looks like a stripped-down C. It has one type,
the 64-bit integer, and everything -- numbers, addresses, strings, function
references -- is a value of that type. coff compiles c0 straight to x86_64
assembly, and the binaries it produces are freestanding: no libc, no
runtime, just `_start` and raw syscalls.

It can also target Moonshot, the operating system I am writing in c0,
which is what every program running in ring 3 on that kernel is built
through. Same assembly backend, that kernel's syscall numbers.

The compiler is written in c0 and compiles itself, and the repository
carries its own assembler and linker, smed, also written in c0. Once
bootstrapped, you need nothing except this repository: a C compiler and
binutils are needed once, to get the first `coff1` and the first `smed`
built, and after that neither is on the path from c0 source to a running
program.

## What is in the repository

| Path | What it is |
|------|------------|
| `coff0.c` | The bootstrap compiler, in plain C. It compiles `c0/coff.c0` the first time, and the test suite diffs everything against it. |
| `c0/coff.c0` | The real compiler, written in c0. The binary built from it is called `coff1`. |
| `c0/lex.c0` | A standalone c0 lexer in c0, kept as a differential test stage. |
| `c0/parse.c0` | A standalone c0 parser in c0, same purpose. |
| `c0/smed.c0` | The assembler and linker, in c0. Turns the assembly `coff1` emits into a finished executable, with no GNU tool in between. |
| `c0/sha256.c0`, `c0/cmp.c0` | `sha256sum` and `cmp` written in c0. The audit builds them with the `coff1` it has just verified and runs them next to the system tools, so every hash and every byte comparison is checked by two inspectors. |
| `tests/` | The test programs, which double as the input set for the differential checks. |
| `run_tests.sh` | Builds coff0 and runs the compiler test suite. |
| `smed_tests.sh` | Checks smed against GNU as and ld over every program in the tree, then runs the suite with no GNU tool in the pipeline. The last check builds my kernel with both and compares them, when it is checked out alongside. |
| `verify_bootstrap.sh` | The trusting-trust audit: rebuild, fixpoint, determinism, and diverse double-compiling against every other C compiler it finds. |
| `fetch_ddc_cproc.sh` | Fetches and builds cproc and QBE at pinned commits, as a third C compiler for the audit. |
| `TRUST.md` | What is trusted, what is checked, and what is not. Read it before changing how anything here is built. |
| `BOOTSTRAP.sha256` | Hashes of every source, script and test file, so tampering shows up as a text diff. |

## Building

You need Linux on x86_64, a C compiler and `as` and `ld` from binutils
(all three for the first bootstrap only), and bash for the scripts.

```sh
# 1. build the bootstrap compiler
tcc -o coff0 coff0.c            # or: gcc -Wall -Wextra -o coff0 coff0.c

# 2. use it to build the real, self-hosted compiler
./coff0 c0/coff.c0 coff1.s
as coff1.s -o coff1.o
ld coff1.o -o coff1

# 3. use that to build the assembler and linker
./coff1 c0/smed.c0 smed.s
as smed.s -o smed.o
ld smed.o -o smed

# 4. have smed rebuild itself, so the binary you keep is its own output
./smed smed.s smed.new
mv smed.new smed
chmod +x smed
```

Step 4 is not decoration. The `smed` from step 3 was made by `as` and `ld`,
which is the one thing this repository exists to stop depending on, and it
is a different binary from the one smed makes of the same assembly, because
smed does not pick the short encodings binutils picks. Running it once more
through itself reaches a fixpoint immediately, and that is the binary the
audit expects to find. Skip step 4 and `./verify_bootstrap.sh` will tell
you the committed smed is not what smed produces from its own source, which
would be correct.

(Note the temporary name. Linux refuses to write to a running executable,
so `./smed smed.s smed` fails with "cannot open output" rather than
corrupting anything.)

After step 4 you do not need the C compiler, `as` or `ld` anymore. `coff1`
compiles c0, including its own source, and `smed` assembles and links what
it emits, including its own source. The test scripts still use gcc or tcc
to rebuild `coff0`, and `as`/`ld` as an oracle to check `smed` against.

tcc is shown first on purpose. Either compiler produces a byte-identical
`coff1`, but tcc can be bootstrapped from a hand-auditable seed and gcc
cannot, which matters for the reasons in [TRUST.md](TRUST.md).

## Using the compiler

Write a program:

```c
// hello.c0
int main() {
    print("hello from c0\n");
    return 0;
}
```

Compile, assemble and link, run:

```sh
./coff1 hello.c0 hello.s
./smed hello.s hello
chmod +x hello
./hello
```

`smed` writes a plain file, so the executable bit is yours to set. `as` and
`ld` produce an equivalent binary from the same `hello.s` if you prefer
them.

The full form is:

```
smed [-T linker.ld] [--base ADDR] [-m symbols.map] input.s [more.s ...] output
```

Several inputs are assembled and linked together in the order given, which
is how a kernel made of hand-written boot assembly plus compiler output
gets built in one call. `-T` reads a GNU ld linker script, so an existing
script keeps working unchanged; `--base` sets the load address when no
script says otherwise; `-m` writes a symbol map, which is a plain list of
addresses and names, useful for checking a layout against another linker's.

The process exit code is whatever `main` returns. The compiler's full form
is

```
coff1 [--elf] [-I dir]... input.c0 output.s
```

`-I` adds a directory to look in for an include that is not next to the
file including it, which is how programs in different directories share a
library. With no file arguments, `coff1` reads c0 source on stdin and
writes assembly to stdout instead; the test harness uses that form.

A program with a mistake in it gets a real message, with the file and the
line, such as `coff1: hello.c0:3: expected ';'`. coff1 and coff0 say exactly
the same thing for every input, and the test suite holds them to it.

## The language

c0 has `int` variables and globals, functions with up to 16 parameters
(the first six in registers, the rest on the stack), recursion, `if`/`else`,
`while` with `break`/`continue`, block scoping with shadowing,
short-circuiting `&&`/`||`, `%`, the bitwise operators `& | ^ ~ << >>`
(with C's precedence, warts included, and `>>` arithmetic), string
literals, char literals, hex literals,
`buf[i]` indexing (sugar over byte load/store), and function references: a
bare function name is its address, and calling through a variable holding
one is an indirect call. `include "file.c0";` splits a program across
files by plain textual substitution. `extern int name;` declares a global
that some other object file defines. A global's initializer may be any
constant expression, folded at compile time. `embed name "file";` puts a
file's bytes into the program as data, with `name` as their address and
`name_size` as their length; the compiler never opens the file, the
assembler does.

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
over `load64`/`store64`. A layout name can also be used as a type, as in
`Entity e = alloc(sizeof(Entity));`. The variable is still an int, but its
fields are looked up in that layout, so two layouts may put the same field
name at different offsets. An untyped variable reaches a field through the
one offset every layout agrees on, and is refused when they do not agree.

There is no type checking, no optimizer, and on Linux nothing is ever
freed. Some of that will change, some of it is the design. The full
grammar is in the header comment of `coff0.c`.

I/O and memory are compiler builtins that map directly to syscalls:
`print`, `write`, `read`, `open_read`, `open_write`, `close`, `alloc`,
`load8`/`store8`, `load64`/`store64`, `exit`, `argc`/`argv`, and
`outb`/`inb`/`outw`/`inw` for port I/O in freestanding code. They are all listed with
their signatures in `coff0.c`'s header comment.

## Output modes

coff emits x86_64 assembly text, always. One flag changes who that
assembly is for:

| Flag | What it emits |
|------|---------------|
| (none) | Assembly for Linux. Linux syscall numbers. |
| `--elf` | Assembly for Moonshot, my OS kernel. Its own syscall numbers, and the OS builtins below. |

Either way the output is assembly text that `smed` turns into a finished
binary. For the Moonshot target, `smed --base 0x80000000` places the image
where that kernel maps user programs.

There is deliberately no second backend. `--elf` is a handful of emit
sites that differ in a syscall number, `coff0.c` has the same flag, and the
two are diffed against each other like everything else.

`--elf` adds builtins that are syscalls on that kernel and exist in no
other mode. `win_info` and `win_max` describe the window a program was
given, `present` and `blit` and `fill` draw, `key_poll` and `mouse_poll`
read input, `glyph` hands over the kernel's font, `ticks` reads the timer,
`readfile` and `writefile` load and save files, `sound` queues audio,
`dealloc` gives back what `alloc` handed out, and `syscall(n, a, b, c, d,
e)` reaches any system call by number, so a new one needs no compiler
change. `argc` and `argv` work there too, as syscalls. Calling an `--elf`
builtin outside `--elf` is a compile error, and calling a Linux-only
builtin such as `open_read` or `outb` under `--elf` is too.

The distinction `win_info` versus `win_max` is worth knowing if you write
against this: `win_info` is the window's size right now and changes
whenever the window manager reflows, while `win_max` is the largest window
the machine can ever hand out. A program sizes its frame buffer once from
`win_max`, so it never has to reallocate it, and treats `win_info` as the
region it is currently allowed to draw into.

## Testing

```sh
./run_tests.sh
./smed_tests.sh     # needs the coff1 from Building in the tree
                    # check 5 also needs my kernel at ../moonshot, and skips without it
```

Every test program goes through the real pipeline: compile, assemble,
link, execute, check the exit code (and the exact stdout, where a test has
an `.expected_stdout` file, and the exact error message, file and line,
where a test has an `.expected_error` file). Then each c0-written stage is
diffed byte for byte against coff0 over the whole corpus, error messages
included, the `-I`, `embed` and large-source cases are run through both
compilers, the c0 `sha256` and `cmp` are held to the system tools, and last
the bootstrap fixpoint is checked: `coff.c0` compiling itself three
generations deep, all byte-identical. That is over 400 checks, and they all
have to stay green.
`CC=tcc ./run_tests.sh` runs the same suite with gcc absent from the
machine entirely.

`smed_tests.sh` does the same for the assembler: every program in the tree
is assembled once by `as` and `ld` and once by `smed`, and the two binaries
must agree on exit code and output. Then `smed` rebuilds the compiler, which
must reproduce its own assembly; `smed` rebuilds itself, and the two must
emit identical bytes; and the whole suite runs again with no GNU tool in
the pipeline. The oracle is the thing being replaced, which is the same
trick `coff0` plays for `coff1`.

There is a fifth check that only runs when my kernel is checked out at
`../moonshot`, and reports SKIP otherwise. It exists because the boot code
and the ring-3 trampolines are hand-written assembly full of instructions
no c0 program ever emits, so the corpus above says nothing about `lgdt`,
`iretq`, control registers or the far jump into long mode. The kernel is
built twice from the very same sources, once by `as` and `ld` and once by
`smed`, and the two are compared: the entry point, the multiboot header and
every linker-script symbol must land at the same addresses, and the
hand-written code must disassemble to the same instruction sequence. Bytes
are not compared, because smed never picks a short encoding where a long
one works, but small immediates are, since an earlier version that
normalised those away let a wrong segment selector through.

Those byte-for-byte checks are how I trust the compiler at all. `--elf` is
the same code path as the default one and `coff0.c` implements it too, so
it is diffed byte for byte like everything else. What the suite cannot tell
is whether a program behaves correctly *on the kernel*, only that both
compilers agree about it. That part is covered
outside this repository, by small programs whose exit code is only correct
if codegen is correct, run on the kernel itself.

## Trust

A green test suite is not evidence that the toolchain is free of a
Thompson-style payload. The bootstrap fixpoint is exactly the property such
a payload is built to preserve, and I have checked that claim rather than
repeated it: a simulated compromised gcc, backdooring the kernel's password
check through `coff0`, passed every test above with no trace in any source
file. `./verify_bootstrap.sh` is the audit that bears on that question. It
rebuilds the whole chain from source, checks the fixpoint, checks that the
compiler's output does not depend on locale, time zone, build directory or
date, and then does diverse double-compiling: `coff0.c` built by
unrelated C compilers must produce byte-identical assembly over every c0
source it can find. That last stage is the one that caught the simulated
attack, and it only did so once the corpus included the code worth
attacking. It needs a second C compiler installed, tcc by preference, and
uses cproc as a third when `./fetch_ddc_cproc.sh` has built it. Every hash
and byte comparison the audit makes is checked twice, by the system tools
and by the c0 `sha256` and `cmp` built by the `coff1` it has just verified.

[TRUST.md](TRUST.md) is the full account: what is trusted, what is checked
by what, what is still open, and what none of it proves.

## Contributing

Contributions are welcome. Some things to know before opening a pull
request:

- The language is implemented four times over (`coff0.c`, `c0/lex.c0`,
  `c0/parse.c0`, `c0/coff.c0`). A language change lands in `coff0.c`
  first, then gets mirrored into the c0 implementations, each verified
  byte-identical against coff0 before moving on. A new error message has to
  read the same in coff0 and coff1.
- `./run_tests.sh` must pass, including both fixpoint checks, and so must
  `./smed_tests.sh` and `./verify_bootstrap.sh`.
- New behavior needs a test in `tests/`. Bugs found the hard way get a
  regression test. A new test file is also a new input to every
  differential check and to the manifest, so it is reviewed like code.
- Keep it minimal. No new dependencies. Anything that touches how the
  compiler or the assembler is built goes through `TRUST.md` first, and
  `coff0.c` has to keep building under tcc.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Copyright (C) 2026 tavro
