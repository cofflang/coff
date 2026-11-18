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
# 1984) describes a compiler that recognises two things, the login program,
# into which it inserts a backdoor, and its own source, into which it
# inserts the recogniser. Once installed, the payload survives forever and
# the compiler source contains no evidence of it. The attack is not limited
# to compilers -- in Thompson's words, "I could have picked on any
# program-handling program such as an assembler, a loader, or even hardware
# microcode."
#
# This script exists because coff is self-hosting, and run_tests.sh proves the
# bootstrap reaches a fixpoint (stage1 == stage2 == stage3). That fixpoint
# is NOT evidence against a Thompson payload. It is the exact property such
# a payload is built to preserve -- a self-reproducing backdoor produces a
# byte-identical stage2 by construction. A green fixpoint and a fully
# compromised chain look the same from outside. So the fixpoint tests
# answer "is the compiler internally consistent", and this script answers
# a different question, "is every binary on the path to my OS kernel
# derived from source I can read, and does an independent toolchain agree?"
#
# What it checks, in order of what it would catch.
#
#   1. In-tree binaries match a fresh rebuild. No binary is committed.
#      coff0, coff1 and smed are built into this directory from source,
#      and the kernel build runs the coff1 and smed found here directly,
#      so a binary altered after it was built would compile or assemble
#      every kernel from then on. This stage rebuilds the whole chain from
#      source into a temporary directory and compares bytes against the
#      binaries in the tree. smed is on that path as well, so coff1 must
#      reproduce smed.s from c0/smed.c0, and smed must reproduce itself
#      from that assembly. On a fresh clone that has not been built yet
#      the comparisons report SKIP, and the rebuilds still have to succeed.
#
#   2. The bootstrap fixpoint. Kept here as well as in run_tests.sh so a
#      single command covers the whole trust story.
#
#   3. Diverse Double-Compiling (Wheeler, 2009). The only stage that can
#      catch a payload rather than merely catching tampering. coff0 is
#      plain C, so independently written C compilers can build it. If
#      gcc's, tcc's and cproc's coff0 behave identically over a whole
#      corpus, a payload would have to exist in all of those compilers, recognise coff0.c
#      in each, and emit identical injected code from unrelated codebases. This is what Schneier's 2006 write-up points at, and it
#      is why coff0.c must stay small and buildable by anything.
#
#      IMPORTANT. Comparing only stage1 -- coff0 compiling coff.c0 -- is
#      NOT enough. A payload that fires on some other input sails straight
#      through, because coff.c0 never contains the target. A simulated
#      compromised gcc that backdoors my kernel's password check passes a
#      stage1-only DDC while the whole test suite stays green. So the
#      comparison below runs over every c0 source in the ecosystem, the
#      kernel included when it sits next to this tree. A payload is caught
#      on any input where it actually fires, so the corpus must contain the
#      code worth attacking.
#
#   4. A recorded manifest. BOOTSTRAP.sha256 pins the hash of every
#      bootstrap artifact plus the versions of every tool trusted to
#      produce it. Binary tampering is invisible in a git diff; a changed
#      line in a text manifest is not. The manifest hashes are themselves
#      cross-checked by c0/sha256.c0, and the comparisons by c0/cmp.c0,
#      both built by the coff1 the audit has just verified.
#
# What it cannot check, stated plainly so nobody reads a green run as more
# than it is, is a payload present in gcc AND in every diverse compiler, a
# payload in as, ld, or the kernel, or anything in microcode or silicon. See
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

