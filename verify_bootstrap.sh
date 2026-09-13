#!/usr/bin/env bash
#
# verify_bootstrap.sh - audit the trusting-trust surface of the coff
# bootstrap chain.
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# Ken Thompson's "Reflections on Trusting Trust" (Turing Award lecture,
# 1984) describes a compiler that recognises two things: the login program,
# into which it inserts a backdoor, and its own source, into which it
# inserts the recogniser. Once installed, the payload survives forever and
# the compiler source contains no evidence of it. The attack is not limited
# to compilers -- Thompson: "I could have picked on any program-handling
# program such as an assembler, a loader, or even hardware microcode."
#
# Why this script exists: coff is self-hosting, and run_tests.sh proves the
# bootstrap reaches a fixpoint (stage1 == stage2 == stage3). That fixpoint
# is NOT evidence against a Thompson payload. It is the exact property such
# a payload is built to preserve -- a self-reproducing backdoor produces a
# byte-identical stage2 by construction. A green fixpoint and a fully
# compromised chain look the same from outside. So the fixpoint tests
# answer "is the compiler internally consistent", and this script answers
# the different question: "is every binary on the path to my OS kernel
# derived from source I can read, and does an independent toolchain agree?"
#
# What it checks, in order of what it would catch:
#
#   1. Committed artifacts match a fresh rebuild. In my own tree coff0,
#      coff1.s and coff1 are checked in and the kernel build runs the coff1
#      binary directly. A tampered binary committed once would compile
#      every kernel from then on, and a binary diff inside a commit is not
#      something a human reads. This stage rebuilds the whole chain from
#      source and compares bytes. This repository commits no binaries, so
#      here the comparisons report SKIP unless you have built into the
#      tree; the rebuild itself still has to succeed.
#
#   2. The bootstrap fixpoint. Kept here as well as in run_tests.sh so a
#      single command covers the whole trust story.
#
#   3. Diverse Double-Compiling (Wheeler, 2009). The only stage that can
#      catch a payload rather than merely catching tampering. coff0 is
#      plain C, so an independently written C compiler can build it; if
#      gcc's coff0 and tcc's coff0 behave identically over a whole corpus,
#      a payload would have to exist in both compilers, recognise coff0.c
#      in both, and emit identical injected code from two unrelated
#      codebases. This is what Schneier's 2006 write-up points at, and it
#      is why coff0.c must stay small and buildable by anything.
#
#      IMPORTANT, learned by building a working attack against this exact
#      tree (2026-09-05): comparing only stage1 -- coff0 compiling coff.c0
#      -- is NOT enough. A payload that fires on some other input sails
#      straight through, because coff.c0 never contains the target. A
#      simulated compromised gcc that backdoors my kernel's password check
#      passed a stage1-only DDC while the whole test suite stayed green. So the comparison below runs over every c0
#      source in the ecosystem, Moonshot's kernel included: a payload is
#      caught on any input where it actually fires, so the corpus must
#      contain the code worth attacking.
#
#   4. A recorded manifest. BOOTSTRAP.sha256 pins the hash of every
#      bootstrap artifact plus the versions of every tool trusted to
#      produce it. Binary tampering is invisible in a git diff; a changed
#      line in a text manifest is not.
#
# What it cannot check, stated plainly so nobody reads a green run as more
# than it is: a payload present in gcc AND in the diverse compiler; a
# payload in as, ld, or the kernel; anything in microcode or silicon. See
# TRUST.md for the full trusted computing base and what is being done about
# each item.

set -u
cd "$(dirname "$0")"

ddc_only=0
write_manifest=0
for arg in "$@"; do
  case "$arg" in
    --ddc-only) ddc_only=1 ;;
    --write-manifest) write_manifest=1 ;;
    -h|--help)
      echo "usage: verify_bootstrap.sh [--ddc-only] [--write-manifest]"
      echo "  --ddc-only        run only the diverse-double-compile stage"
      echo "  --write-manifest  regenerate BOOTSTRAP.sha256 from this run"
      exit 0
      ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

# The seed compiler, chosen the same way run_tests.sh chooses it: tcc
# when available, because tcc is reachable from the 357-byte hex0 seed
# through the stage0/M2-Planet/Mes chain and gcc is reachable from nothing.
# Override with CC=gcc.
if [ -z "${CC:-}" ]; then
  if command -v tcc >/dev/null 2>&1; then CC=tcc; else CC=gcc; fi
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

