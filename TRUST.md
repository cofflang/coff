# Trust

This file is the trusting-trust perspective for c0, coff and the operating
system I am writing in c0, Moonshot. It is the first thing to read before
changing how any of the three are built, and it takes priority over
convenience, build speed and elegance.

It is written from the point of view of my own working tree, where the
compiler and the kernel sit next to each other. No binary is committed in
either place. The compilers are built from source into the tree, and the checks
ship here and report SKIP for anything that is not present.

## The threat

Ken Thompson, *Reflections on Trusting Trust*, ACM Turing Award lecture, 1984.
He modified a C compiler so that it recognised two things: the `login`
program, into which it inserted a backdoor, and its own source, into which it
inserted the recogniser. He compiled the compiler with itself and deleted the
modified source. From then on the backdoor lived only in the binary,
reproduced itself into every future compiler built from clean source, and left
no evidence in any source file anyone could read.

Three consequences shape everything below.

**It is not a compiler problem.** Thompson: *"I could have picked on any
program-handling program such as an assembler, a loader, or even hardware
microcode. As the level of program gets lower, these bugs will be harder and
harder to detect."*

**It defeats inspection, because it defeats the inspectors.** A payload
propagates into the disassembler, the debugger and the editor used to hunt for
it.

**Its one weakness is recognition.** Identifying every target correctly is
undecidable, so a payload must misfire somewhere. Every real defence is a way
of forcing that mistake into the open.

## This is not hypothetical here

A working Thompson attack was built against this exact toolchain, as a test
of whether the defences detect anything. A shim standing
in for a compromised gcc silently altered the `coff0` it emitted, leaving
`coff0.c` byte-identical to git. Results:

- The kernel's password check was made to always succeed. The payload
  existed in no source file.
- **The whole test suite passed. Both bootstrap fixpoints passed. The
  in-tree binary checks passed.** A green suite on a compromised machine.
- A stage1-only DDC check **also passed**, because the payload fired on a
  program the compiler never compiles while checking itself.
- Corpus-wide DDC, with the kernel sources in the corpus, **caught it** — the
  diff showed the password check reduced to `mov rax, 1 / ret`.

That last result is why the DDC stage compares every compile the build
performs, as `ddc_entries.sh` lists them, rather than one.
The check that only looked at the compiler compiling itself was worthless
against a payload aimed at the kernel.

## What is trusted, and what is done about it

The trusted computing base for a kernel image. "Checked" means something
automated fails the build when it is wrong.

