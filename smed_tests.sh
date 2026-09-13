#!/usr/bin/env bash
#
# smed_tests.sh - verification for smed, the c0 assembler and linker.
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# smed exists to take GNU as and GNU ld out of this project's trusted
# computing base, which Thompson named as better hiding places than a
# compiler. That only helps if smed itself is right, and an assembler that
# is subtly wrong does not crash, it produces a program that computes the
# wrong answer. So smed needs an oracle in the same way coff.c0 needs
# coff0.c, and for now GNU as is that oracle: it is being removed from the
# build, not from the test bench. Using the thing you are replacing to check
# the replacement is the same trick coff0 plays, and it stops working the
# day GNU as is uninstalled, which is fine, because by then the differential
# corpus has been run thousands of times.
#
# Four checks, in order of how much they prove:
#
#   1. Differential corpus. Every c0 source in the tree is compiled to
#      assembly, then assembled twice, once by as+ld and once by smed. The
#      two binaries must agree on exit code and on stdout. This is the check
#      that catches encoding bugs, and it does catch them: flipping the
#      direction bit of one sub opcode (0x29 to 0x2B, same length, same
#      operands, silently wrong answer) makes 13 programs diverge.
#
#   2. The compiler builds itself through smed. smed assembles coff1.s into
#      a working compiler, and that compiler must reproduce coff1.s exactly.
#
#   3. smed fixpoint. smed assembles its own assembly into a second smed,
#      and the two must produce byte-identical output for the same input.
#
#   4. The whole test suite with no GNU tools anywhere in the pipeline.
#
# Every program is run under `timeout` because the failure mode of a bad
# assembler is frequently an infinite loop rather than a crash, which hung
# this script the first time it was written.

set -u
cd "$(dirname "$0")"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0

ok()  { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

COFF=./coff1
if [ ! -x "$COFF" ]; then echo "no ./coff1 in the tree; build it first (see Building in README.md)"; exit 1; fi

echo "Building smed..."
$COFF c0/smed.c0 "$tmp/smed.s" || { echo "smed.c0 failed to compile"; exit 1; }

# The very first smed has to come from somewhere, and that somewhere is
# still as+ld. This is the same one-time bootstrap coff0 provides for coff1,
# and it is bounded the same way: everything after this line can be done by
# smed alone, and check 3 below proves the bootstrapped smed and a
# smed-built smed behave identically.
if [ -x ./smed ]; then
  ./smed "$tmp/smed.s" "$tmp/smed" || { echo "smed could not assemble itself"; exit 1; }
  chmod +x "$tmp/smed"
else
  as "$tmp/smed.s" -o "$tmp/smed.o" && ld "$tmp/smed.o" -o "$tmp/smed" \
    || { echo "one-time smed bootstrap via as/ld failed"; exit 1; }
fi
SMED="$tmp/smed"

echo ""
echo "1. Differential corpus: smed vs as+ld over every c0 source"
agree=0
for src in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/coff.c0 c0/smed.c0; do
  n=$(basename "$src" .c0)
  # Sources coff rejects on purpose (the compile-error tests) produce no
  # assembly on either side and are not comparable.
  $COFF "$src" "$tmp/t.s" 2>/dev/null || continue
  as "$tmp/t.s" -o "$tmp/t.o" 2>/dev/null && ld "$tmp/t.o" -o "$tmp/t_gnu" 2>/dev/null || continue
  if ! "$SMED" "$tmp/t.s" "$tmp/t_smed" 2>"$tmp/e"; then
    bad "smed rejected $n: $(cat "$tmp/e")"
    continue
  fi
  chmod +x "$tmp/t_smed"
  timeout 10 "$tmp/t_gnu"  >"$tmp/o_gnu"  2>/dev/null; a=$?
  timeout 10 "$tmp/t_smed" >"$tmp/o_smed" 2>/dev/null; b=$?
  if [ "$a" = "$b" ] && cmp -s "$tmp/o_gnu" "$tmp/o_smed"; then
    agree=$((agree + 1))
  else
    bad "$n behaves differently (as+ld exit $a, smed exit $b)"
  fi
done
[ "$fail" -eq 0 ] && ok "all $agree comparable programs behave identically under as+ld and smed"

echo ""
echo "2. The compiler builds itself through smed"
if "$SMED" coff1.s "$tmp/coff1_smed" 2>/dev/null; then
  chmod +x "$tmp/coff1_smed"
  if "$tmp/coff1_smed" c0/coff.c0 "$tmp/self.s" 2>/dev/null && cmp -s coff1.s "$tmp/self.s"; then
    ok "smed-built coff1 reproduces coff1.s byte-for-byte"
  else
    bad "smed-built coff1 does not reproduce coff1.s"
  fi
else
  bad "smed could not assemble coff1.s"
fi

echo ""
echo "3. smed fixpoint"
if "$SMED" "$tmp/smed.s" "$tmp/smed2" 2>/dev/null; then
  chmod +x "$tmp/smed2"
  "$SMED"       coff1.s "$tmp/c_a" 2>/dev/null
  "$tmp/smed2"  coff1.s "$tmp/c_b" 2>/dev/null
  if cmp -s "$tmp/c_a" "$tmp/c_b"; then
    ok "smed and the smed it built emit byte-identical output"
  else
    bad "smed is not a fixpoint"
  fi
else
  bad "smed could not assemble itself"
fi

echo ""
echo "4. Whole suite with no GNU tools in the pipeline"
CC="$tmp/coff1_smed"
if [ -x "$CC" ]; then
  p2=0; f2=0
  for src in tests/*.c0; do
    n=$(basename "$src" .c0)
    exp=$(head -n1 "$src" | sed -n 's|^// expect: ||p')
    args=$(sed -n '2p' "$src" | sed -n 's|^// args: ||p')
    if [ "$exp" = "compile-error" ]; then
      if "$CC" "$src" "$tmp/x.s" 2>/dev/null; then f2=$((f2+1)); else p2=$((p2+1)); fi
      continue
    fi
    "$CC" "$src" "$tmp/x.s" 2>/dev/null || { echo "  FAIL $n (compile)"; f2=$((f2+1)); continue; }
    [ "$exp" = "no-exec" ] && { p2=$((p2+1)); continue; }
    "$SMED" "$tmp/x.s" "$tmp/x" 2>/dev/null || { echo "  FAIL $n (smed)"; f2=$((f2+1)); continue; }
    chmod +x "$tmp/x"
    timeout 10 "$tmp/x" $args >"$tmp/x.out" 2>/dev/null; got=$?
    if [ "$got" != "$exp" ]; then echo "  FAIL $n (exit $got, expected $exp)"; f2=$((f2+1)); continue; fi
    if [ -f "tests/$n.expected_stdout" ] && ! cmp -s "tests/$n.expected_stdout" "$tmp/x.out"; then
      echo "  FAIL $n (stdout)"; f2=$((f2+1)); continue
    fi
    p2=$((p2+1))
  done
  if [ "$f2" -eq 0 ]; then ok "$p2 tests pass with no gcc, no as and no ld"; else bad "$f2 tests failed in the binutils-free pipeline"; fi
else
  bad "no smed-built compiler to test with"
fi

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
