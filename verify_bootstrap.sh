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
#   1. Committed artifacts match a fresh rebuild. In my own tree coff0,
#      coff1.s, coff1, smed.s and smed are checked in and the kernel build
#      runs those binaries directly. A tampered binary committed once would
#      compile or assemble every kernel from then on, and a binary diff
#      inside a commit is not something a human reads. This stage rebuilds
#      the whole chain from source and compares bytes. Since smed took over
#      assembling and linking the kernel, smed is on that path too, so it
#      is rebuilt and compared the same way. coff1 must reproduce smed.s
#      from c0/smed.c0, and smed must reproduce itself from that assembly.
#      This repository commits no binaries, so here the comparisons report
#      SKIP unless you have built into the tree; the rebuilds themselves
#      still have to succeed.
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

echo "=== 1. Committed artifacts vs a fresh rebuild from source ==="
echo ""

# coff0 is rebuilt into the tmpdir rather than over the committed one. The
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
    ok "committed coff1 == freshly bootstrapped coff1 (the binary Moonshot runs)"
  else
    bad "committed coff1 differs from a fresh bootstrap"
    echo "     this is the binary the kernel is compiled with."
    echo "     do not build a kernel until this is explained."
  fi
else
  warn "no committed coff1 to compare against"
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
# a committed binary that moonshot/build.sh runs directly. Its assembly must
# be what coff1 emits from c0/smed.c0, and the binary must be what smed
# produces from that assembly -- the committed smed assembling its own
# source is a fixpoint check, and only the very first smed is produced any
# other way (from as+ld).
if [ -f "$tmpdir/coff1" ]; then
  if "$tmpdir/coff1" c0/smed.c0 "$tmpdir/smed.s" 2>/dev/null; then
    if [ -f smed.s ]; then
      if cmpq "$tmpdir/smed.s" smed.s; then
        ok "committed smed.s == coff1's output for c0/smed.c0"
      else
        bad "committed smed.s differs from what coff1 emits for c0/smed.c0"
      fi
    else
      warn "no committed smed.s to compare against"
    fi
    if [ -x smed ]; then
      if ./smed "$tmpdir/smed.s" "$tmpdir/smed_self" 2>/dev/null \
         && cmpq "$tmpdir/smed_self" smed; then
        ok "committed smed == smed assembling its own source (the binary that links the kernel)"
      else
        bad "committed smed is not what smed produces from its own source"
        echo "     this is the binary the kernel is assembled and linked with."
        echo "     do not build a kernel until this is explained."
      fi
    else
      warn "no committed smed to compare against"
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
  # Must be $CC, not a hardcoded gcc. With --ddc-only nothing has built
  # the seed side yet, and building it with the same compiler that is about
  # to play the "diverse" role compares a compiler against itself, which
  # lets a compromised gcc pass this check while backdooring the kernel.
  if [ ! -f "$tmpdir/coff0" ]; then
    $CC -Wall -Wextra -o "$tmpdir/coff0" coff0.c 2>/dev/null
  fi

  # The corpus. Everything that matters is in here, because a payload is
  # only caught on inputs where it fires. Moonshot's kernel sources are
  # included deliberately, being the code actually worth backdooring,
  # and leaving them out is what lets the simulated attack through.
  corpus=""
  for f in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/coff.c0 c0/smed.c0 c0/sha256.c0 c0/cmp.c0; do
    [ -f "$f" ] && corpus="$corpus $f"
  done
  ms=../moonshot
  if [ -d "$ms" ]; then
    # The ring-3 programs the kernel carries are kernel content too, and so
    # is the tool that packs the tests (bake.c0 sits in the kernel
    # directory and is picked up by the first glob). Gnista's sources live
    # in ../gnista in the dev tree and in programs/ in a release.
    for f in "$ms"/*.c0 "$ms"/programs/*.c0 ../gnista/*.c0; do
      case "$f" in *_data.c0|*.bak*) continue ;; esac
      [ -f "$f" ] && corpus="$corpus $f"
    done
  else
    warn "moonshot sources not found at $ms -- kernel code is NOT in the DDC corpus"
  fi

  for diverse in $diverse_list; do
    if [ "$diverse" = cproc ] && [ -f "$HOME/.local/opt/c0-ddc/VERSIONS" ]; then
      echo "diverse compiler: cproc ($(tr '\n' ' ' < "$HOME/.local/opt/c0-ddc/VERSIONS"))"
    else
      echo "diverse compiler: $diverse ($($diverse --version 2>&1 | head -1))"
    fi
    rm -rf "$tmpdir/div"; mkdir -p "$tmpdir/div"
    if build_coff0_with "$diverse" "$tmpdir/coff0_div" 2>"$tmpdir/div.err"; then
      ok "coff0.c builds under $diverse"
      ddc_checked=0
      ddc_bad=0
      for f in $corpus; do
        name=$(basename "$f" .c0)
        # Only inputs both binaries accept are comparable, since a file coff0
        # rejects produces no output to diff on either side.
        if ! "$tmpdir/coff0" "$f" "$tmpdir/a_$name.s" 2>/dev/null; then continue; fi
        if ! "$tmpdir/coff0_div" "$f" "$tmpdir/div/b_$name.s" 2>/dev/null; then
          bad "DDC: $diverse-built coff0 rejected $f which $CC-built coff0 accepted"
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
        echo "     corpus includes Moonshot's kernel sources, so a payload"
        echo "     targeting kernel code is caught where it fires."
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
  echo "# Recorded so that tampering with a committed binary shows up as a"
  echo "# text diff. See TRUST.md."
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "seed CC: $CC ($($CC --version 2>&1 | head -1))"
  echo "gcc: $(gcc --version 2>/dev/null | head -1)"
  echo "as: $(as --version 2>/dev/null | head -1)"
  echo "ld: $(ld --version 2>/dev/null | head -1)"
  [ -n "$diverse" ] && echo "diverse: $($diverse --version 2>&1 | head -1)"
  echo ""
  hashq coff0.c c0/coff.c0 c0/lex.c0 c0/parse.c0 c0/smed.c0 c0/sha256.c0 c0/cmp.c0
  # In my own tree the bootstrap binaries (coff0, coff1.s, coff1, smed.s,
  # smed) are committed and hashed here too, since they sit on the critical
  # path. This repository commits none of them, so they are left out. A
  # manifest that changes depending on whether you have built in the tree
  # would fail for every fresh clone, and a check that cries wolf gets
  # ignored.
  # Build code is code. The xz-utils backdoor (CVE-2024-3094) did not live
  # in the library source at all. It lived in the build machinery and in
  # files that looked like test data. Both of those categories exist here
  # -- run_tests.sh decides what gets compiled here, and a "test" file in
  # this project is input to the compiler under test, so it is hashed like
  # code. The kernel's build script and its generators are the same
  # category, but they live in the kernel's repository and are recorded
  # there. For the same reason no binary is hashed above, nothing outside
  # this repository is either, or the committed manifest would match only
  # one of the two layouts the README describes. The kernel sources still
  # go into the DDC corpus (stage 3) when they sit next to this tree.
  hashq run_tests.sh verify_bootstrap.sh smed_tests.sh fetch_ddc_cproc.sh
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
  # hash-gated build would then trust it. A committed coff0 that does not
  # match its source would fail stage 1, and moonshot/build.sh would still
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

echo ""
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