pass=0
fail=0
skip=0

ok()   { echo "PASS $*"; pass=$((pass + 1)); }
bad()  { echo "FAIL $*"; fail=$((fail + 1)); }
warn() { echo "SKIP $*"; skip=$((skip + 1)); }

# Compares two files and reports. Used everywhere instead of a bare cmp so
# a mismatch says which artifact drifted, not just that something did.
cmpq() {
  if cmp -s "$1" "$2"; then return 0; fi
  return 1
}

if [ "$ddc_only" -eq 0 ]; then

echo "=== 1. Committed artifacts vs a fresh rebuild from source ==="
echo ""

# coff0 is rebuilt into the tmpdir rather than over the committed one: the
# point is to compare against what is in the tree, so overwriting it first
# would compare a file with itself.
if $CC -Wall -Wextra -o "$tmpdir/coff0" coff0.c 2>"$tmpdir/cc.err"; then
  ok "coff0.c builds under $CC"
else
  bad "coff0.c does not build under $CC"
  cat "$tmpdir/cc.err"
  echo ""
  echo "$pass passed, $fail failed, $skip skipped"
  exit 1
fi

# The committed coff0 is accepted if ANY available C compiler reproduces it,
# and the matching one is named. Pinning this to a single compiler would
# make the check fail merely because the tree was last built with the other
# one, and a check that cries wolf gets ignored -- which is worse than no
# check. A tampered coff0 still matches none of them, which is the property
# that matters.
if [ -f coff0 ]; then
  coff0_match=""
  for c in $CC gcc tcc clang cproc chibicc pcc; do
    command -v "$c" >/dev/null 2>&1 || continue
    "$c" -w -o "$tmpdir/coff0_$c" coff0.c 2>/dev/null || continue
    if cmpq "$tmpdir/coff0_$c" coff0; then coff0_match="$c"; break; fi
  done
  if [ -n "$coff0_match" ]; then
    ok "committed coff0 == a fresh build of coff0.c (by $coff0_match)"
  else
    bad "committed coff0 matches no fresh build of coff0.c by any available compiler"
    echo "     the committed seed compiler is not what the source produces."
  fi
else
  warn "no committed coff0 to compare against"
fi

if "$tmpdir/coff0" c0/coff.c0 "$tmpdir/coff1.s" 2>"$tmpdir/stage1.err"; then
  ok "coff0 compiles c0/coff.c0 (stage1)"
else
  bad "coff0 could not compile c0/coff.c0"
  cat "$tmpdir/stage1.err"
  echo ""
  echo "$pass passed, $fail failed, $skip skipped"
  exit 1
fi

if [ -f coff1.s ]; then
  if cmpq "$tmpdir/coff1.s" coff1.s; then
    ok "committed coff1.s == freshly emitted stage1.s"
  else
    bad "committed coff1.s differs from freshly emitted stage1"
    diff "$tmpdir/coff1.s" coff1.s | head -20
  fi
else
  warn "no committed coff1.s to compare against"
fi

# The intermediate files are named coff1.s/coff1.o rather than anything
# more descriptive because as(1) records the object file's basename in an
# STT_FILE symbol, which ld copies into the linked binary. Assembling the
# identical assembly through a differently named temp file produces a
# binary that differs at byte 78313 and is otherwise the same program.
# Byte-identity here is therefore only meaningful against the same names
# the canonical bootstrap uses (`as coff1.s -o coff1.o`, as in the README).
# Worth knowing generally: "reproducible build" includes the file names.
if as "$tmpdir/coff1.s" -o "$tmpdir/coff1.o" 2>/dev/null \
   && ld "$tmpdir/coff1.o" -o "$tmpdir/coff1" 2>/dev/null; then
  ok "stage1.s assembles and links"
else
  bad "stage1.s does not assemble/link"
  echo ""
  echo "$pass passed, $fail failed, $skip skipped"
  exit 1
fi

# The one that matters most: my kernel's build script runs THIS binary
# against the kernel source. Everything else in the chain can be clean and it would not
# help if this file is not what the source produces.
if [ -f coff1 ]; then
  if cmpq "$tmpdir/coff1" coff1; then
    ok "committed coff1 == freshly bootstrapped coff1 (the binary Moonshot runs)"
  else
    bad "committed coff1 differs from a fresh bootstrap"
    echo "     this is the binary the kernel is compiled with."
    echo "     do not build a kernel until this is explained."
  fi
else
  warn "no committed coff1 to compare against"