| Component | Role | Status |
|---|---|---|
| `coff1` binary | compiles the kernel | **checked** — byte-verified against a fresh bootstrap; hash-gated on every kernel build |
| `c0/coff.c0` | source of `coff1` | readable, 4013 lines as printed by the audit on 2026-11-18, in git, hashed in the manifest |
| `coff0` binary | bootstraps `coff1` once | **checked** — byte-verified against a fresh build of `coff0.c` |
| `coff0.c` | the seed compiler | readable, 3346 lines of plain C as printed by the audit on 2026-11-18, hashed. Buildable by gcc, **tcc and cproc** |
| gcc | *optional* | **not required.** See "gcc is out of the trusted base" |
| **tcc 0.9.28** | builds `coff0` | **partially mitigated** — DDC-cross-checked against gcc **and cproc** over every compile in the list `ddc_entries.sh` prints, which is the coff tests and sources and everything the Moonshot build scripts compile, 209 entries (153 coff, 72 of them compile-error tests compared on message and exit code, and 56 Moonshot) as printed by the audit on 2026-11-18. Not yet from a bootstrapped seed |
| **cproc + QBE** (pinned commits, see `fetch_ddc_cproc.sh`) | third DDC witness | built by tcc, not gcc; independent authors (Michael Forney, Quentin Carbonneaux). Diversifies the compilation proper only. It uses the system `cpp` (gcc's preprocessor), GNU as/ld and glibc, so those are not cross-checked by this leg |
| `smed` binary | assembles and links `coff1`, every c0 program, **and the kernel** | **checked** — `smed.s` byte-verified as `coff1`'s output for `c0/smed.c0`, the binary byte-verified as smed's own output for `smed.s`; hash-gated on every kernel build like `coff1`; differential-tested against as+ld over the whole corpus and over the kernel's hand-written assembly and layout |
| `c0/smed.c0` | source of `smed` | readable, 2255 lines as printed by the audit on 2026-11-18, in git, hashed in the manifest |
| GNU as 2.47.0 | *off every build path.* Produces the very first `smed` once, the way a C compiler produces `coff0` once | **removed from the per-build base.** Kept installed as smed's oracle in `smed_tests.sh`; Thompson's own example of a better hiding place than a compiler |
| GNU ld 2.47.0 | *off every build path.* Same one-time role | **removed from the per-build base.** Same |
| glibc | linked into `coff0` only | `coff1` is statically linked and uses raw syscalls, so glibc is in the seed path only, never per-build |
| the kernel's generators | pack compiler output into files the shipped kernel embeds | **checked** — written in c0, so they are entries of the DDC corpus. That covers bake, which packs the ELF test programs, and the two tools that build the site disk image, `site_copy.c0` and `site_images.c0`, each compiled the way the Moonshot build compiles it. They live in the kernel's repository, so this repository's manifest does not hash them. python3 is off the build path entirely and appears only in the QEMU test harness |
| kernel test programs, `tests/` | test data that becomes kernel content | **hashed** — see "the xz lesson" |
| Build scripts | decide what gets compiled | **hashed** — build code is code. The scripts in this repository are in the manifest, `ddc_entries.sh` among them, since it decides what the DDC compares. The kernel's own build scripts live in its repository, and `ddc_entries.sh` fails the audit when one of them compiles something it does not model |
| `--elf` target | builds every ring-3 program baked into the kernel | **checked** — the same assembly backend as the kernel with Moonshot's syscall numbers, present in `coff0.c` too, so `coff0 --elf` and `coff1 --elf` are diffed on every test run over every compile `ddc_entries.sh` lists, which is every ring-3 program the kernel build compiles with the directory and `-I` flags it gives them, every ELF test, and the kernel in both of its debug variants. The corpus DDC compares the same list. Every `embed` in the kernel must map to a listed program, or the audit fails. smed makes the ELF |
| the audit's `cmp` and `sha256sum` | every byte-identity verdict and every manifest hash | **cross-checked**. `c0/cmp.c0` and `c0/sha256.c0` are built by the freshly verified `coff1`, and from that point every comparison and every manifest hash runs through both them and the system tools. Any disagreement fails the audit and blocks `--write-manifest`. A lie needs both inspectors to lie identically, and they share no code or compiler. **Limit.** The checks that verify `coff1` itself run before the c0 tools exist, so those use the system `cmp` alone. The c0 tools are also linked by GNU as/ld inside the audit, since smed is not yet verified at that point |
| **the audit's other tools** | `diff`, `readelf`, `objdump`, `strings`, `grep`, `as`, `ld`, bash (and python3 in the kernel's QEMU test harness) | **UNMITIGATED.** The inspectors are built by the same toolchain they inspect |
| the ISO builder | produces the bootable image | in the boot path, not in the kernel image |
| Linux, microcode, silicon | everything | out of scope, honestly so |

## gcc is out of the trusted base

`coff0` built by **tcc alone**, with gcc absent from the entire chain,
bootstraps a `coff1` that is **byte-identical** to the one a gcc-built `coff0`
bootstraps, and that `coff1` emits **byte-identical** assembly for the kernel. The
full suite passes gcc-free (`CC=tcc ./run_tests.sh`).

This matters more than it first looks. gcc is reachable from nothing — there
is no way to obtain a trustworthy gcc except from another gcc. tcc *is*
reachable: the Bootstrappable Builds work has a complete chain from a
**357-byte `hex0` seed**, small enough to verify by hand against its own
documented source, up through `hex1`/`hex2`/`M0`/`M1`/`M2-Planet`, GNU Mes and
MesCC, to a self-hosting tcc. Guix has shipped a full-source bootstrap on this
basis since 2022.

So the kernel has a **credible path to a full-source bootstrap**:

```
hex0 (357 bytes, hand-auditable)
  -> hex1 -> hex2 -> M0 -> M1 -> M2-Planet
  -> GNU Mes / MesCC
  -> tcc
  -> coff0        (3346 lines of C, hand-auditable)
  -> coff1        (self-hosted c0)
  -> the kernel
```

Every arrow above except the last three is already solved by other people and
is reproducible today. The remaining work is mine and it is small, because
`coff0.c` is small. **This is the single most valuable structural property the
project has, and it exists only because `coff0.c` stayed plain and portable.**

