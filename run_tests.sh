#!/usr/bin/env bash
# run_tests.sh - build coff0 and run the whole coff test suite
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# This builds coff0, then runs every tests/*.c0 program through the
# full pipeline (coff0 -> as -> ld -> run) and checks its exit code
# against the `// expect: N` (or `// expect: compile-error`) comment on
# the file's first line. If tests/<name>.expected_stdout exists, the
# program's actual stdout is also compared against it, byte for byte,
# and for a compile-error test tests/<name>.expected_error is compared
# against coff0's message, file and line included.
#
# `// expect: no-exec` is a third variant, where the program must compile but
# is never assembled, linked or run. This is for builtins like outb/inb
# whose instructions fault in normal Linux userspace (no ring 0, no
# IOPL). Their codegen is still covered by the differential loops below,
# which never execute the compiled binary.
#
# After the unit tests come the differential stages (each c0-written
# stage diffed against coff0 over the whole corpus, error messages
# included), checks for -I, embed, large sources and the audit's own
# sha256 and cmp, and finally the bootstrap fixpoint checks, coff.c0
# compiling itself three generations deep, all byte-identical.
#
# What this suite does NOT establish is that the toolchain is free of a
# Ken Thompson "trusting trust" payload. The bootstrap fixpoint below
# (stage1 == stage2 == stage3) is the property such a payload is designed
# to preserve, not evidence against one. See ./verify_bootstrap.sh and
# TRUST.md for the checks that actually bear on that question.
set -u

cd "$(dirname "$0")"

# The seed compiler is selectable so the whole suite can be run without gcc
# anywhere in it, and `CC=tcc ./run_tests.sh` produces a byte-identical
# coff1 and a byte-identical Moonshot kernel. That matters
# because tcc is reachable from the 357-byte hex0 seed through the
# stage0/M2-Planet/Mes chain, and gcc is not reachable from anything.
# Same selection as moonshot/build.sh and verify_bootstrap.sh, tcc when
# present. They must agree, because this script rewrites ./coff0 in the
# tree -- if they disagreed the seed binary would flip on every run and the
# manifest would break constantly.
if [ -z "${CC:-}" ]; then
  if command -v tcc >/dev/null 2>&1; then CC=tcc; else CC=gcc; fi
fi
echo "Building coff0 (CC=$CC)..."
$CC -Wall -Wextra -o coff0 coff0.c || { echo "coff0 failed to build"; exit 1; }

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

pass=0
fail=0

