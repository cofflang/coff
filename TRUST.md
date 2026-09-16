# Trust

This file is the trusting-trust perspective for c0, coff and the operating
system I am writing in c0, Moonshot. It is the first thing to read before
changing how any of the three are built, and it takes priority over
convenience, build speed and elegance.

It is written from the point of view of my own working tree, where the
compiler and the kernel sit next to each other and the bootstrap binaries are
checked in. This repository commits no binaries, so where the text says a
binary is "committed" or "hash-gated", read that as a statement about that
tree; the checks themselves ship here and report SKIP for anything that is not
present.

Last full review: 2026-09-05. smed landed the same day.

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

A working Thompson attack was built against this exact toolchain on
2026-09-05, as a test of whether the defences detect anything. A shim standing
in for a compromised gcc silently altered the `coff0` it emitted, leaving
`coff0.c` byte-identical to git. Results:

- The kernel's password check was made to always succeed. The payload
  existed in no source file.
- **The whole test suite passed. Both bootstrap fixpoints passed. The
  committed-artifact checks passed.** A green suite on a compromised machine.
- A stage1-only DDC check **also passed**, because the payload fired on a
  program the compiler never compiles while checking itself.
- Corpus-wide DDC, with the kernel sources in the corpus, **caught it** — the
  diff showed the password check reduced to `mov rax, 1 / ret`.

That last iteration is why the DDC stage compares every c0 source it can find
rather than one. The check that only looked at the compiler compiling itself
was worthless against a payload aimed at the kernel.

## What is trusted, and what is done about it

The trusted computing base for a kernel image. "Checked" means something
automated fails the build when it is wrong.

| Component | Role | Status |
|---|---|---|
| `coff1` binary | compiles the kernel | **checked** — byte-verified against a fresh bootstrap; hash-gated on every kernel build |
| `c0/coff.c0` | source of `coff1` | readable, about 3000 lines, in git, hashed in the manifest |
| `coff0` binary | bootstraps `coff1` once | **checked** — byte-verified against a fresh build of `coff0.c` |
| `coff0.c` | the seed compiler | readable, about 2000 lines of plain C, hashed. Buildable by gcc **and tcc** |
| gcc | *optional* | **no longer required.** See "gcc is out of the trusted base" |
| **tcc 0.9.28** | builds `coff0` | **partially mitigated** — DDC-cross-checked against gcc over the whole corpus. Not yet from a bootstrapped seed |
| `smed` | assembles and links `coff1` and every c0 program | **checked** — differential-tested against as+ld over the whole corpus, self-hosting fixpoint, hashed in the manifest |
| GNU as 2.46.0 | *no longer on the coff path.* Still assembles the kernel's hand-written boot stage and smed's own first bootstrap | **partially removed.** Thompson's own example of a better hiding place than a compiler |
| GNU ld 2.46.0 | *no longer on the coff path.* Still links the kernel image and smed's first bootstrap | **partially removed.** Same |
| glibc | linked into `coff0` only | `coff1` is statically linked and uses raw syscalls, so glibc is in the seed path only, never per-build |
| python3 | the kernel's generators | **hashed** in the manifest. Bakes compiler output into the shipped kernel |
| kernel test programs, `tests/` | test data that becomes kernel content | **hashed** — see "the xz lesson" |
| Build scripts | decide what gets compiled | **hashed** — build code is code |
| **`--elf` / `--raw` backend** | builds every ring-3 program baked into the kernel | **OUTSIDE DDC COVERAGE.** `coff0.c` has no machine-code backend, so there is no second implementation to compare against |
| **the audit's own tools** | `cmp`, `diff`, `sha256sum`, `readelf`, `strings`, `grep`, `as`, `ld`, bash, python3 | **UNMITIGATED.** The inspectors are built by the same toolchain they inspect |
| the ISO builder | produces the bootable image | in the boot path, not in the kernel image |
| Linux, microcode, silicon | everything | out of scope, honestly so |

## gcc is out of the trusted base

Measured 2026-09-05: `coff0` built by **tcc alone**, with gcc absent from the
entire chain, bootstraps a `coff1` that is **byte-identical** to the committed
one, and that `coff1` emits **byte-identical** assembly for the kernel. The
full suite passes gcc-free (`CC=tcc ./run_tests.sh`).