fi

echo ""
echo "=== 2. Bootstrap fixpoint (consistency, not evidence of absence) ==="
echo ""

if "$tmpdir/coff1" c0/coff.c0 "$tmpdir/coff2.s" 2>/dev/null \
   && cmpq "$tmpdir/coff1.s" "$tmpdir/coff2.s"; then
  ok "fixpoint stage1 == stage2"
else
  bad "fixpoint stage1 != stage2"
  diff "$tmpdir/coff1.s" "$tmpdir/coff2.s" | head -10
fi

if as "$tmpdir/coff2.s" -o "$tmpdir/coff2.o" 2>/dev/null \
   && ld "$tmpdir/coff2.o" -o "$tmpdir/coff2" 2>/dev/null \
   && "$tmpdir/coff2" c0/coff.c0 "$tmpdir/coff3.s" 2>/dev/null \
   && cmpq "$tmpdir/coff2.s" "$tmpdir/coff3.s"; then
  ok "fixpoint stage2 == stage3"
else
  bad "fixpoint stage2 != stage3"
fi

fi  # end ddc_only guard

echo ""
echo "=== 2b. Determinism and environment independence ==="
echo ""

# DDC is worthless without reproducibility: if the compiler's output varies
# with the environment, a real mismatch cannot be told apart from noise.
# This is the standing advice from the reproducible-builds work and from
# the ddc4cd thesis (KTH, 2025) -- "start with determinism" is step one.
#
# The locale used is tr_TR on purpose. Turkish has a dotless i, so
# toupper('i') is not 'I' there; it is the classic locale that breaks
# case-folding in lexers and has bitten real compilers.
if "$tmpdir/coff0" c0/coff.c0 "$tmpdir/det_a.s" 2>/dev/null \
   && env -i PATH=/usr/bin TZ=Pacific/Kiritimati LANG=tr_TR.UTF-8 \
        LC_ALL=tr_TR.UTF-8 SOURCE_DATE_EPOCH=1 \
        "$tmpdir/coff0" c0/coff.c0 "$tmpdir/det_b.s" 2>/dev/null \
   && cmpq "$tmpdir/det_a.s" "$tmpdir/det_b.s"; then
  ok "output identical under hostile locale, TZ and SOURCE_DATE_EPOCH"
else
  bad "compiler output depends on the environment"
  echo "     DDC cannot distinguish an attack from noise until this is fixed."
fi

# Build path must not leak into the seed compiler. gcc embeds __FILE__ and
# debug paths when asked to; coff0 is built without -g so it should not,
# but "should not" is not a check.
mkdir -p "$tmpdir/otherpath"
cp coff0.c "$tmpdir/otherpath/"
if (cd "$tmpdir/otherpath" && $CC -Wall -Wextra -o coff0b coff0.c 2>/dev/null) \
   && cmpq "$tmpdir/coff0" "$tmpdir/otherpath/coff0b"; then
  ok "coff0 is independent of the build directory (built with $CC)"
else
  bad "coff0 differs when built from another directory (build path leaks in)"
fi

# Embedded paths and timestamps are the classic reproducibility leaks, and
# a path in a binary is also an information leak in a shipped artifact.
if [ -f coff1 ]; then
  if strings coff1 | grep -qE "$HOME|20[0-9][0-9]-[01][0-9]-[0-3][0-9]"; then
    bad "coff1 embeds a host path or a date"
  else
    ok "coff1 embeds no host path and no build date"
  fi
  if readelf -S coff1 2>/dev/null | grep -qi 'build.id'; then
    bad "coff1 carries a GNU build-id (nondeterministic across toolchains)"
  else
    ok "coff1 carries no GNU build-id"
  fi
fi

echo ""
echo "=== 3. Diverse Double-Compiling (the only stage that catches a payload) ==="
echo ""

