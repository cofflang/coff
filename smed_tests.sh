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
#   5. The kernel, when my OS is checked out at ../moonshot. The boot code
#      and the ring-3 trampolines are hand-written assembly that no c0
#      program exercises, so the corpus above says nothing about lgdt,
#      iretq, control registers or the far jump into long mode. The kernel
#      is built twice from the very same sources, once by as+ld and once by
#      smed, and the two are compared where they can be. The linker half:
#      the fixed hand-off slots, the multiboot header, the entry point and
#      every linker-script symbol must land at the same addresses. The
#      assembler half: the hand-written code must disassemble to the same
#      instruction sequence. Bytes are not compared, because the encodings
#      differ by design -- smed never picks a short form. Only addresses
#      are normalised, and nothing else is: small immediates such as the
#      far jump's selector, MSR bits and shift counts have to match
#      exactly, because an earlier normalisation that erased every
#      immediate let a wrong selector through. Whether the kernel then
#      boots is its own test suite's job; six deliberate encoding mutations
#      were each caught there when this check was written (2026-09-17).
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

echo "5. The kernel: as+ld and smed agree on layout and on boot.s's instructions"
K=../moonshot
if [ -f "$K/boot.s" ] && [ -f "$K/linker.ld" ] && [ -f "$K/ring3_handlers.s" ] && [ -x "$K/build.sh" ]; then
  # The kernel's own C-like source is compiled fresh here, the way its
  # build script does it, rather than trusting whatever assembly is lying
  # in the tree.
  if "$COFF" "$K/kmain.c0" "$tmp/kmain.s" 2>/dev/null; then
    sed -i '1i .global main\n.global int_dispatch\n.global paging_verify\n.global kernel_post_init\n.global sched_start\n.global sys_dispatch' "$tmp/kmain.s"
    kfail=0
    (cd "$K" && as boot.s -o "$tmp/boot.o" && as "$tmp/kmain.s" -o "$tmp/kmain.o" \
      && as ring3_handlers.s -o "$tmp/r3.o" \
      && ld -T linker.ld -o "$tmp/k_gnu" "$tmp/boot.o" "$tmp/kmain.o" "$tmp/r3.o") 2>/dev/null \
      || { bad "as+ld could not build the kernel (oracle unavailable)"; kfail=1; }
    if [ "$kfail" -eq 0 ]; then
      (cd "$K" && "$SMED" -T linker.ld -m "$tmp/k.map" boot.s "$tmp/kmain.s" ring3_handlers.s "$tmp/k_smed") 2>"$tmp/e" \
        || { bad "smed rejected the kernel: $(cat "$tmp/e")"; kfail=1; }
    fi
    if [ "$kfail" -eq 0 ]; then
      # Linker half. Everything before .text is fixed by the linker
      # script and
      # must sit at identical addresses; everything after .text shifts,
      # because smed's wide encodings make .text longer, so there the
      # comparison is of distances: between the script's reservations
      # (their sizes), between boot.s's .bss objects (their sizes and
      # .balign padding), and within .data. Alignment is checked on smed's
      # image directly, since ld's is not the point.
      lfail=0
      gsym() { nm "$tmp/k_gnu" | awk -v s="$1" '$3==s{print "0x" $1; exit}'; }
      ssym() { awk -v s="$1" '$2==s{print "0x" $1; exit}' "$tmp/k.map"; }
      [ "$(gsym _boot_start)" = "$(ssym _boot_start)" ] || { echo "  _boot_start: as+ld $(gsym _boot_start), smed $(ssym _boot_start)"; lfail=1; }
      for pair in kscratch_start:idt_start idt_start:dispatch_start dispatch_start:_kernel_end \
                  p4_table:p3_table p3_table:p2_table p2_table:stack_bottom stack_bottom:stack_top \
                  stack_top:tss64 tss64:df_stack_bottom df_stack_bottom:df_stack_top \
                  gdt64:gdt64_tss gdt64_tss:gdt64_end gdt64_end:gdt64_pointer; do
        x=${pair%%:*}; y=${pair##*:}
        dg=$(( $(gsym "$y") - $(gsym "$x") )); ds=$(( $(ssym "$y") - $(ssym "$x") ))
        [ "$dg" -eq "$ds" ] || { echo "  $y - $x: as+ld $dg, smed $ds"; lfail=1; }
      done
      for a4096 in p4_table p3_table p2_table; do
        [ $(( $(ssym "$a4096") % 4096 )) -eq 0 ] || { echo "  $a4096 is not page aligned in smed's image"; lfail=1; }
      done
      for a16 in stack_bottom tss64 df_stack_bottom gdt64; do
        [ $(( $(ssym "$a16") % 16 )) -eq 0 ] || { echo "  $a16 is not 16-byte aligned in smed's image"; lfail=1; }
      done
      e_gnu=$(readelf -h "$tmp/k_gnu" | awk '/Entry point/{print $NF}')
      e_smed=$(readelf -h "$tmp/k_smed" | awk '/Entry point/{print $NF}')
      [ "$e_gnu" = "$e_smed" ] || { echo "  entry: as+ld $e_gnu, smed $e_smed"; lfail=1; }
      # The multiboot header, at the same file offset in both.
      mb=$(nm "$tmp/k_gnu" | awk '$3=="_boot_start"{print $1}')
      off=$(printf '%d' "0x$(readelf -S "$tmp/k_gnu" | awk '$2==".multiboot"{print $5}')")
      [ "$off" -gt 0 ] && dd if="$tmp/k_gnu" bs=1 skip="$off" count=48 2>/dev/null > "$tmp/mb_gnu" \
        && dd if="$tmp/k_smed" bs=1 skip="$off" count=48 2>/dev/null > "$tmp/mb_smed" \
        && cmp -s "$tmp/mb_gnu" "$tmp/mb_smed" || { echo "  multiboot header differs or moved"; lfail=1; }
      if [ "$lfail" -eq 0 ]; then ok "kernel layout: entry, multiboot header, reservation sizes and alignments agree"; else bad "kernel layout differs between as+ld and smed"; fi

      # Assembler half: the hand-written code, disassembled from both
      # images and compared as instruction sequences. boot.s's 32-bit
      # entry runs up to long_mode_start; its 64-bit part up to _start
      # (where coff's code begins); ring3_handlers.s is .ring3_text.
      dis() { # file vma-of-file-offset-0 start stop mode
        objdump -D -b binary -m "$5" -M intel --adjust-vma="$2" --start-address="$3" --stop-address="$4" "$1" \
          | grep -E '^ +[0-9a-f]+:' | sed -E 's/^ +[0-9a-f]+:\t([0-9a-f]{2} )+\s*//' \
          | sed -E 's/<[^>]*>//g; s/\[rip\+0x[0-9a-f]+\]/[N]/g; s/[[:space:]]*#[[:space:]]*(0x)?[0-9a-f]+[[:space:]]*$//; s/ds:0x[0-9a-f]+/[N]/g; s/\+0x0\]/]/g; s/0x[0-9a-f]{5,}/N/g; s/\b[0-9a-f]{6,}\b/N/g; s/[[:space:]]+/ /g; s/ $//' \
          | grep -v '^$'
      }
      # as+ld's image has its own file offsets; take the load segment's
      # offset and address from its program header.
      goff=$(readelf -l "$tmp/k_gnu" | awk '/LOAD/{print $2; exit}')
      gva=$(readelf -l "$tmp/k_gnu" | awk '/LOAD/{print $3; exit}')
      gadj=$(printf '0x%x' $((gva - goff)))
      soff=$(readelf -l "$tmp/k_smed" | awk '/LOAD/{print $2; exit}')
      sva=$(readelf -l "$tmp/k_smed" | awk '/LOAD/{print $3; exit}')
      sadj=$(printf '0x%x' $((sva - soff)))
      afail=0
      dis "$tmp/k_gnu"  "$gadj" "$(gsym _boot_start)" "$(gsym long_mode_start)" i386 > "$tmp/d32_gnu"
      dis "$tmp/k_smed" "$sadj" "$(ssym _boot_start)" "$(ssym long_mode_start)" i386 > "$tmp/d32_smed"
      cmp -s "$tmp/d32_gnu" "$tmp/d32_smed" || { echo "  32-bit entry code differs:"; diff "$tmp/d32_gnu" "$tmp/d32_smed" | head -10; afail=1; }
      dis "$tmp/k_gnu"  "$gadj" "$(gsym long_mode_start)" "$(gsym _start)" i386:x86-64 > "$tmp/d64_gnu"
      dis "$tmp/k_smed" "$sadj" "$(ssym long_mode_start)" "$(ssym _start)" i386:x86-64 > "$tmp/d64_smed"
      cmp -s "$tmp/d64_gnu" "$tmp/d64_smed" || { echo "  64-bit boot code differs:"; diff "$tmp/d64_gnu" "$tmp/d64_smed" | head -10; afail=1; }
      # ring3_handlers.s: from syscall_entry to the end of enter_ring3's
      # iretq. The end is not a symbol, so both are cut at the first iretq
      # after enter_ring3 by disassembling generously and trimming.
      dis "$tmp/k_gnu"  "$gadj" "$(gsym syscall_entry)" "$(printf '0x%x' $(($(gsym syscall_entry) + 512)))" i386:x86-64 | awk '{print} /^iretq/{exit}' > "$tmp/r3_gnu"
      dis "$tmp/k_smed" "$sadj" "$(ssym syscall_entry)" "$(printf '0x%x' $(($(ssym syscall_entry) + 512)))" i386:x86-64 | awk '{print} /^iretq/{exit}' > "$tmp/r3_smed"
      cmp -s "$tmp/r3_gnu" "$tmp/r3_smed" || { echo "  ring-3 trampolines differ:"; diff "$tmp/r3_gnu" "$tmp/r3_smed" | head -10; afail=1; }
      n32=$(wc -l < "$tmp/d32_gnu"); n64=$(wc -l < "$tmp/d64_gnu"); nr3=$(wc -l < "$tmp/r3_gnu")
      if [ "$afail" -eq 0 ] && [ "$n32" -gt 10 ] && [ "$n64" -gt 10 ] && [ "$nr3" -gt 10 ]; then
        ok "kernel assembly: as and smed emit the same $((n32 + n64 + nr3)) hand-written instructions"
      else
        bad "kernel assembly differs between as and smed (or the disassembly windows are empty)"
      fi
    fi
  else
    bad "coff1 could not compile the kernel source"
  fi
else
  echo "SKIP no ../moonshot checkout next to this repository; kernel check not run"
fi

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