Note the honest gap: the tcc in use here was built by gcc, so it is not yet
independent of gcc. It raises the bar a long way — gcc would have to recognise
tcc's source, inject a payload surviving tcc's self-hosting, and have that
payload recognise `coff0.c` — but it is not the same as a tcc from the hex0
seed. Closing that is item 1 under "Open work" below.

## What is checked, and by what

**`./verify_bootstrap.sh`** — the full audit, four stages:

1. The in-tree binaries byte-match a fresh rebuild from source (SKIP on a
   fresh clone where nothing has been built yet; the rebuild itself still has
   to succeed).
2. Bootstrap fixpoint (stage1 == stage2 == stage3).
3. Determinism and environment independence — output must be identical under
   a hostile locale (`tr_TR`, where `toupper('i')` is not `'I'`; the classic
   locale that breaks lexers), a hostile TZ, and a forced
   `SOURCE_DATE_EPOCH`; `coff0` must not vary with the build directory;
   `coff1` must embed no host path, no build date and no GNU build-id.
   Reproducibility comes first because DDC cannot distinguish an attack from
   noise without it.
4. Diverse double-compiling over every compile in the list `ddc_entries.sh`
   prints. That is the coff tests and the c0 sources, and, when the kernel
   tree sits next to this one, every compile the kernel build performs, read
   from its build scripts. On 2026-11-18 with the Moonshot release tree beside it
   the audit printed 209 entries, 153 coff (72 of them compile-error tests,
   compared on message and exit code) and 56 Moonshot. A listed file the
   reference coff0 rejects, or one that leaves no result, fails the audit,
   and without the kernel tree the audit says so and compares only the coff
   entries. The audit prints both counts and the line counts of `coff0.c`,
   `c0/coff.c0` and `c0/smed.c0`. Manifest validation comes with it. It runs against every diverse
   compiler it finds, not just the first. cproc with QBE, fetched and built
   with tcc at pinned commits by `fetch_ddc_cproc.sh` into
   `~/.local/opt/c0-ddc`, is the third, and tcc-built, gcc-built and
   cproc-built `coff0` agree byte for byte over the whole corpus. One extra
   instruction planted in the `coff0.c` given to cproc makes DDC report a
   mismatch on every file it touches.

**`./run_tests.sh`** — 373 tests as printed by run_tests.sh on 2026-11-18, the unit
tests, the comparison of coff1 with coff0 and the fixpoint checks, including
the in-tree binary checks. coff.c0's front end is checked only end to end.
codegendiff compares coff1's assembly with coff0's over every compile in the
corpus, and diagdiff compares their messages on the 72 compile-error tests.
There is no token-level or tree-level oracle, so a lexer or parser difference
is caught only where it changes an output or a message on a file in the
corpus. Of eleven mutations of coff.c0's front end measured on 2026-11-17,
ten were each caught by between 1 and 46 corpus files, eight of them by at
most two, so coverage of those rules rests on one or two tests. The
eleventh, letting coff1 accept a `for` with no `;` after its condition, was
caught by none until test 150 was added, which now catches it in both
compilers. Both compilers refuse any argument that starts with '-' other than
--elf and -I dir, and an -I without a directory or past 8, before opening any
file. `CC=tcc` runs the whole thing without gcc.

**The kernel build** — hash-gates `coff1` **and `smed`** on every build and
refuses to produce a kernel on mismatch. No binary is committed and the
manifest records none, so the kernel's build script keeps its own record of both hashes, written right after a passing audit and nowhere
else. A binary with no recorded hash at all is unverified, not tampered: it
gets the full audit before building anything. Re-bootstraps and re-runs the
full audit when `coff.c0` or `smed.c0` moves; a rebuilt smed is assembled by
the smed already present, and by as+ld only when there is none, which is the
one-time seed. Prefers tcc as the seed compiler when present. Fails closed.
The kernel image itself is one smed call, reading the same linker script GNU
ld reads, so the oracle below consumes the same inputs.

**`./smed_tests.sh`** — five checks on the assembler: a differential corpus
against as+ld over every c0 source, the compiler rebuilding itself through
smed, the smed self-hosting fixpoint, the whole test suite run with no GNU
tool anywhere in the pipeline, and the kernel built twice from identical
inputs -- by as+ld and by smed -- with the layout compared where the linker
script fixes it (entry, multiboot header, reservation sizes, alignments) and
the hand-written assembly compared as disassembled instruction sequences
(encodings differ on purpose, small immediates must not). Every half of the
kernel check catches a deliberate mutation, and six deliberate encoding
mutations are each caught by the kernel's own suite as well, three of them
before the first serial byte. The check runs only when that tree sits next
to this one, and reports SKIP otherwise.