# The seed compiler, chosen the same way moonshot/build.sh chooses it, is
# tcc when available, because tcc is reachable from the 357-byte hex0 seed
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
#
# The audit's own tools. Until coff1 has been verified, this is the
# system's cmp and nothing else. Once it has, c0/cmp.c0 built by that coff1
# is set as own_cmp, and from then on every comparison runs both tools and
# a disagreement fails the audit outright, whichever way it goes. Two
# inspectors with unrelated origins then have to be wrong in the same way
# for a bad comparison to get through. Same for hashes (hashq, own_sha).
own_cmp=""
own_sha=""
cmp_checked=0
tools_disagree=0
cmpq() {
  local sys=1 own=1
  cmp -s "$1" "$2" && sys=0
  if [ -n "$own_cmp" ]; then
    "$own_cmp" -s "$1" "$2" && own=0
    cmp_checked=$((cmp_checked + 1))
    if [ "$own" -ne "$sys" ]; then
      echo "     AUDIT TOOLS DISAGREE: cmp exit $sys, c0/cmp exit $own, on $1 $2"
      tools_disagree=$((tools_disagree + 1))
      return 1
    fi
  fi
  return $sys
}

# sha256sum, cross-checked against c0/sha256 the same way once it exists.
hash_checked=0
hashq() {
  local sys own
  [ "$#" -eq 0 ] && return 0
  sys=$(sha256sum "$@" 2>/dev/null)
  if [ -n "$own_sha" ]; then
    own=$("$own_sha" "$@" 2>/dev/null)
    hash_checked=$((hash_checked + $#))
    if [ "$sys" != "$own" ]; then
      diff <(printf '%s\n' "$sys") <(printf '%s\n' "$own") \
        | sed -n 's/^< [0-9a-f]\{64\}  /     AUDIT TOOLS DISAGREE on the hash of /p' \
        >> "$tmpdir/hash_disagree"
      # A file one tool could read and the other could not shows up as a
      # line on only one side, so record the raw diff too if nothing
      # matched above.
      [ -s "$tmpdir/hash_disagree" ] \
        || echo "     AUDIT TOOLS DISAGREE on hashes of: $*" >> "$tmpdir/hash_disagree"
    fi
  fi
  [ -n "$sys" ] && printf '%s\n' "$sys"
  return 0
}

if [ "$ddc_only" -eq 0 ]; then

echo "=== 1. In-tree binaries vs a fresh rebuild from source ==="
echo ""

# coff0 is rebuilt into the tmpdir rather than over the in-tree one. The
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

# The in-tree coff0 is accepted if ANY available C compiler reproduces it,
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
    ok "in-tree coff0 == a fresh build of coff0.c (by $coff0_match)"
  else
    bad "in-tree coff0 matches no fresh build of coff0.c by any available compiler"
    echo "     the in-tree seed compiler is not what the source produces."
  fi
else
  warn "no in-tree coff0 to compare against"
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
    ok "in-tree coff1.s == freshly emitted stage1.s"
  else
    bad "in-tree coff1.s differs from freshly emitted stage1"
    diff "$tmpdir/coff1.s" coff1.s | head -20
  fi
else
  warn "no in-tree coff1.s to compare against"
fi

# The intermediate files are named coff1.s/coff1.o rather than anything
# more descriptive because as(1) records the object file's basename in an
# STT_FILE symbol, which ld copies into the linked binary. Assembling the
# identical assembly through a differently named temp file produces a
# binary that differs at byte 78313 and is otherwise the same program.
# Byte-identity here is therefore only meaningful against the same names
# the canonical bootstrap uses (moonshot/build.sh runs `as coff1.s -o
# coff1.o`). Worth knowing generally, "reproducible build" includes the file
# names.
if as "$tmpdir/coff1.s" -o "$tmpdir/coff1.o" 2>/dev/null \
   && ld "$tmpdir/coff1.o" -o "$tmpdir/coff1" 2>/dev/null; then
  ok "stage1.s assembles and links"
else
  bad "stage1.s does not assemble/link"
  echo ""
  echo "$pass passed, $fail failed, $skip skipped"
  exit 1
fi

# The one that matters most. moonshot/build.sh runs THIS binary against the
# kernel source. Everything else in the chain can be clean and it would not
# help if this file is not what the source produces.
if [ -f coff1 ]; then
  if cmpq "$tmpdir/coff1" coff1; then
    ok "in-tree coff1 == freshly bootstrapped coff1 (the binary Moonshot runs)"
  else
    bad "in-tree coff1 differs from a fresh bootstrap"
    echo "     this is the binary the kernel is compiled with."
    echo "     do not build a kernel until this is explained."
  fi
else
  warn "no in-tree coff1 to compare against"
fi

# The audit's own tools, from the coff1 just rebuilt from source. See the
# note above cmpq. Built with as and ld rather than smed, because smed has
# not been checked yet at this point and these tools are about to check it.
if "$tmpdir/coff1" c0/cmp.c0 "$tmpdir/c0cmp.s" 2>/dev/null \
   && as "$tmpdir/c0cmp.s" -o "$tmpdir/c0cmp.o" 2>/dev/null \
   && ld "$tmpdir/c0cmp.o" -o "$tmpdir/c0cmp" 2>/dev/null \
   && "$tmpdir/coff1" c0/sha256.c0 "$tmpdir/c0sha256.s" 2>/dev/null \
   && as "$tmpdir/c0sha256.s" -o "$tmpdir/c0sha256.o" 2>/dev/null \
   && ld "$tmpdir/c0sha256.o" -o "$tmpdir/c0sha256" 2>/dev/null; then
  own_cmp="$tmpdir/c0cmp"
  own_sha="$tmpdir/c0sha256"
  ok "c0/cmp.c0 and c0/sha256.c0 built by the verified coff1, cross-checking cmp and sha256sum from here on"
else
  bad "could not build c0/cmp.c0 and c0/sha256.c0 with the fresh coff1"
fi

# smed assembles and links the kernel, so it has exactly coff1's standing,
# a binary in the tree that the kernel build runs directly. Its assembly must
# be what coff1 emits from c0/smed.c0, and the binary must be what smed
# produces from that assembly -- the in-tree smed assembling its own
# source is a fixpoint check, and only the very first smed is produced any
# other way (from as+ld).
if [ -f "$tmpdir/coff1" ]; then
  if "$tmpdir/coff1" c0/smed.c0 "$tmpdir/smed.s" 2>/dev/null; then
    if [ -f smed.s ]; then
      if cmpq "$tmpdir/smed.s" smed.s; then
        ok "in-tree smed.s == coff1's output for c0/smed.c0"
      else
        bad "in-tree smed.s differs from what coff1 emits for c0/smed.c0"
      fi
    else
      warn "no in-tree smed.s to compare against"
    fi
    if [ -x smed ]; then
      if ./smed "$tmpdir/smed.s" "$tmpdir/smed_self" 2>/dev/null \
         && cmpq "$tmpdir/smed_self" smed; then
        ok "in-tree smed == smed assembling its own source (the binary that links the kernel)"
      else
        bad "in-tree smed is not what smed produces from its own source"
        echo "     this is the binary the kernel is assembled and linked with."
        echo "     do not build a kernel until this is explained."
      fi
    else
      warn "no in-tree smed to compare against"
    fi
  else
    bad "coff1 could not compile c0/smed.c0"
  fi
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

# Must be $CC, not a hardcoded gcc. With --ddc-only nothing has built the
# seed side yet, and both 2b and 3 below run it. Building it with the same
# compiler that is about to play the "diverse" role would compare a compiler
# against itself, which lets a compromised gcc pass the DDC while
# backdooring the kernel.
if [ ! -f "$tmpdir/coff0" ]; then
  $CC -Wall -Wextra -o "$tmpdir/coff0" coff0.c 2>/dev/null
fi

echo ""
echo "=== 2b. Determinism and environment independence ==="
echo ""

# DDC is worthless without reproducibility. If the compiler's output varies
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
# fixpoint still closes. The independent axis is the C compiler underneath,
# so coff0 is built with something that is not gcc and the two coff0
# binaries must behave identically. coff0.c is plain C precisely so this is
# possible; keeping it small and portable is a security
# property, not tidiness.
#
# tcc is preferred over clang, being a separate codebase with no shared lineage
# with gcc, and reachable from the 357-byte hex0 seed through the
# live-bootstrap chain, which is what makes a future full-source bootstrap
# of Moonshot possible at all. See TRUST.md.
#
# One caveat, recorded honestly. A tcc built by gcc is not fully independent of
# gcc. It raises the bar a great deal -- gcc would have to recognise tcc's
# source, inject a payload that survives tcc's own self-hosting, and have
# that payload recognise coff0.c -- but it is not the same as a tcc from a
# bootstrapped seed. TRUST.md tracks closing that gap.
# The diverse compiler must not be the seed compiler. Picking the first
# available from a fixed list would happily choose tcc while CC is also
# tcc, comparing a compiler against itself and reporting a pass that means
# nothing -- the most dangerous kind of green check.
# Every available diverse compiler is used, not just the first, since each is
# another unrelated party a payload would have to be present in, behaving
# identically. cproc is looked for in ~/.local/opt/c0-ddc too, where
# fetch_ddc_cproc.sh builds it at pinned versions.
[ -x "$HOME/.local/opt/c0-ddc/cproc" ] && PATH="$HOME/.local/opt/c0-ddc:$PATH"
diverse_list=""
for c in tcc gcc clang cproc chibicc pcc; do
  [ "$c" = "$CC" ] && continue
  if command -v "$c" >/dev/null 2>&1; then diverse_list="$diverse_list $c"; fi
done

# cproc compiles and leaves linking to its driver, whose default asks for
# gcc's crtbegin.o. coff0 is plain C and needs none of gcc's runtime, so it
# is linked here against glibc's own start files instead -- which also keeps
# gcc out of the cproc path altogether.
build_coff0_with() { # compiler output
  if [ "$1" = cproc ]; then
    cproc -c -o "$2.o" coff0.c && ld -o "$2" -dynamic-linker /lib64/ld-linux-x86-64.so.2 \
      /usr/lib/crt1.o /usr/lib/crti.o "$2.o" -lc /usr/lib/crtn.o
  else
    "$1" -w -o "$2" coff0.c
  fi
}

if [ -z "$diverse_list" ]; then
  warn "no second C compiler installed -- DDC cannot run"
  echo "     bootstrap diversity is ZERO: every path to coff1 goes through"
  echo "     one gcc. A Thompson payload in that gcc would be invisible to"
  echo "     every other check in this script."
  echo "     fix: install tcc (see TRUST.md for why tcc specifically)"
else
  # The corpus. Everything that matters is in here, because a payload is
  # only caught on inputs where it fires. ddc_entries.sh lists every compile
  # the coff suite and the Moonshot build perform, each with the directory
  # and flags it is compiled with, Moonshot's kernel and every program it
  # embeds included. Nothing in the list is tried and dropped. A file the
  # reference coff0 rejects is a failure unless it is a compile-error test,
  # and a compile-error test must be rejected by both compilers with the
  # same message and exit code. run_tests.sh's codegendiff reads the same
  # list, so the two cannot drift apart.
  ms=../moonshot
  ms_listed=0
  mkdir -p "$tmpdir/entries"
  : > "$tmpdir/ddc_list"
  if ./ddc_entries.sh coff > "$tmpdir/ddc_coff" 2>"$tmpdir/ddc_entries.err"; then
    sed 's/^/coff|/' "$tmpdir/ddc_coff" >> "$tmpdir/ddc_list"
  else
    bad "DDC: the coff corpus could not be listed"
    sed 's/^/     /' "$tmpdir/ddc_entries.err"
  fi
  if [ -d "$ms" ]; then
    if ./ddc_entries.sh moonshot "$ms" "$tmpdir/entries" > "$tmpdir/ddc_ms" 2>"$tmpdir/ddc_entries.err"; then
      sed 's/^/moonshot|/' "$tmpdir/ddc_ms" >> "$tmpdir/ddc_list"
      ms_listed=1
    else
      bad "DDC: the Moonshot corpus could not be listed, so none of it is compared"
      sed 's/^/     /' "$tmpdir/ddc_entries.err"
    fi
  else
    warn "moonshot sources not found at $ms -- kernel code is NOT in the DDC corpus"
  fi
  ddc_total=$(wc -l < "$tmpdir/ddc_list")
  kmain_total=$(grep -c '/kernel/core/kmain\.c0 with ' "$tmpdir/ddc_list")

  # ddc_compile <coff0> <outdir>, every entry into <outdir>/<n>.s with
  # <outdir>/<n>.err and <outdir>/<n>.rc. Numbered, so two sources with one
  # basename cannot overwrite each other.
  ddc_compile() {
    local c="$1" o="$2" n=0 grp label dir flags file expect
    mkdir -p "$o"
    while IFS='|' read -r grp label dir flags file expect; do
      n=$((n + 1))
      (cd "$dir" && "$c" $flags "$file" "$o/$n.s") 2>"$o/$n.err" >/dev/null
      echo $? > "$o/$n.rc"
    done < "$tmpdir/ddc_list"
  }

  # Every coff0 is built first, then the reference and each diverse coff0
  # compile the whole list at the same time, each into its own directory.
  # The reference side is compiled once and shared by every comparison.
  for diverse in $diverse_list; do
    build_coff0_with "$diverse" "$tmpdir/coff0_$diverse" 2>"$tmpdir/div_$diverse.err" \
      || rm -f "$tmpdir/coff0_$diverse"
  done
  ddc_compile "$tmpdir/coff0" "$tmpdir/ref" &
  for diverse in $diverse_list; do
    [ -x "$tmpdir/coff0_$diverse" ] && ddc_compile "$tmpdir/coff0_$diverse" "$tmpdir/div_$diverse" &
  done
  wait

  # Every entry the reference rejects is reported here, and every
  # compile-error test it accepts. Neither is compared further.
  n=0
  ref_bad=" "
  while IFS='|' read -r grp label dir flags file expect; do
    n=$((n + 1))
    # A compile that left no exit code behind did not finish, and is a
    # failure like any other.
    rc=""
    { read -r rc < "$tmpdir/ref/$n.rc"; } 2>/dev/null
    case "$rc" in
      ''|*[!0-9]*)
        bad "DDC: $CC-built coff0 left no result for $label"
        ref_bad="$ref_bad$n "
        continue ;;
    esac
    if [ "$expect" = error ] && [ "$rc" -eq 0 ]; then
      bad "DDC: $CC-built coff0 accepted $label, a compile-error test"
      ref_bad="$ref_bad$n "
    elif [ "$expect" = ok ] && [ "$rc" -ne 0 ]; then
      bad "DDC: $CC-built coff0 rejected $label (exit $rc), which the build compiles"
      head -3 "$tmpdir/ref/$n.err" | sed 's/^/     /'
      ref_bad="$ref_bad$n "
    fi
  done < "$tmpdir/ddc_list"

  for diverse in $diverse_list; do
    if [ "$diverse" = cproc ] && [ -f "$HOME/.local/opt/c0-ddc/VERSIONS" ]; then
      echo "diverse compiler: cproc ($(tr '\n' ' ' < "$HOME/.local/opt/c0-ddc/VERSIONS"))"
    else
      echo "diverse compiler: $diverse ($($diverse --version 2>&1 | head -1))"
    fi
    div="$tmpdir/div_$diverse"
    if [ -x "$tmpdir/coff0_$diverse" ]; then
      ok "coff0.c builds under $diverse"
      ddc_bad=0
      ddc_coff=0
      ddc_coff_err=0
      ddc_ms=0
      ddc_kmain=0
      n=0
      while IFS='|' read -r grp label dir flags file expect; do
        n=$((n + 1))
        case "$ref_bad" in *" $n "*) continue ;; esac
        rref=""; rdiv=""
        read -r rref < "$tmpdir/ref/$n.rc"
        { read -r rdiv < "$div/$n.rc"; } 2>/dev/null
        case "$rdiv" in
          ''|*[!0-9]*)
            bad "DDC: $diverse-built coff0 left no result for $label"
            ddc_bad=$((ddc_bad + 1)); continue ;;
        esac
        if [ "$expect" = error ]; then
          if [ "$rdiv" -eq 0 ]; then
            bad "DDC: $diverse-built coff0 accepted $label, a compile-error test $CC-built coff0 rejects"
            ddc_bad=$((ddc_bad + 1)); continue
          fi
          if [ "$rref" != "$rdiv" ] || ! cmpq "$tmpdir/ref/$n.err" "$div/$n.err"; then
            bad "DDC MISMATCH on $label, the two coff0 reject it differently ($CC exit $rref, $diverse exit $rdiv)"
            diff "$tmpdir/ref/$n.err" "$div/$n.err" | head -6 | sed 's/^/     /'
            ddc_bad=$((ddc_bad + 1)); continue
          fi
          ddc_coff_err=$((ddc_coff_err + 1))
        else
          if [ "$rdiv" -ne 0 ]; then
            bad "DDC: $diverse-built coff0 rejected $label which $CC-built coff0 accepted"
            head -3 "$div/$n.err" | sed 's/^/     /'
            ddc_bad=$((ddc_bad + 1)); continue
          fi
          # Two empty message files are equal without asking a tool.
          if ! cmpq "$tmpdir/ref/$n.s" "$div/$n.s" \
             || { { [ -s "$tmpdir/ref/$n.err" ] || [ -s "$div/$n.err" ]; } \
                  && ! cmpq "$tmpdir/ref/$n.err" "$div/$n.err"; }; then
            bad "DDC MISMATCH on $label ($CC vs $diverse)"
            diff "$tmpdir/ref/$n.s" "$div/$n.s" | head -12
            diff "$tmpdir/ref/$n.err" "$div/$n.err" | head -6 | sed 's/^/     /'
            ddc_bad=$((ddc_bad + 1)); continue
          fi
        fi
        if [ "$grp" = coff ]; then
          ddc_coff=$((ddc_coff + 1))
        else
          ddc_ms=$((ddc_ms + 1))
          case "$label" in */kernel/core/kmain*\ with\ *) ddc_kmain=$((ddc_kmain + 1)) ;; esac
        fi
      done < "$tmpdir/ddc_list"
      ddc_checked=$((ddc_coff + ddc_ms))

      if [ -d "$ms" ] && [ "$ms_listed" -eq 0 ]; then
        # Already a failure above. No PASS line, since one would describe a
        # corpus without Moonshot in it.
        echo "     $ddc_checked of $ddc_total listed files agree ($ddc_bad bad), all of them coff."
        echo "     Moonshot is not compared, see the listing failure above."
      elif [ "$ddc_bad" -eq 0 ] && [ "$ddc_checked" -gt 0 ] && [ "$ddc_checked" -eq "$ddc_total" ]; then
        ok "DDC: $CC-built and $diverse-built coff0 agree on all $ddc_checked corpus files, $ddc_coff coff files ($ddc_coff_err of them compile-error tests, compared on message and exit code) and $ddc_ms Moonshot files"
        if [ "$kmain_total" -gt 0 ] && [ "$ddc_kmain" -eq "$kmain_total" ]; then
          echo "     corpus includes Moonshot's kernel ($ddc_kmain build variants of its"
          echo "     main source), so a payload targeting kernel code is caught where it fires."
        fi
      elif [ "$ddc_checked" -eq 0 ]; then
        bad "DDC ran but compared nothing -- corpus empty, treat as no coverage"
      else
        [ "$ddc_bad" -eq 0 ] && bad "DDC: $diverse compared $ddc_checked of $ddc_total corpus files"
        echo "     $ddc_checked of $ddc_total corpus files agree, $ddc_coff coff and $ddc_ms Moonshot."
        if [ "$ddc_bad" -eq 0 ]; then
          echo "     The rest failed on the $CC-built coff0 side, see above."
        else
          echo "     A mismatch is a compiler bug, undefined behaviour in coff0.c, or"
          echo "     the thing this script exists to find. Do not build a kernel"
          echo "     until it is explained."
        fi
      fi
    else
      warn "coff0.c does not build under $diverse"
      head -5 "$tmpdir/div_$diverse.err"
      echo "     worth fixing: coff0.c staying buildable by any C compiler is"
      echo "     what keeps DDC available at all."
    fi
  done