# Wheeler's DDC in the shape this project can actually run it. coff1 is
# whatever coff0 emitted, so coff1 agreeing with coff0 proves nothing about
# gcc -- a payload gcc injected into coff0 propagates into coff1 and the
# fixpoint still closes. The independent axis is the C compiler underneath:
# build coff0 with something that is not gcc and check that the two coff0
# binaries behave identically. coff0.c is 2054 lines of plain C precisely
# so this is possible; keeping it small and portable is a security
# property, not tidiness.
#
# tcc is preferred over clang: a separate codebase with no shared lineage
# with gcc, and reachable from the 357-byte hex0 seed through the
# live-bootstrap chain, which is what makes a future full-source bootstrap
# of Moonshot possible at all. See TRUST.md.
#
# Caveat recorded honestly: a tcc built by gcc is not fully independent of
# gcc. It raises the bar a great deal -- gcc would have to recognise tcc's
# source, inject a payload that survives tcc's own self-hosting, and have
# that payload recognise coff0.c -- but it is not the same as a tcc from a
# bootstrapped seed. TRUST.md tracks closing that gap.
# The diverse compiler must not be the seed compiler. Picking the first
# available from a fixed list would happily choose tcc while CC is also
# tcc, comparing a compiler against itself and reporting a pass that means
# nothing -- the most dangerous kind of green check.
diverse=""
for c in tcc gcc clang cproc chibicc pcc; do
  [ "$c" = "$CC" ] && continue
  if command -v "$c" >/dev/null 2>&1; then diverse="$c"; break; fi
done

if [ -z "$diverse" ]; then
  warn "no second C compiler installed -- DDC cannot run"
  echo "     bootstrap diversity is ZERO: every path to coff1 goes through"
  echo "     one gcc. A Thompson payload in that gcc would be invisible to"
  echo "     every other check in this script."
  echo "     fix: install tcc (see TRUST.md for why tcc specifically)"