This matters more than it first looks. gcc is reachable from nothing — there
is no way to obtain a trustworthy gcc except from another gcc. tcc *is*
reachable: the Bootstrappable Builds work has a complete chain from a
**357-byte `hex0` seed**, small enough to verify by hand against its own
documented source, up through `hex1`/`hex2`/`M0`/`M1`/`M2-Planet`, GNU Mes and
MesCC, to a self-hosting tcc. Guix has shipped a full-source bootstrap on this
basis since 2022.

So the kernel now has a **credible path to a full-source bootstrap**:

```
hex0 (357 bytes, hand-auditable)
  -> hex1 -> hex2 -> M0 -> M1 -> M2-Planet
  -> GNU Mes / MesCC
  -> tcc
  -> coff0        (about 2000 lines of C, hand-auditable)
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
seed. Closing that is item 2 below.

## What is checked, and by what

**`./verify_bootstrap.sh`** — the full audit, four stages:

1. Committed artifacts byte-match a fresh rebuild from source (SKIP here,
   where nothing is committed; the rebuild itself still has to succeed).
2. Bootstrap fixpoint (stage1 == stage2 == stage3).
3. Determinism and environment independence — output must be identical under
   a hostile locale (`tr_TR`, where `toupper('i')` is not `'I'`; the classic
   locale that breaks lexers), a hostile TZ, and a forced
   `SOURCE_DATE_EPOCH`; `coff0` must not vary with the build directory;
   `coff1` must embed no host path, no build date and no GNU build-id.
   Reproducibility comes first because DDC cannot distinguish an attack from
   noise without it.
4. Diverse double-compiling over every c0 source it can find — the tests,
   the compiler, smed, and the kernel sources when that tree sits next to
   this one — plus manifest validation.

**`./run_tests.sh`** — the unit tests, the differential stages and the
fixpoint checks. `CC=tcc` runs the whole thing without gcc.

**The kernel build** — hash-gates `coff1` on every build and refuses to
produce a kernel on mismatch. Since this repository commits no binaries and
its manifest records none, the kernel's build script keeps its own record of
the `coff1` hash, written right after a passing audit and nowhere else. A
`coff1` with no recorded hash at all is unverified, not tampered: it gets the
full audit before compiling anything. Re-bootstraps and re-runs the full
audit when `coff.c0` moves. Prefers tcc as the seed compiler when present.
Fails closed.

**`./smed_tests.sh`** — four checks on the assembler: a differential corpus
against as+ld over every c0 source, the compiler rebuilding itself through
smed, the smed self-hosting fixpoint, and the whole test suite run with no GNU
tool anywhere in the pipeline.

**`BOOTSTRAP.sha256`** — hashes of every bootstrap artifact, every build
script and the whole test corpus, plus the versions of every tool trusted to
produce them. In this repository it covers sources, scripts and tests, since
no binaries are committed, and nothing outside this repository, so the same
committed file matches whether or not the kernel sits next to it. Regenerate
deliberately with `--write-manifest`; never to make a failure go away.

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
than in most projects — one of the kernel's generators bakes its test programs
into the shipped kernel as byte arrays, so **a "test" file in this project
becomes code running inside the operating system.** Test data and build
scripts are therefore hashed in the manifest exactly like source. Reviewing
`coff.c0` while waving through a new kernel test program would reproduce the
xz failure precisely.

## Two gaps the c2 discussion names that are still open

**The inspectors are compromised too.** From the article: the payload
*"easily propagates into the binaries of all the inspectors, debuggers,
disassemblers, and dumpers a programmer would use to try to detect it. And
defeats them."* Every check in `verify_bootstrap.sh` runs through `cmp`,
`diff`, `sha256sum`, `readelf`, `strings`, `grep`, bash and python3, all built
by the same distribution toolchain being audited. A sufficiently determined
attacker owns the audit as well as the compiler. There is no practical fix
here, and the honest mitigation is that it raises the recognition problem
enormously: the payload would have to recognise and subvert this specific
script, not merely `login`. Naming it is the best that can be done.

**smed has no diverse implementation either, and neither does the machine-code
backend.** smed is checked against GNU as over the whole corpus today, which
is a real oracle and caught a flipped `sub` direction bit immediately (13
programs diverged). But that oracle is the very tool being removed, so it
disappears the day binutils does. A second c0 assembler, or a byte-level
comparison against a disassembler, would be the durable answer.

**The machine-code backend has no diverse implementation.** DDC compares
`coff0` against `coff0`, and `coff0.c` implements only the text backend.
Meanwhile every ring-3 program in the kernel is built with `coff1 --elf` and
baked into the shipped image. So the code path that produces programs running
inside the operating system is the one path with no oracle of any kind,
diverse or otherwise. Several silent wrong-code bugs have already been found in
it by eye. This is the largest coverage hole in the whole scheme and it is not
a hypothetical one.

## Standing rules

1. **No binary enters a build path without a byte-check against source.** New
   tool, generator or prebuilt artifact: it gets a row in the table and a
   check in `verify_bootstrap.sh`, or it does not join.
2. **Never trust a timestamp.** The kernel build once gated `coff1` on
   `[ "$COFF_SRC" -nt "$COFF1" ]`, which a tampered binary answers correctly.
3. **Keep the seed readable and portable.** `coff0.c` is the only artifact
   small enough to audit in an afternoon and the only reason tcc, DDC and the
   full-source bootstrap path are available at all. Do not retire it because
   `coff1` can do everything it does. If a change makes `coff0.c` stop
   building under tcc, that change is wrong.
4. **DDC corpus must contain the code worth attacking.** A payload is caught
   only where it fires. When the kernel grows a new standalone-compilable
   source, it belongs in the corpus.
5. **Test data is code.** Anything that reaches the kernel is reviewed and
   hashed as source, however it is labelled.
6. **Prefer removing a tool to verifying it.** `--raw`/`--elf` removing `as`
   and `ld` is worth more than any amount of checking them.
7. **Determinism before diversity.** Any nondeterminism in coff's output
   blinds DDC and must be fixed before it is treated as noise.

## Open work, in priority order

1. **smed milestone two: the kernel.** smed already replaces `as` and `ld`
   for coff and every c0 program. The kernel's hand-written boot stage still
   goes through GNU as and GNU ld. What is missing is a couple of dozen
   mnemonics coff never emits (`lgdt`, `lidt`, `ltr`, `iretq`, `rdmsr`,
   `wrmsr`, `movabs`, `shr`, and the rest), mixed `.code32`/`.code64`, the
   multiboot header, and the reserved zero-filled sections the linker script
   places before the code. Finishing this removes GNU binutils from the
   kernel entirely.
2. **Get tcc from a bootstrapped seed instead of from gcc.** Run
   `live-bootstrap` (or Guix's full-source bootstrap) to produce a tcc
   descended from the 357-byte hex0 seed, then re-run DDC against it. That
   converts the tcc row from "partially mitigated" to something close to
   settled, and makes the full-source bootstrap real rather than credible.
3. **Give the `--elf` backend an oracle.** Either teach `coff0.c` a
   machine-code backend so DDC covers it, or cross-check `--elf` output
   against the text backend assembled by `as` and compare the resulting
   machine code. The second is cheaper and catches codegen bugs, though it
   is self-consistency rather than diversity, since the same compiler emits
   both sides. The first is the real answer and it doubles as the
   differential oracle that path has never had.
4. **Audit `coff0.c` by hand, once, and record the date.** Two thousand
   lines, one sitting. It is the only artifact where this is feasible, which
   is exactly why it is worth doing.
5. **Add a third diverse compiler** (cproc or chibicc) so DDC does not rest on
   a single alternative.
6. **Consider dropping committed binaries from my working tree.** All are
   reproducible in under a second; this repository already commits none.
   That would delete the surface rather than guard it.

## What none of this achieves

It does not prove the kernel has no backdoor, and nothing available to a
hobbyist can. `as`, `ld`, the Linux kernel, CPU microcode and the silicon are
all trusted on faith. The c2 wiki discussion puts the endgame at building a
machine from salvaged TTL logic, and is right that even that convinces nobody
but the builder.

What is achieved is narrower and real: every binary between `coff0.c` and a
running kernel is derived from source in this repository and continuously
checked to still be; a tampered compiler cannot silently compile a kernel; gcc
has been removed from the required set; the compiler is verified against an
independent implementation over the code that actually matters; and the
remaining trusted components are enumerated rather than forgotten. Two of
those remain genuinely open rather than merely acknowledged, the machine-code
backend and the audit tooling itself, both described above. The honest
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