fi

if [ "$ddc_only" -eq 0 ]; then

echo ""
echo "=== 4. Manifest ==="
echo ""

# Recorded so tampering shows up as a text diff in review. The toolchain
# versions are part of it because a chain is only reproducible against the
# tools that produced it. A hash that changes when gcc changes is expected,
# a hash that changes when nothing changed is not.
{
  echo "# coff bootstrap manifest -- regenerate with ./verify_bootstrap.sh --write-manifest"
  echo "# Recorded so that tampering with a committed file shows up as a"
  echo "# text diff. See TRUST.md."
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed CC: $CC ($($CC --version 2>&1 | head -1))"
  echo "gcc: $(gcc --version 2>/dev/null | head -1)"
  echo "as: $(as --version 2>/dev/null | head -1)"
  echo "ld: $(ld --version 2>/dev/null | head -1)"
  [ -n "$diverse" ] && echo "diverse: $($diverse --version 2>&1 | head -1)"
  echo ""
  hashq coff0.c c0/coff.c0 c0/smed.c0 c0/sha256.c0 c0/cmp.c0 \
        lib/flyt.c0 lib/flyt_check.c0
  # No binary is hashed. None is committed, and a manifest that changed
  # whenever the tree was built would fail on every fresh clone, and a
  # check that cries wolf gets ignored. The binaries themselves are checked
  # by stage 1 above.
  # Build code is code. The xz-utils backdoor (CVE-2024-3094) did not live
  # in the library source at all. It lived in the build machinery and in
  # files that looked like test data. Both of those categories exist here
  # -- run_tests.sh decides what gets compiled here, and a "test" file in
  # this project is input to the compiler under test, so it is hashed like
  # code. ddc_entries.sh decides what the DDC compares, so it is build code
  # too. The kernel's build script and its generators are the same
  # category, but they live in the kernel's repository and are recorded
  # there. For the same reason that no binary is hashed, nothing outside
  # this repository is either, or the committed manifest would match only
  # one of the two layouts the README describes. The kernel sources still
  # go into the DDC corpus (stage 3) when they sit next to this tree.
  hashq run_tests.sh check_saved_regs.awk verify_bootstrap.sh ddc_entries.sh \
        smed_tests.sh fetch_ddc_cproc.sh lib/flyt_check.sh
  # File names here never contain spaces, so word splitting is safe.
  hashq $(find tests -type f 2>/dev/null | LC_ALL=C sort)
} > "$tmpdir/manifest"

