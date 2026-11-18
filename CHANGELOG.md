# Changelog

Every release of coff, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions are
plain [semantic versions](https://semver.org/) with no names.

## [0.8.0] - 2026-11-18

### Added

- `&=`, `|=`, `^=`, `<<=` and `>>=`, next to the `+=`, `-=`, `*=`, `/=` and
  `%=` that were already there. They work on locals, globals, `buf[i]` and
  `p.f`, and the lexer takes the longest match, so `x<<=2` is one operator.
- `ddc_entries.sh`, the one list of what the build compiles. The diverse
  double-compiling check and `run_tests.sh` both read it, and the audit
  fails when one of the build scripts of my kernel compiles something the
  list does not name. The check now compares every program, test and kernel
  variant the kernel build compiles, with the flags the build gives it, where
  it used to try files and skip the ones it could not build. A compile-error
  test is compared on its message and its exit code.
- The audit prints the line counts of `coff0.c`, `c0/coff.c0` and
  `c0/smed.c0`, so the figures `TRUST.md` quotes can be checked against it.
- Tests 133 to 150 cover a missing `;` after a call, an assignment, a field
  store, an index store and a `for` condition, the string escapes, `'\0'`, `&` against `==`, and
  every compound operator, including the order its parts run in, its use in
  `for` and `defer`, its lexing and a shift at the very end of the input.
  Each was seen failing, either by breaking the behaviour it pins or by
  running it against 0.7.1.

### Changed

- `buf[i] += v` and `p.f += v` work out the buffer and the index, or the
  record, once, and read the old value before the right side runs, so
  `b[f()] += 1` calls `f` once, where it called it twice. The address is
  taken before the right side runs as well. A right side that moves the
  block a global base points to therefore stores into the old block, which
  is what plain `=` has always done.
- A store whose value is a number or a variable keeps its address in `rdi`
  while the value is worked out. Every other store, and every call, still
  goes through the stack. coff's own assembly comes out a few instructions
  shorter.
- coff0 and coff1 refuse an argument that starts with `-` and is not `--elf`
  or `-I`, before they open anything. Before, `coff0 -x prog.c0` took `-x`
  for the input and `prog.c0` for the output, and compiled over its own
  source. `-I` without a directory and more than eight `-I` have their own
  messages.
- `run_tests.sh` reads both halves of its codegen comparison from
  `ddc_entries.sh`, and has a new stage that checks the command line of both
  compilers.
- `BOOTSTRAP.sha256` hashes `ddc_entries.sh` and the new tests, and no
  longer lists the lexer and the parser.
- `TRUST.md` gives the figures the audit prints with the date it printed
  them, and has two new standing rules. A call cannot write a local, and a
  store keeps its address in `rdi` only across a number or a variable.

### Fixed

- The large-source check in `run_tests.sh` was meant to build a file over
  1MB and built a smaller one. It now exceeds 1MB and asserts it.

### Removed

- `-t` and `-a`, the token and tree dumps of coff0.
- `c0/lex.c0` and `c0/parse.c0`, the standalone lexer and parser, and the
  two stages of `run_tests.sh` that compared them with coff0. coff1 is still
  compared with coff0 on every compile and every message, and the compiler
  still compiles itself to a fixpoint. What is gone is a comparison at the
  token and tree level, so coff1's front end is checked only end to end.
  `TRUST.md` says so.

## [0.7.1] - 2026-10-26

### Fixed

- `run_tests.sh` compared every `.c0` beside a Moonshot clone's programs
  under `--elf`, the libraries among them, which have no `main`, so four
  checks failed whenever Gnista's sources sat in `moonshot/programs`. A
  file without a `main` is now left to the program that includes it.

## [0.7.0] - 2026-10-26

### Added

- flyt, `lib/flyt.c0`, IEEE 754 doubles as a c0 library, since c0 has no
  floating-point type. A double is an int holding its 64 bits. Addition,
  subtraction, multiplication, division and square roots round to nearest
  with ties to even, with signed zeros, subnormals, infinities and NaN, and
  decimal text is read and written exactly rounded. `lib/flyt_check.sh`
  compares it with the machine's own doubles over about 49000 cases.
- `open_read`, `open_write`, `read`, `write` and `close` under `--elf`, as
  Moonshot's file system calls, so coff and smed themselves compile for
  Moonshot unchanged.
- smed assembles `cpuid`.
- Tests 126 to 132 cover the most negative number, the literals, the
  include paths, the defer rule, `_start` and `main` below. Each was checked
  by bringing the old behaviour back and watching the test fail, and test
  130, which holds what a defer may still use, by making the new check too
  strict.

### Changed

- smed writes a program as two segments, its code readable and runnable,
  and everything else, from the next page on, readable and writable but not
  runnable, where it was one segment that was all three.
- smed starts with a 1MB input buffer instead of 8MB. It still grows as
  needed, and now fits in a Moonshot program's heap.
- The busiest local variables of each function live in registers (rbx and
  r12 to r15) instead of on the stack. A use inside a loop counts for more
  than one outside it, so loop counters get a register first. Each function
  saves and restores the registers it uses.
- A local held in a register is used directly as an operand, compared
  directly in a condition, and updated in place by `x = x + y` and `+=` when
  `y` is a number or another register.
- `run_tests.sh` checks that every function saves and restores the
  registers it keeps locals in, with `check_saved_regs.awk`, where it used to
  check that no code used rbx at all.
- `BOOTSTRAP.sha256` also hashes flyt and `check_saved_regs.awk`.
- The checks that use my kernel find it in its new folder layout.
- A defer may no longer use a local declared in an inner block. The block
  has ended by the time the defer runs, and its slot may already hold
  another block's variable, so such a defer read whatever was there.
- A function, extern or embed named `_start` is refused. It used to get
  through and fail in the assembler, since every program has a `_start`.
- `main` declared `(int, int)` is refused, as `void main` already was.
- coff writes its output through a 64KB buffer instead of one system call
  per piece of text, so it compiles about twice as fast.

### Fixed

- Every string literal took a buffer as large as the whole source file while
  it was read. It now takes what the literal can need. On Linux the unused
  pages cost nothing, but a system that zeroes what it hands out paid for
  all of them.
- The most negative number, -2^63, was written into the assembly as a bare
  minus sign, so `1 << 63` as a global or a constant came out as 0. coff0
  wrote it correctly, so the two compilers disagreed on it. Every number
  printer in the toolchain had the same fault and all are fixed.
- A number literal too large for 64 bits is its low 64 bits in every
  implementation. coff0 clamped it to 2^64 - 1 where the others wrapped, so
  the two compilers disagreed on it.
- Include-once goes by the file, not by how its path is written, so
  `include "lib.c0";` and `include "./lib.c0";` no longer splice the same
  file twice.

## [0.6.0] - 2026-10-14

### Added

- `for (init; condition; step)` loops. `init` may declare a variable that
  belongs to the loop alone, and `continue` goes on to the step, as in C.
- `!`, which gives 1 for 0 and 0 for anything else.
- `+=`, `-=`, `*=`, `/=` and `%=`, each exactly `x = x OP (rhs)`.
- Functions that return two values, declared `(int, int) f(...)`, returning
  `return a, b;` and taken apart with `a, b = f(...);`. The two values travel
  in `rax` and `rdx`.
- `defer statement;`, which runs the statement at every return of the
  function, last-written first. It is decided at compile time, so a defer runs
  even if it sat under an `if` that was false, and even on a return that comes
  before it in the text. A `return`, `break`, `continue` or `defer` inside a
  deferred statement is a compile error.
- `load32` and `store32`, which read and write four bytes. A read is
  zero-extended.
- Tests 114 to 125 cover these, the include change and the code generation
  shortcuts below. Each was checked by breaking the feature on purpose and
  watching the test fail.

### Changed

- An included file is pasted in at most once, so two files can both include a
  third. A file that ends up including itself is still an error.
- Code generation takes a few shortcuts where the shape of the code makes them
  safe, in `coff0.c` and `c0/coff.c0` alike, so the two still agree byte for
  byte. A number or a variable on the right of an operator loads straight into
  a register, a comparison in a condition jumps on the flags, `&&` and `||` in
  a condition become chains of compares and jumps, small constants go into the
  instruction itself, commutative operators load the simple side last and
  calls load simple arguments straight into their registers. My OS kernel came
  out a fifth smaller.
- The compiler's own sources use the new statements and operators. Hand-written
  bit tests and remainders became `>>`, `&` and `%`.
- The audit's first stage compares the rebuilt chain with whatever compiler
  binaries are in the working tree, and skips that comparison in a clone where
  nothing has been built yet.
- `TRUST.md` gives the current line counts of `coff0.c` and `c0/coff.c0`,
  describes the toolchain as it is rather than how it got there, and lists
  only the work still open.

### Removed

- Test 77, which checked that a lone `!` was refused. `!` is an operator now.

## [0.5.0] - 2026-10-10

### Added

- Real error messages from `coff1`, with the file and the line, in exactly the
  words `coff0` uses. The test suite runs every program that is meant to fail
  through both compilers and fails if the messages differ, and some tests pin
  the exact message they must give.
- `%`, with the sign of the left operand as in C.
- The bitwise operators `&`, `|`, `^`, `~`, `<<` and `>>`, with C's
  precedence. `>>` keeps the sign.
- Constant expressions as global initializers, worked out at compile time.
- Layout names as types, as in `Point p = alloc(sizeof(Point));`, so two
  layouts may put the same field name at different offsets.
- `embed name "file";`, which puts a file's bytes in the program as data, with
  `name_size` as their length. The compiler never opens the file, the
  assembler does.
- `-I dir`, a directory to search for includes.
- Under `--elf`, the builtins `dealloc`, `sound`, `writefile`, `argc`,
  `argv` and `syscall(n, a, b, c, d, e)`.
- `c0/cmp.c0` and `c0/sha256.c0`. The audit builds them with the `coff1` it
  has just verified and runs every comparison and every hash through both them
  and the system tools.
- `fetch_ddc_cproc.sh`, which builds cproc and QBE at pinned commits as a third
  C compiler for diverse double-compiling.

### Changed

- The buffers `coff1` and `smed` read their input into grow as they fill, so no
  source file is cut short.

## [0.4.0] - 2026-09-22

### Added

- `smed -T linker.ld`, which lays the output out by a GNU ld linker script.
- Several input files in one `smed` call, assembled and linked in order.
- `smed -m symbols.map`, a list of every symbol and its address.
- `smed_tests.sh` builds my OS kernel with both smed and binutils when it is
  checked out alongside, and compares the two.

### Changed

- GNU `as` and `ld` are off every build path, kept only as the reference smed
  is checked against.
- `--elf` is a target of the ordinary assembly backend, mirrored in
  `coff0.c` and diffed like everything else. smed writes the ELF.
- The machine-code backends and `--raw` are gone. They were the one path no
  differential test could see.
- The audit checks smed against its own source.

## [0.3.1] - 2026-09-16

### Fixed

- `BOOTSTRAP.sha256` no longer hashes the kernel's build files when they sit
  next to this tree, so the committed manifest matches in both layouts the
  README describes. The kernel's build keeps its own record of the `coff1`
  hash instead.

## [0.3.0] - 2026-09-13

### Added

- smed, an assembler and linker written in c0, which replaces GNU `as` and
  `ld` for every c0 program.
- `verify_bootstrap.sh`, the trusting-trust audit. It rebuilds the chain,
  checks the fixpoint, checks that the output is deterministic and runs
  diverse double-compiling against every other C compiler it finds.
- `BOOTSTRAP.sha256`, hashes of every source, script and test, so tampering
  shows up as a text diff.
- `TRUST.md`, which says what is trusted, what is checked and what none of it
  proves.

### Changed

- gcc is no longer required. tcc builds a byte-identical `coff1` and can
  itself be bootstrapped from a seed small enough to check by hand.

## [0.2.0] - 2026-08-08

### Added

- `layout` and `sizeof`, which name field offsets in a block of memory.
- `extern int name;` for a global another object file defines.
- `--raw`, which writes a flat binary, and `--elf`, which writes an ELF64
  executable for my OS kernel, with the builtins that are system calls there.

### Fixed

- `exit()` under `--elf`.
- Parameters five and six.
- The top half of 64-bit literals.
- An overflowing label table.

## [0.1.0] - 2026-07-21

### Added

- The first public release of coff and c0. c0 is a small systems language with
  one type, the 64-bit integer. coff compiles it to freestanding x86-64
  assembly and is written in c0 itself. `coff0.c` is the one-time bootstrap,
  kept as the reference the tests compare everything against.

[0.7.1]: https://github.com/cofflang/coff/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/cofflang/coff/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/cofflang/coff/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/cofflang/coff/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/cofflang/coff/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/cofflang/coff/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/cofflang/coff/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/cofflang/coff/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/cofflang/coff/releases/tag/v0.1.0
