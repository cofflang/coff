# Changelog

Every release of coff, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions are
plain [semantic versions](https://semver.org/) with no names.

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

[0.6.0]: https://github.com/cofflang/coff/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/cofflang/coff/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/cofflang/coff/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/cofflang/coff/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/cofflang/coff/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/cofflang/coff/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/cofflang/coff/releases/tag/v0.1.0