for src in tests/*.c0; do
  name=$(basename "$src" .c0)
  expect=$(head -n1 "$src" | sed -n 's/^\/\/ expect: //p')
  # An optional second-line `// args: a b c` comment gives extra argv
  # entries passed to the compiled binary, for tests exercising argc()/argv(i).
  args=$(sed -n '2p' "$src" | sed -n 's/^\/\/ args: //p')

  if [ "$expect" = "compile-error" ]; then
    # tests/<name>.expected_error, when present, pins the exact message,
    # file and line included. The diagdiff section below only proves the
    # two compilers agree with each other, which they could do while both
    # pointing at the wrong line.
    if ./coff0 "$src" "$tmpdir/$name.s" 2>"$tmpdir/$name.err"; then
      echo "FAIL $name (expected a compile error, but it compiled)"
      fail=$((fail + 1))
    elif [ -f "tests/$name.expected_error" ] && ! diff -q "tests/$name.expected_error" "$tmpdir/$name.err" >/dev/null; then
      echo "FAIL $name (compile error, but not the message in tests/$name.expected_error)"
      diff "tests/$name.expected_error" "$tmpdir/$name.err"
      fail=$((fail + 1))
    else
      echo "PASS $name (compile error, as expected)"
      pass=$((pass + 1))
    fi
    continue
  fi

  if [ "$expect" = "no-exec" ]; then
    if ./coff0 "$src" "$tmpdir/$name.s" 2>"$tmpdir/$name.err"; then
      echo "PASS $name (compiled, not executed -- see run_tests.sh header)"
      pass=$((pass + 1))
    else
      echo "FAIL $name (expected to compile, but it errored)"
      cat "$tmpdir/$name.err"
      fail=$((fail + 1))
    fi
    continue
  fi

  if ! ./coff0 "$src" "$tmpdir/$name.s" 2>"$tmpdir/$name.err"; then
    echo "FAIL $name (compile error, expected exit $expect)"
    cat "$tmpdir/$name.err"
    fail=$((fail + 1))
    continue
  fi

  as "$tmpdir/$name.s" -o "$tmpdir/$name.o" || { echo "FAIL $name (assembler error)"; fail=$((fail + 1)); continue; }
  ld "$tmpdir/$name.o" -o "$tmpdir/$name.bin" || { echo "FAIL $name (linker error)"; fail=$((fail + 1)); continue; }

  "$tmpdir/$name.bin" $args >"$tmpdir/$name.out"
  actual=$?

  if [ "$actual" != "$expect" ]; then
    echo "FAIL $name (expected exit $expect, got $actual)"
    fail=$((fail + 1))
    continue
  fi

  expected_stdout="tests/$name.expected_stdout"
  if [ -f "$expected_stdout" ] && ! diff -q "$expected_stdout" "$tmpdir/$name.out" >/dev/null; then
    echo "FAIL $name (exit code ok, but stdout did not match $expected_stdout)"
    diff "$expected_stdout" "$tmpdir/$name.out"
    fail=$((fail + 1))
    continue
  fi

  echo "PASS $name (exit $actual)"
  pass=$((pass + 1))
done

echo ""
echo "Differential lexer test: c0/lex.c0 (compiled by coff0) vs coff0 -t"
if ./coff0 c0/lex.c0 "$tmpdir/lex.s" \
   && as "$tmpdir/lex.s" -o "$tmpdir/lex.o" \
   && ld "$tmpdir/lex.o" -o "$tmpdir/lex"; then
  for src in tests/*.c0 c0/lex.c0 c0/sha256.c0 c0/cmp.c0; do
    name=$(basename "$src" .c0)
    # Files coff0 itself refuses to lex are skipped. On error coff0 -t prints
    # nothing while lex.c0 streams tokens until the bad byte, so the outputs
    # are incomparable by design.
    if ./coff0 -t "$src" > "$tmpdir/ref_tokens" 2>/dev/null; then
      # Passing the real path (not piping via stdin) is what lets lex.c0
      # resolve include directives -- a relative include path has nothing
      # to resolve against from piped bytes alone. See c0/lex.c0's main().
      "$tmpdir/lex" "$src" > "$tmpdir/c0_tokens"
      if diff -q "$tmpdir/ref_tokens" "$tmpdir/c0_tokens" >/dev/null; then
        echo "PASS lexdiff $name"
        pass=$((pass + 1))
      else
        echo "FAIL lexdiff $name"
        diff "$tmpdir/ref_tokens" "$tmpdir/c0_tokens" | head -10
        fail=$((fail + 1))
      fi
    fi
  done
else
  echo "FAIL building c0/lex.c0"
  fail=$((fail + 1))
fi

echo ""
echo "Differential parser test: c0/parse.c0 (compiled by coff0) vs coff0 -a"
if ./coff0 c0/parse.c0 "$tmpdir/parse.s" \
   && as "$tmpdir/parse.s" -o "$tmpdir/parse.o" \
   && ld "$tmpdir/parse.o" -o "$tmpdir/parse"; then
  for src in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/sha256.c0 c0/cmp.c0; do
    name=$(basename "$src" .c0)
    if ./coff0 -a "$src" > "$tmpdir/ref_ast" 2>/dev/null; then
      # Real path, not piped stdin -- see lexdiff's identical note above.
      "$tmpdir/parse" "$src" > "$tmpdir/c0_ast" 2>/dev/null
      if diff -q "$tmpdir/ref_ast" "$tmpdir/c0_ast" >/dev/null; then
        echo "PASS astdiff $name"
        pass=$((pass + 1))
      else
        echo "FAIL astdiff $name"
        diff "$tmpdir/ref_ast" "$tmpdir/c0_ast" | head -10
        fail=$((fail + 1))
      fi
    fi
  done
else
  echo "FAIL building c0/parse.c0"
  fail=$((fail + 1))
fi

echo ""
echo "Differential codegen test: c0/coff.c0 (compiled by coff0) vs coff0's own codegen"
if ./coff0 c0/coff.c0 "$tmpdir/coff1.s" \
   && as "$tmpdir/coff1.s" -o "$tmpdir/coff1.o" \
   && ld "$tmpdir/coff1.o" -o "$tmpdir/coff1"; then
  for src in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/coff.c0 c0/sha256.c0 c0/cmp.c0; do
    name=$(basename "$src" .c0)
    # Only compares files that actually compile. coff0's own resolve-time
    # errors (undeclared var, wrong arity, etc.) are excluded here since
    # neither side produces assembly to diff in that case. Uses coff1's real
    # file-based CLI (`coff1 src out.s`), the same interface coff0 itself has.
    if ./coff0 "$src" "$tmpdir/ref_$name.s" 2>/dev/null; then
      "$tmpdir/coff1" "$src" "$tmpdir/c0_$name.s" 2>/dev/null
      if diff -q "$tmpdir/ref_$name.s" "$tmpdir/c0_$name.s" >/dev/null; then
        echo "PASS codegendiff $name"
        pass=$((pass + 1))
      else
        echo "FAIL codegendiff $name"
        diff "$tmpdir/ref_$name.s" "$tmpdir/c0_$name.s" | head -10
        fail=$((fail + 1))
      fi
    fi
  done

  # The same diff for the Moonshot ring-3 target. `--elf` is the same
  # assembly backend with Moonshot's syscall numbers, present in coff0.c as
  # well, so every ring-3 program the kernel ships is compared the same way,
  # the test programs, the probes, and Gnista, whichever of them are checked
  # out beside this repository. The
  # sources include files relative to their own directory, hence the cd.
  # They include the ring-3 library (moonshot/programs/lib/moon.c0) by name,
  # so both compilers get its directory with -I, the way Moonshot's
  # generators pass it.
  elflib=$(cd .. && pwd)/moonshot/programs/lib
  for src in ../moonshot/elf_tests/*.c0 ../moonshot/programs/*.c0 \
             ../moonshot/programs/lira/lira.c0 ../utsikt/utsikt.c0 \
             ../gnista/gnista.c0 ../gnista/infecteria.c0 ../gnista/fps_probe.c0; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c0)
    dir=$(dirname "$src")
    if (cd "$dir" && "$OLDPWD/coff0" --elf -I "$elflib" "$(basename "$src")" "$tmpdir/eref_$name.s") 2>/dev/null; then
      (cd "$dir" && "$tmpdir/coff1" --elf -I "$elflib" "$(basename "$src")" "$tmpdir/ec0_$name.s") 2>/dev/null
      if diff -q "$tmpdir/eref_$name.s" "$tmpdir/ec0_$name.s" >/dev/null; then
        echo "PASS codegendiff --elf $name"
        pass=$((pass + 1))
      else
        echo "FAIL codegendiff --elf $name"
        diff "$tmpdir/eref_$name.s" "$tmpdir/ec0_$name.s" | head -10
        fail=$((fail + 1))
      fi
    else
      echo "FAIL codegendiff --elf $name (coff0 --elf rejected it)"
      fail=$((fail + 1))
    fi
  done

  echo ""
  echo "Differential diagnostics: coff1's error message vs coff0's, every compile-error test"
  # Both compilers must reject the file, coff0 must actually say something,
  # and the two messages must be identical once each compiler's own name is
  # taken off the front.
  for src in tests/*.c0; do
    [ "$(head -n1 "$src")" = "// expect: compile-error" ] || continue
    name=$(basename "$src" .c0)
    ref=$(./coff0 "$src" "$tmpdir/dref.s" 2>&1 >/dev/null | sed 's/^coff0: /coff1: /')
    got=$("$tmpdir/coff1" "$src" "$tmpdir/dc0.s" 2>&1 >/dev/null)
    rc=$?
    if [ "$rc" -ne 0 ] && [ -n "$ref" ] && [ "$ref" = "$got" ]; then
      echo "PASS diagdiff $name"
      pass=$((pass + 1))
    else
      echo "FAIL diagdiff $name (coff1 exit $rc)"
      echo "  coff0: $ref"
      echo "  coff1: $got"
      fail=$((fail + 1))
    fi
  done

  echo ""
  echo "Large source: a file over coff1's first 1MB read buffer"
  # A source file over 1MB must compile whole. A reader with a fixed buffer
  # would silently lose the end of the file, and with it main. Built here
  # rather than committed, since the point is only its size.
  for i in $(seq 1 14000); do
    echo "// padding, to take this file past one megabyte"
  done > "$tmpdir/big.c0"
  echo "int main() { return 77; }" >> "$tmpdir/big.c0"
  if ./coff0 "$tmpdir/big.c0" "$tmpdir/big0.s" 2>/dev/null \
     && "$tmpdir/coff1" "$tmpdir/big.c0" "$tmpdir/big1.s" 2>/dev/null \
     && cmp -s "$tmpdir/big0.s" "$tmpdir/big1.s" \
     && as "$tmpdir/big1.s" -o "$tmpdir/big.o" && ld "$tmpdir/big.o" -o "$tmpdir/big"; then
    "$tmpdir/big"; rc=$?
    if [ "$rc" -eq 77 ]; then
      echo "PASS a $(wc -c < "$tmpdir/big.c0")-byte source compiles whole in both compilers"
      pass=$((pass + 1))
    else
      echo "FAIL the large source ran with exit $rc, expected 77"
      fail=$((fail + 1))
    fi
  else
    echo "FAIL a source over 1MB did not compile the same in both compilers"
    fail=$((fail + 1))
  fi
  echo ""

  echo "Generated code never touches rbx"
  # rbx is a register the System V ABI has a function preserve, and nothing
  # coff emits saves it, so nothing coff emits may use it.
  rbx_hits=0
  for src in tests/*.c0; do
    if ./coff0 "$src" "$tmpdir/rbx.s" 2>/dev/null && grep -q "rbx" "$tmpdir/rbx.s"; then
      echo "  rbx in the output for $src"; rbx_hits=$((rbx_hits + 1))
    fi
  done
  if [ "$rbx_hits" -eq 0 ]; then
    echo "PASS no compiled test program uses rbx"
    pass=$((pass + 1))
  else
    echo "FAIL $rbx_hits compiled test program(s) use rbx"
    fail=$((fail + 1))
  fi
  echo ""

  echo "embed: a quote in the source file's directory is refused"
  # The .incbin path is the embed's path joined with the source file's own
  # directory, all of it between quotes in the assembly. Both compilers must
  # refuse a directory with a quote in its name rather than emit broken
  # assembly, with the same message.
  qdir="$tmpdir/we\"ird"
  mkdir -p "$qdir"
  printf 'embed blob "x.bin";\nint main() { return blob_size; }\n' > "$qdir/q.c0"
  : > "$qdir/x.bin"
  q0=$(./coff0 "$qdir/q.c0" "$tmpdir/q0.s" 2>&1 >/dev/null | sed 's/^coff0: /coff1: /')
  q1=$("$tmpdir/coff1" "$qdir/q.c0" "$tmpdir/q1.s" 2>&1 >/dev/null)
  if echo "$q0" | grep -q "may not contain quotes" && [ "$q0" = "$q1" ]; then
    echo "PASS both compilers refuse it with the same message"
    pass=$((pass + 1))
  else
    echo "FAIL coff0 said '$q0', coff1 said '$q1'"
    fail=$((fail + 1))
  fi
  echo ""

  echo "Include search path: -I, in both compilers"
  # An include that is not next to the including file is looked for in each
  # -I directory. tests/include_search/main.c0 includes searchlib.c0, which
  # only exists in tests/include_search/lib. Both compilers must find it with
  # -I and produce the same assembly, the program must return 42, and
  # without -I both must refuse it.
  isrc=tests/include_search/main.c0
  ilib=tests/include_search/lib
  if ./coff0 -I "$ilib" "$isrc" "$tmpdir/inc0.s" 2>/dev/null \
     && "$tmpdir/coff1" "$isrc" "$tmpdir/inc1.s" -I "$ilib" 2>/dev/null \
     && cmp -s "$tmpdir/inc0.s" "$tmpdir/inc1.s" \
     && as "$tmpdir/inc0.s" -o "$tmpdir/inc0.o" && ld "$tmpdir/inc0.o" -o "$tmpdir/inc0"; then
    "$tmpdir/inc0"; rc=$?
    if [ "$rc" -eq 42 ]; then
      echo "PASS -I finds the library, both compilers agree (exit 42)"
      pass=$((pass + 1))
    else
      echo "FAIL -I program exited $rc, expected 42"
      fail=$((fail + 1))
    fi
  else
    echo "FAIL -I: a compiler did not find the library, or the two disagree"
    fail=$((fail + 1))
  fi
  e0=$(./coff0 "$isrc" "$tmpdir/inc0.s" 2>&1 >/dev/null)
  e1=$("$tmpdir/coff1" "$isrc" "$tmpdir/inc1.s" 2>&1 >/dev/null)
  if echo "$e0" | grep -q "include: cannot open" && echo "$e1" | grep -q "include: cannot open"; then
    echo "PASS without -I both compilers refuse it"
    pass=$((pass + 1))
  else
    echo "FAIL without -I: coff0 said '$e0', coff1 said '$e1'"
    fail=$((fail + 1))
  fi
  echo ""

  echo "Audit tools: c0/sha256.c0 and c0/cmp.c0, built by coff1, vs sha256sum and cmp"
  # verify_bootstrap.sh cross-checks the system's sha256sum and cmp against
  # these two, so they are held to the system tools here on every run.
  # GNU coreutils is the oracle, the same trick smed_tests.sh plays with
  # GNU as.
  if "$tmpdir/coff1" c0/sha256.c0 "$tmpdir/sha256.s" \
     && as "$tmpdir/sha256.s" -o "$tmpdir/sha256.o" && ld "$tmpdir/sha256.o" -o "$tmpdir/sha256" \
     && "$tmpdir/coff1" c0/cmp.c0 "$tmpdir/cmp.s" \
     && as "$tmpdir/cmp.s" -o "$tmpdir/cmp.o" && ld "$tmpdir/cmp.o" -o "$tmpdir/c0cmp"; then
    # Padding edge cases first. SHA-256 pads to 56 bytes mod 64, so 55, 56
    # and 64 bytes are where a padding bug shows.
    mkdir -p "$tmpdir/at"
    : > "$tmpdir/at/empty"
    printf 'abc' > "$tmpdir/at/abc"
    head -c 55 /dev/zero > "$tmpdir/at/z55"
    head -c 56 /dev/zero > "$tmpdir/at/z56"
    head -c 64 /dev/zero > "$tmpdir/at/z64"
    set -- "$tmpdir"/at/* tests/*.c0 c0/*.c0 coff0.c "$tmpdir/coff1.s" "$tmpdir/coff1"
    if [ "$("$tmpdir/sha256" "$@")" = "$(sha256sum "$@")" ]; then
      echo "PASS sha256 matches sha256sum on $# files"
      pass=$((pass + 1))
    else
      echo "FAIL sha256 differs from sha256sum"
      diff <("$tmpdir/sha256" "$@") <(sha256sum "$@") | head -6
      fail=$((fail + 1))
    fi

    printf 'ab\ncd' > "$tmpdir/at/c1"
    printf 'ab\nce' > "$tmpdir/at/c2"
    printf 'ab' > "$tmpdir/at/c3"
    printf 'a\nb\n' > "$tmpdir/at/d1"
    printf 'a\nb\n\n' > "$tmpdir/at/d2"
    cmp_bad=0
    cmp_n=0
    at="$tmpdir/at"
    for pair in "c1 c1" "c1 c2" "c1 c3" "c3 c1" "empty c1" "c1 empty" "empty empty" \
                "c1 missing" "d1 d2" "d2 d1" "-s c1 c2" "-s c1 c1" "-s c1 missing"; do
      set -- $pair
      args=()
      for a in "$@"; do
        case "$a" in -s) args+=("$a") ;; *) args+=("$at/$a") ;; esac
      done
      want=$(LC_ALL=C cmp "${args[@]}" 2>&1; echo "exit $?")
      got=$("$tmpdir/c0cmp" "${args[@]}" 2>&1; echo "exit $?")
      cmp_n=$((cmp_n + 1))
      if [ "$want" != "$got" ]; then
        echo "  cmp ${pair}: want [$want] got [$got]"
        cmp_bad=$((cmp_bad + 1))
      fi
    done
    if [ "$cmp_bad" -eq 0 ]; then
      echo "PASS cmp matches cmp (C locale) on $cmp_n cases, output and exit code"
      pass=$((pass + 1))
    else
      echo "FAIL cmp differs from cmp on $cmp_bad of $cmp_n cases"
      fail=$((fail + 1))
    fi
  else
    echo "FAIL building c0/sha256.c0 or c0/cmp.c0 with coff1"
    fail=$((fail + 1))
  fi

  echo ""
  echo "Bootstrap fixpoint: c0/coff.c0 compiling itself via its own CLI, stage2 vs stage1"
  if "$tmpdir/coff1" c0/coff.c0 "$tmpdir/coff2.s" \
     && diff -q "$tmpdir/coff1.s" "$tmpdir/coff2.s" >/dev/null; then
    echo "PASS fixpoint stage1==stage2"
    pass=$((pass + 1))
  else
    echo "FAIL fixpoint stage1==stage2"
    diff "$tmpdir/coff1.s" "$tmpdir/coff2.s" | head -10
    fail=$((fail + 1))
  fi

  echo ""
  echo "Bootstrap fixpoint: stage2 binary compiling coff.c0 again, stage3 vs stage2"
  if as "$tmpdir/coff2.s" -o "$tmpdir/coff2.o" \
     && ld "$tmpdir/coff2.o" -o "$tmpdir/coff2" \
     && "$tmpdir/coff2" c0/coff.c0 "$tmpdir/coff3.s" \
     && diff -q "$tmpdir/coff2.s" "$tmpdir/coff3.s" >/dev/null; then
    echo "PASS fixpoint stage2==stage3"
    pass=$((pass + 1))
  else
    echo "FAIL fixpoint stage2==stage3"
    diff "$tmpdir/coff2.s" "$tmpdir/coff3.s" | head -10
    fail=$((fail + 1))
  fi
else
  echo "FAIL building c0/coff.c0"
  fail=$((fail + 1))
fi

echo ""
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