**`BOOTSTRAP.sha256`** — hashes of every source, every script and the whole
test corpus, plus the versions of every tool trusted to build from them. It
records no binary, since none is committed, and nothing outside this
repository, so the same committed file matches whether or not the kernel sits
next to it. Regenerate deliberately with `--write-manifest`; never to make a
failure go away.

### What the fixpoint test is not

`stage1 == stage2 == stage3` is the property a Thompson payload is *built to
preserve*. It is worth having — it forces an attacker to write a complete
self-reproducing quine rather than a one-generation patch — but a green
fixpoint and a compromised chain are indistinguishable from outside, as
demonstrated above. Never cite it as a security result.

## The xz lesson

CVE-2024-3094 (xz-utils, 2024) put a backdoor into millions of systems without
touching the library's source: the payload lived in the **build machinery** and
in files that looked like **test data**. Both categories exist here, and worse
than in most projects — one of the kernel's generators packs its test programs
into a file the shipped kernel embeds, so **a "test" file in this project
becomes code running inside the operating system.** Test data and build
scripts are therefore hashed in the manifest exactly like source. Reviewing
`coff.c0` while waving through a new kernel test program would reproduce the
xz failure precisely.

## Two gaps the c2 discussion names that are still open

**The inspectors are compromised too.** From the article: the payload
*"easily propagates into the binaries of all the inspectors, debuggers,
disassemblers, and dumpers a programmer would use to try to detect it. And
defeats them."* Every check in `verify_bootstrap.sh` runs through `cmp`,
`diff`, `sha256sum`, `readelf`, `strings`, `grep` and bash, all built by the
same distribution toolchain being audited, and the kernel's test scripts drive
QEMU through python3. A sufficiently determined
attacker owns the audit as well as the compiler. There is no practical fix
here, and the honest mitigation is that it raises the recognition problem
enormously: the payload would have to recognise and subvert this specific
script, not merely `login`. Naming it is the best that can be done. The
one narrowing is for `cmp` and `sha256sum`, the two tools every verdict
rests on. c0 versions of both, built by the verified `coff1`, cross-check
the system ones (see the table above).

**smed has no diverse implementation.** smed is checked against GNU as over
the whole corpus and over the kernel's hand-written assembly, which is a real
oracle. A flipped `sub` direction bit makes 13 programs diverge at once. But
that oracle is the very tool that is off the build, so it disappears the day
binutils is uninstalled. A second c0 assembler, or a
byte-level comparison against a disassembler, would be the durable answer.

**One backend, so ring-3 code has an oracle.** `--elf` is a target of the
one assembly backend (a handful of emit lines that differ in a syscall
number), mirrored in `coff0.c`, and smed assembles the result at the ring-3
base. Ring-3 code therefore gets exactly the coverage the kernel gets, the
`coff0`-versus-`coff1` diff and the corpus DDC, plus smed's own oracle. A
second backend that wrote machine code directly would have none of that,
which is why there is no such backend. What remains true is the sentence
above: smed has no diverse implementation.

The `--elf` target is a coff feature aimed at Moonshot, and it is in this
repository; the kernel and its ring-3 programs are not, so the coverage
claims above describe what runs when both trees are checked out together.

## Standing rules

1. **No binary enters a build path without a byte-check against source.** New
   tool, generator or prebuilt artifact: it gets a row in the table and a
   check in `verify_bootstrap.sh`, or it does not join.
2. **Never trust a timestamp.** A gate like `[ "$COFF_SRC" -nt "$COFF1" ]`
   is one a tampered binary answers correctly.
3. **Keep the seed readable and portable.** `coff0.c` is the only artifact
   small enough to audit in an afternoon and the only reason tcc, DDC and the
   full-source bootstrap path are available at all. Do not retire it because
   `coff1` can do everything it does. If a change makes `coff0.c` stop
   building under tcc, that change is wrong.
4. **The DDC corpus is what the build compiles.** A payload is caught only
   where it fires. `ddc_entries.sh` reads the corpus from the kernel's build
   scripts and fails the audit when one of them gains a compile, a script call
   or an embed it does not model. That check catches honest drift, not a
   deliberate evasion, and a hostile edit to those scripts is caught only by
   reviewing them, since they live in the kernel's repository and not in this
   manifest. Teach it the new compile and never relax the check.
