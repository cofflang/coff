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
# Purpose: builds coff0, then runs every tests/*.c0 program through the
# full pipeline (coff0 -> as -> ld -> run) and checks its exit code
# against the `// expect: N` (or `// expect: compile-error`) comment on
# the file's first line. If tests/<name>.expected_stdout exists, the
# program's actual stdout is also compared against it, byte for byte.
#
# `// expect: no-exec` is a third variant: the program must compile, but
# is never assembled, linked or run. This is for builtins like outb/inb
# whose instructions fault in normal Linux userspace (no ring 0, no
# IOPL). Their codegen is still covered by the differential loops below,
# which never execute the compiled binary.
#
# After the unit tests come the differential stages (each c0-written
# stage diffed against coff0 over the whole corpus) and finally the
# bootstrap fixpoint checks: coff.c0 compiling itself, three
# generations deep, all byte-identical.
set -u

cd "$(dirname "$0")"

echo "Building coff0..."
gcc -Wall -Wextra -o coff0 coff0.c || { echo "coff0 failed to build"; exit 1; }

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

pass=0
fail=0

for src in tests/*.c0; do
  name=$(basename "$src" .c0)
  expect=$(head -n1 "$src" | sed -n 's/^\/\/ expect: //p')
  # An optional `// args: a b c` comment on the second line lists extra
  # argv entries for the compiled binary (used by the argc/argv tests).
  args=$(sed -n '2p' "$src" | sed -n 's/^\/\/ args: //p')

  if [ "$expect" = "compile-error" ]; then
    if ./coff0 "$src" "$tmpdir/$name.s" 2>/dev/null; then
      echo "FAIL $name (expected a compile error, but it compiled)"
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
  for src in tests/*.c0 c0/lex.c0; do
    name=$(basename "$src" .c0)
    # Files coff0 itself refuses to lex are skipped: on error coff0 -t
    # prints nothing, while lex.c0 streams tokens up to the bad byte, so
    # the outputs are not comparable there.
    if ./coff0 -t "$src" > "$tmpdir/ref_tokens" 2>/dev/null; then
      # Passing the real path (not piping stdin) is what lets lex.c0
      # resolve include directives -- a relative include path has
      # nothing to resolve against from piped bytes. See lex.c0's main().
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
  for src in tests/*.c0 c0/lex.c0 c0/parse.c0; do
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
  for src in tests/*.c0 c0/lex.c0 c0/parse.c0 c0/coff.c0; do
    name=$(basename "$src" .c0)
    # Only compares files that actually compile: compile-error cases are
    # excluded since neither side produces assembly to diff. Uses coff1's
    # real file-based CLI, the same interface coff0 itself has.
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