else
  echo "diverse compiler: $diverse ($($diverse --version 2>&1 | head -1))"
  mkdir -p "$tmpdir/div"
  # Must be $CC, not a hardcoded gcc: with --ddc-only nothing has built
  # the seed side yet, and building it with the same compiler that is about
  # to play the "diverse" role compares a compiler against itself. A
  # compromised gcc passed this check while backdooring the kernel, until
  # this line was fixed (found 2026-09-05 by running the audit under a
  # simulated attack).
  if [ ! -f "$tmpdir/coff0" ]; then
    $CC -Wall -Wextra -o "$tmpdir/coff0" coff0.c 2>/dev/null
  fi
  if "$diverse" -w -o "$tmpdir/coff0_div" coff0.c 2>"$tmpdir/div.err"; then
    ok "coff0.c builds under $diverse"

    # The corpus. Everything that matters is in here, because a payload is
    # only caught on inputs where it fires. Moonshot's kernel sources are
    # included deliberately: they are the code actually worth backdooring,
    # and leaving them out is what let the simulated attack through.
    corpus=""
    for f in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/coff.c0 c0/smed.c0; do
      [ -f "$f" ] && corpus="$corpus $f"
    done
    ms=../moonshot
    kernel_in_corpus=0
    if [ -d "$ms" ]; then
      kernel_in_corpus=1
      for f in "$ms"/*.c0; do
        case "$f" in *_data.c0|*.bak*) continue ;; esac
        [ -f "$f" ] && corpus="$corpus $f"
      done
    else
      warn "moonshot sources not found at $ms -- kernel code is NOT in the DDC corpus"
    fi

    ddc_checked=0
    ddc_bad=0
    for f in $corpus; do
      name=$(basename "$f" .c0)
      # Only inputs both binaries accept are comparable: a file coff0
      # rejects produces no output to diff on either side.
      if ! "$tmpdir/coff0" "$f" "$tmpdir/a_$name.s" 2>/dev/null; then continue; fi
      if ! "$tmpdir/coff0_div" "$f" "$tmpdir/div/b_$name.s" 2>/dev/null; then
        bad "DDC: $diverse-built coff0 rejected $f which gcc-built coff0 accepted"
        ddc_bad=$((ddc_bad + 1))
        continue
      fi
      ddc_checked=$((ddc_checked + 1))
      if ! cmpq "$tmpdir/a_$name.s" "$tmpdir/div/b_$name.s"; then
        bad "DDC MISMATCH on $f ($CC vs $diverse)"
        diff "$tmpdir/a_$name.s" "$tmpdir/div/b_$name.s" | head -12
        ddc_bad=$((ddc_bad + 1))
      fi
    done

    if [ "$ddc_bad" -eq 0 ] && [ "$ddc_checked" -gt 0 ]; then
      ok "DDC: $CC-built and $diverse-built coff0 agree byte-for-byte on all $ddc_checked corpus files"
      if [ "$kernel_in_corpus" -eq 1 ]; then
        echo "     corpus includes Moonshot's kernel sources, so a payload"
        echo "     targeting kernel code is caught where it fires."
      else
        echo "     corpus is this repository only; a payload that fires on"
        echo "     kernel code would not be caught here."
      fi
    elif [ "$ddc_checked" -eq 0 ]; then
      bad "DDC ran but compared nothing -- corpus empty, treat as no coverage"
    else
      echo "     $ddc_bad mismatch(es) over $ddc_checked files compared."
      echo "     Either a compiler bug, undefined behaviour in coff0.c, or"
      echo "     the thing this script exists to find. Do not build a kernel"
      echo "     until it is explained."
    fi
  else
    warn "coff0.c does not build under $diverse"
    head -5 "$tmpdir/div.err"
    echo "     worth fixing: coff0.c staying buildable by any C compiler is"
    echo "     what keeps DDC available at all."
  fi
fi

if [ "$ddc_only" -eq 0 ]; then

echo ""
echo "=== 4. Manifest ==="
echo ""

# Recorded so tampering shows up as a text diff in review. The toolchain
# versions are part of it because a chain is only reproducible against the
# tools that produced it: a hash that changes when gcc changes is expected,
# a hash that changes when nothing changed is not.
{
  echo "# coff bootstrap manifest -- regenerate with ./verify_bootstrap.sh --write-manifest"
  echo "# Recorded so that tampering with a committed binary shows up as a"
  echo "# text diff. See TRUST.md."
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed CC: $CC ($($CC --version 2>&1 | head -1))"
  echo "gcc: $(gcc --version 2>/dev/null | head -1)"
  echo "as: $(as --version 2>/dev/null | head -1)"
  echo "ld: $(ld --version 2>/dev/null | head -1)"
  [ -n "$diverse" ] && echo "diverse: $($diverse --version 2>&1 | head -1)"
  echo ""
  sha256sum coff0.c c0/coff.c0 c0/lex.c0 c0/parse.c0 c0/smed.c0 2>/dev/null
  # In my own tree the bootstrap binaries (coff0, coff1.s, coff1, smed.s,
  # smed) are committed and hashed here too, since they sit on the critical
  # path. This repository commits none of them, so they are left out: a
  # manifest that changes depending on whether you have built in the tree
  # would fail for every fresh clone, and a check that cries wolf gets
  # ignored.
  # Build code is code. The xz-utils backdoor (CVE-2024-3094) did not live
  # in the library source at all: it lived in the build machinery and in
  # files that looked like test data. Both of those categories exist here
  # -- run_tests.sh and my kernel's build script decide what gets compiled,
  # and one of the kernel's generators bakes its test programs into the
  # shipped image as byte arrays. A "test" file in this project becomes
  # code running inside the operating system, so it is hashed like code.
  # The kernel-side files are hashed when the kernel tree sits next to
  # this one, and silently left out otherwise.
  sha256sum run_tests.sh verify_bootstrap.sh smed_tests.sh 2>/dev/null
  for f in ../moonshot/build.sh ../moonshot/gen_seed.py \
           ../moonshot/gen_elf_tests.py; do
    [ -f "$f" ] && sha256sum "$f"
  done
  find tests -name '*.c0' -o -name '*.expected_stdout' 2>/dev/null \
    | LC_ALL=C sort | xargs sha256sum 2>/dev/null
  [ -d ../moonshot/elf_tests ] && find ../moonshot/elf_tests -type f \
    | LC_ALL=C sort | xargs sha256sum 2>/dev/null
} > "$tmpdir/manifest"

if [ "$write_manifest" -eq 1 ]; then
  cp "$tmpdir/manifest" BOOTSTRAP.sha256
  echo "wrote BOOTSTRAP.sha256"
elif [ -f BOOTSTRAP.sha256 ]; then
  # Only the hash lines are compared: the date and tool versions are
  # expected to move, the artifact hashes are not.
  grep -E '^[0-9a-f]{64} ' BOOTSTRAP.sha256 > "$tmpdir/m_old" || true
  grep -E '^[0-9a-f]{64} ' "$tmpdir/manifest" > "$tmpdir/m_new" || true
  if cmpq "$tmpdir/m_old" "$tmpdir/m_new"; then
    ok "BOOTSTRAP.sha256 matches the tree"
  else
    bad "BOOTSTRAP.sha256 does not match the tree"
    diff "$tmpdir/m_old" "$tmpdir/m_new"
    echo "     if this change is intended, re-run with --write-manifest."
  fi
else
  warn "no BOOTSTRAP.sha256 yet -- run with --write-manifest to create it"
fi

fi  # end ddc_only guard

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