5. **Test data is code.** Anything that reaches the kernel is reviewed and
   hashed as source, however it is labelled.
6. **Prefer removing a tool to verifying it.** smed taking the place of `as`
   and `ld` is worth more than any amount of checking them, and a single
   backend does more for ring-3 code than any oracle for a second one could.
7. **Determinism before diversity.** Any nondeterminism in coff's output
   blinds DDC and must be fixed before it is treated as noise.
8. **A call cannot write a local.** c0 has no address-of for local variables, so no
   function can reach another function's locals. Three things rely on it: the register
   allocator keeping locals in callee-saved registers across calls, `is_stable_leaf`
   letting a local trade places with a call (in a commutative operator and in the address
   of `buf[i]`), and compound assignment to a variable being compiled as `x = x OP E`.
   Any change that lets a local's address escape, by an operator, a builtin or a calling
   convention, revisits all three and needs security review before it lands.
9. **A store keeps its address in rdi only across a number or a variable.** `keeps_rdi`
   in both compilers alone decides this. Every call and every inline builtin writes rdi.
   Widening it, or emitting anything that writes rdi between the address and the store,
   needs security review.

## Open work, in priority order

1. **Get tcc from a bootstrapped seed instead of from gcc.** Run
   `live-bootstrap` (or Guix's full-source bootstrap) to produce a tcc
   descended from the 357-byte hex0 seed, then re-run DDC against it. That
   converts the tcc row from "partially mitigated" to something close to
   settled, and makes the full-source bootstrap real rather than credible.
2. **Audit `coff0.c` by hand, once, and record the date.** 3346
   lines, one sitting. It is the only artifact where this is feasible, which
   is exactly why it is worth doing.

## What none of this achieves

It does not prove the kernel has no backdoor, and nothing available to a
hobbyist can. The `as` and `ld` that produced the first `smed`, the Linux
kernel, CPU microcode and the silicon are all trusted on faith. The c2 wiki discussion puts the endgame at building a
machine from salvaged TTL logic, and is right that even that convinces nobody
but the builder.

What is achieved is narrower and real: every binary between `coff0.c` and a
running kernel is derived from source in this repository and continuously
checked to still be; a tampered compiler or assembler cannot silently build
a kernel; gcc, as and ld are not in the per-build set; the compiler is verified against an
independent implementation over the code that actually matters; and the
remaining trusted components are enumerated rather than forgotten. One of
those remains genuinely open rather than merely acknowledged, the audit
tooling itself, described above. The honest
claim is **a small, named, shrinking trusted base with a mapped route to a
357-byte auditable seed** — not the absence of backdoors.

One structural advantage is worth keeping in view: c0 is a language nobody
else has ever seen, and a Thompson payload must recognise its target. No
existing attack recognises `coff.c0`. The exposure is `coff0.c`, which is
ordinary C — which is why the diversity items sit at the top of the list.

## References

- Ken Thompson, *Reflections on Trusting Trust*, CACM 27(8), 1984.
- <https://wiki.c2.com/?TheKenThompsonHack>
- David A. Wheeler, *Fully Countering Trusting Trust through Diverse
  Double-Compiling*, 2009. <https://dwheeler.com/trusting-trust/>
- <https://www.schneier.com/blog/archives/2006/01/countering_trus.html>
- *Diverse Double-Compiling in a CI/CD Pipeline* (ddc4cd), KTH, 2025.
  <https://kth.diva-portal.org/smash/get/diva2:1998446/FULLTEXT01.pdf>
- Bootstrappable Builds. <https://bootstrappable.org/>
- *The Full-Source Bootstrap: Building from source all the way down*, GNU
  Guix, 2023.
  <https://guix.gnu.org/en/blog/2023/the-full-source-bootstrap-building-from-source-all-the-way-down/>
- live-bootstrap. <https://man.sr.ht/~oriansj/bootstrappable/live-bootstrap.md>
- stage0-posix. <https://github.com/oriansj/stage0-posix>
- *Bootstrappable builds: how and why*, LWN, 2025.
  <https://lwn.net/Articles/1088279/>
- Reproducible Builds. <https://reproducible-builds.org/>
- xz-utils backdoor, CVE-2024-3094.