if [ -f "$tmpdir/hash_disagree" ]; then
  bad "sha256sum and c0/sha256 disagree"
  cat "$tmpdir/hash_disagree"
elif [ "$hash_checked" -gt 0 ]; then
  ok "sha256sum and c0/sha256 agree on all $hash_checked manifest hashes"
fi

if [ "$write_manifest" -eq 1 ] && [ "$fail" -ne 0 ]; then
  # A manifest is a record of a tree that passed. Writing one after a
  # failure would bless whatever is in the tree right now, and the next
  # hash-gated build would then trust it. An in-tree coff0 that does not
  # match its source would fail stage 1, and the kernel build would still
  # build a kernel against the blessed manifest.
  bad "not writing BOOTSTRAP.sha256: $fail check(s) failed above"
elif [ "$write_manifest" -eq 1 ]; then
  cp "$tmpdir/manifest" BOOTSTRAP.sha256
  echo "wrote BOOTSTRAP.sha256"
elif [ -f BOOTSTRAP.sha256 ]; then
  # Only the hash lines are compared. The date and tool versions are
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

if [ "$tools_disagree" -gt 0 ]; then
  bad "cmp and c0/cmp disagreed $tools_disagree time(s), see AUDIT TOOLS DISAGREE above"
elif [ "$cmp_checked" -gt 0 ]; then
  ok "cmp and c0/cmp agreed on all $cmp_checked comparisons"
fi

fi  # end ddc_only guard

# The figures TRUST.md quotes, measured on every run so the document can be
# checked against them.
echo ""
echo "sizes: coff0.c $(wc -l < coff0.c) lines, c0/coff.c0 $(wc -l < c0/coff.c0) lines, c0/smed.c0 $(wc -l < c0/smed.c0) lines"

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
