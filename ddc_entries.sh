#!/usr/bin/env bash
#
# ddc_entries.sh - the list of compiles the DDC and codegendiff compare.
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# usage
#   ddc_entries.sh coff
#   ddc_entries.sh moonshot <moonshot-dir> <scratch-dir>
#
# Prints one line per compile, five fields separated by '|'.
#
#   label|dir|flags|file|expect
#
# label names the compile in messages, dir is the directory the compiler
# runs in, flags are the compiler's options (word-split, no path in them
# contains a space), file is the source relative to dir, and expect is
# "ok" or "error" (a test whose first line is `// expect: compile-error`).
#
# A payload is caught only on inputs where it fires, so the list is every
# compile the build performs, run the way the build runs it, and nothing is
# found by trying files and keeping the ones that compile. The coff list is
# the test suite and the compiler sources, as run_tests.sh compiles them.
# The Moonshot list is read from the scripts that build Moonshot.
#
#   build.sh            kernel/core/kmain.c0, once per build/debug.c0 variant
#   gen_programs.sh     every `prog` line, with its directory and -I flags
#   gen_elf_tests.sh    tools/bake.c0 and every tests/elf/*.c0
#
# Every line of those three scripts that can reach a compiler, the
# assembler or another script must equal, in order, the set modelled here,
# and no other tools/*.sh may name a compiler. A script that compiles
# something new, or compiles differently, makes this fail until it is
# modelled, rather than drop out of the comparison unnoticed. This catches
# drift, not a determined author, who can always spell a path so no pattern
# sees it. A hostile edit to those scripts is caught only by reviewing them,
# since they sit in the kernel's repository and this repository's manifest
# does not hash them.
#
# kmain.c0 includes build/debug.c0, which build.sh writes. Each variant gets
# a copy of kernel/ in the scratch directory with only that one file beside
# it, so the audit never depends on, or writes to, the tree's build/. The
# `embed` files are not needed, because coff never opens them (the
# assembler's .incbin does). So the bytes the kernel embeds are covered by
# their own entries instead, and every embed in the kernel must map to an
# entry here or this fails.
#
# Exits non-zero, with the reason on stderr, whenever the list cannot be
# trusted to be complete.

set -u
export LC_ALL=C

die() { echo "ddc_entries.sh: $*" >&2; exit 1; }

# A path that would split wrongly in the flags field, or break the fields.
checkpath() {
  case "$1" in
    *[[:space:]]*|*'|'*|*[*?[]*) die "path with a space, '|' or a glob character cannot be listed: $1" ;;
  esac
}

emit() { # label dir flags file expect
  checkpath "$2"; checkpath "$4"
  case "$3" in *[*?[]*) die "flags with a glob character cannot be listed: $3" ;; esac
  printf '%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5"
}

coff_entries() {
  local here f expect
  here=$(cd "$(dirname "$0")" && pwd)
  checkpath "$here"
  cd "$here" || die "cannot enter $here"
  for f in tests/*.c0; do
    [ -f "$f" ] || die "no tests/*.c0 in $here"
    expect=ok
    [ "$(head -n1 "$f")" = "// expect: compile-error" ] && expect=error
    emit "$f" "$here" "" "$f" "$expect"
  done
  # run_tests.sh's include-search check, with its -I.
  [ -f tests/include_search/main.c0 ] || die "tests/include_search/main.c0 is missing"
  emit "tests/include_search/main.c0 -I tests/include_search/lib" "$here" \
    "-I tests/include_search/lib" tests/include_search/main.c0 ok
  for f in c0/*.c0; do
    [ -f "$f" ] || die "no c0/*.c0 in $here"
    emit "$f" "$here" "" "$f" ok
  done
}

# The lines of a build script that can reach a compiler, an assembler or
# another script. Every line that names coff0, coff1, smed, their
# variables, the c0-coff directory, a .sh file, eval, source, exec or the
# `.` command, comments and `prog` lines left out (the prog lines are read
# one by one below). A second pattern, for scripts that should not compile
# anything at all, names only the compilers.
SCAN='COFF|SMED|coff[01]|\./smed|/smed|c0-coff|\.sh\b|\beval\b|\bsource\b|\bexec\b|^[[:space:]]*\.[[:space:]]'
SCAN_TOOLS='COFF|SMED|coff[01]|\./smed|/smed|c0-coff|\beval\b'
scan() { # pattern file
  grep -E -- "$1" "$2" | grep -vE '^[[:space:]]*#' | grep -vE '^prog '
}

# Fails unless the scanned lines of a script are exactly the modelled set,
# in order. The set is what this file knows how to list. The COFF1= and
# SMED= assignments are in it on purpose, since the prog lines' directory
# "$(dirname "$COFF1")/c0" is read as ../c0-coff/c0 below, and so are the
# [ -x ] checks, which run nothing.
checkset() { # file modelled-set-function
  local got
  got=$(scan "$SCAN" "$1")
  [ "$got" = "$("$2")" ] \
    || die "$1 reaches a compiler, an assembler or a script in a way this list does not model, update ddc_entries.sh to match it.
$(diff <("$2") <(printf '%s\n' "$got") | sed 's/^/  /')"
}

model_build() {
  cat <<'MODEL_END'
    echo "site copy refuses a source outside the named folder" >&2
    echo "site copy refuses a symbolic link at the source root" >&2
    echo "site source folder absent, skipping /site" >&2
    echo "site copy refuses a source root that is not a directory" >&2
COFF1=../c0-coff/coff1
COFF_SRC=../c0-coff/c0/coff.c0
SMED=../c0-coff/smed
SMED_SRC=../c0-coff/c0/smed.c0
COFF_DIR=../c0-coff
  (cd "$COFF_DIR" && ./verify_bootstrap.sh "$@") || {
    echo "build.sh: bootstrap verification FAILED -- refusing to build a kernel"
    echo "          with an unverified compiler. See c0-coff/TRUST.md."
if [ ! -x "$COFF1" ] || [ "$COFF_SRC" -nt "$COFF1" ]; then
  COFF0=$COFF_DIR/coff0
  if [ ! -x "$COFF0" ] || [ "$COFF_DIR/coff0.c" -nt "$COFF0" ]; then
    echo "building coff0 with $CC (one-time bootstrap dependency for coff1)..."
    (cd "$COFF_DIR" && $CC -Wall -Wextra -o coff0 coff0.c)
  echo "(re)bootstrapping coff1 (self-hosted, via coff0)..."
  (cd "$COFF_DIR" && ./coff0 c0/coff.c0 coff1.s && as coff1.s -o coff1.o && ld coff1.o -o coff1)
if [ ! -x "$SMED" ] || [ "$SMED_SRC" -nt "$SMED" ] || [ "$rebuilt" -eq 1 ]; then
  (cd "$COFF_DIR" && ./coff1 c0/smed.c0 smed.s)
  if [ ! -x "$SMED" ]; then
    (cd "$COFF_DIR" && as smed.s -o smed.o && ld smed.o -o smed && rm smed.o)
  (cd "$COFF_DIR" && ./smed smed.s smed.gen1 && chmod +x smed.gen1 \
     && ./smed.gen1 smed.s smed.new && chmod +x smed.new \
  if [ -f "$COFF_DIR/sync_release.sh" ]; then
  sha256sum "$COFF1" "$SMED" | sed 's|  .*/|  |' > "$TOOL_RECORD"
    echo "build.sh: no verified hash recorded for $path -- running the full"
    sha256sum "$COFF1" "$SMED" | sed 's|  .*/|  |' > "$TOOL_RECORD"
    echo "build.sh: $name does not match the hash in $expected_src."
    echo "          that was verified against source. Refusing to build."
    echo "          c0-coff/verify_bootstrap.sh. See c0-coff/TRUST.md."
gate coff1 "$COFF1"
gate smed "$SMED"
./tools/gen_programs.sh >/dev/null
./tools/gen_elf_tests.sh >/dev/null
"$COFF1" kernel/core/kmain.c0 build/kmain.s
"$SMED" -T kernel/core/linker.ld -m build/moonshot.map kernel/core/boot.s build/kmain.s \
"$COFF1" tools/site_copy.c0 build/site_copy.s
"$SMED" build/site_copy.s build/site_copy
"$COFF1" tools/site_images.c0 build/site_images.s
"$SMED" build/site_images.s build/site_images
MODEL_END
}

model_progs() {
  cat <<'MODEL_END'
COFF1=../c0-coff/coff1
SMED=../c0-coff/smed
[ -x "$COFF1" ] || { echo "coff1 not built at $COFF1 -- run build.sh first"; exit 1; }
[ -x "$SMED" ]  || { echo "smed not built at $SMED -- run build.sh first"; exit 1; }
  (cd "$1" && "$OLDPWD/$COFF1" --elf -I "$LIB" ${4:+-I "$4"} "$2" "$OUT/$3.s") \
  "$SMED" --base 0x80000000 "$OUT/$3.s" "$OUT/$3.elf" \
MODEL_END
}

model_elft() {
  cat <<'MODEL_END'
COFF1=../c0-coff/coff1
SMED=../c0-coff/smed
[ -x "$COFF1" ] || { echo "coff1 not built at $COFF1 -- run build.sh first"; exit 1; }
[ -x "$SMED" ]  || { echo "smed not built at $SMED -- run build.sh first"; exit 1; }
"$COFF1" tools/bake.c0 "$tmp/bake.s" && "$SMED" "$tmp/bake.s" "$tmp/bake" && chmod +x "$tmp/bake"
  "$COFF1" --elf -I programs/lib -I "$GNISTA_DIR" "$src" "$tmp/$name.s" || { echo "coff --elf failed on $src"; exit 1; }
  "$SMED" --base 0x80000000 "$tmp/$name.s" "$tmp/$name.elf" || { echo "smed failed on $src"; exit 1; }
MODEL_END
}

moonshot_entries() {
  local ms scratch root gnista utsikt lib n v f line dir src name extra rest
  [ -d "${1:-}" ] || die "moonshot directory not found: ${1:-}"
  [ -d "${2:-}" ] || die "scratch directory not found: ${2:-}"
  ms=$(cd "$1" && pwd)
  scratch=$(cd "$2" && pwd)
  root=$(cd "$ms/.." && pwd)
  checkpath "$ms"; checkpath "$scratch"
  local build="$ms/build.sh" progs="$ms/tools/gen_programs.sh" elft="$ms/tools/gen_elf_tests.sh"
  local kmain="$ms/kernel/core/kmain.c0"
  for f in "$build" "$progs" "$elft" "$kmain"; do
    [ -f "$f" ] || die "$f is missing"
  done

  checkset "$build" model_build
  checkset "$progs" model_progs
  checkset "$elft" model_elft
  for f in "$ms"/tools/*.sh; do
    [ "$f" = "$progs" ] || [ "$f" = "$elft" ] && continue
    [ -z "$(scan "$SCAN_TOOLS" "$f")" ] \
      || die "$f names a compiler or assembler, and this list does not model it, update ddc_entries.sh to match it"
  done
  if grep -nE '^[[:space:]]+prog[[:space:]]' "$progs" >/dev/null; then
    die "gen_programs.sh has an indented prog line this list would not read: $(grep -nE '^[[:space:]]+prog[[:space:]]' "$progs" | head -1)"
  fi

  # The kernel, once per debug.c0 that build.sh can write.
  n=$(grep -c '> build/debug.c0$' "$build")
  v=0
  while IFS= read -r line; do
    v=$((v + 1))
    name=$(printf '%s\n' "$line" | sed -n 's/^include "\.\.\/kernel\/shell\/\([a-z_]*\)\.c0";$/\1/p')
    [ -n "$name" ] || die "build.sh writes a build/debug.c0 this list cannot read: $line"
    dir="$scratch/kmain_$name"
    rm -rf "$dir"
    mkdir -p "$dir/build" || die "cannot create $dir"
    cp -R "$ms/kernel" "$dir/kernel" || die "cannot copy $ms/kernel"
    printf '%s\n' "$line" > "$dir/build/debug.c0"
    emit "${ms#"$root"/}/kernel/core/kmain.c0 with $name" "$dir" "" kernel/core/kmain.c0 ok
  done < <(sed -n "s/^ *echo '\(.*\)' > build\/debug\.c0$/\1/p" "$build")
  [ "$v" -gt 0 ] && [ "$v" -eq "$n" ] \
    || die "build.sh writes build/debug.c0 $n times, $v of them were read"

  # The ring-3 programs, as gen_programs.sh finds and compiles them.
  gnista=${GNISTA_DIR:-}
  if [ -z "$gnista" ]; then
    if [ -f "$ms/programs/infecteria.c0" ]; then gnista=programs; else gnista=../gnista; fi
  fi
  utsikt=${UTSIKT_DIR:-}
  if [ -z "$utsikt" ]; then
    if [ -f "$ms/programs/utsikt/utsikt.c0" ]; then utsikt=programs/utsikt; else utsikt=../utsikt; fi
  fi
  gnista=$(cd "$ms" && cd "$gnista" && pwd) || die "gnista not found from $ms"
  utsikt=$(cd "$ms" && cd "$utsikt" && pwd) || die "utsikt not found from $ms"
  checkpath "$gnista"; checkpath "$utsikt"
  lib="$ms/programs/lib"
  : > "$scratch/prog_names"
  n=0
  while IFS= read -r line; do
    rest=${line#prog }
    rest=${rest//'"$(cd "$GNISTA_DIR" && pwd)"'/@GNISTA@}
    rest=${rest//'"$(dirname "$COFF1")/c0"'/@COFFSRC@}
    rest=${rest//'"$GNISTA_DIR"'/@GNISTA@}
    rest=${rest//'"$UTSIKT_DIR"'/@UTSIKT@}
    printf '%s\n' "$rest" | grep -qE '^[A-Za-z0-9_./@]+ [A-Za-z0-9_]+\.c0 [A-Za-z0-9_]+( @[A-Z]+@)?$' \
      || die "gen_programs.sh has a prog line this list cannot read: $line"
    read -r dir src name extra <<< "$rest"
    case "$dir" in
      @GNISTA@) dir=$gnista ;;
      @UTSIKT@) dir=$utsikt ;;
      @COFFSRC@) dir=$ms/../c0-coff/c0 ;;
      @*) die "gen_programs.sh has a prog line this list cannot read: $line" ;;
      *) dir=$ms/$dir ;;
    esac
    dir=$(cd "$dir" 2>/dev/null && pwd) || die "gen_programs.sh compiles in a directory that is missing: $line"
    [ -f "$dir/$src" ] || die "gen_programs.sh compiles a file that is missing: $dir/$src"
    case "${extra:-}" in
      "") extra="" ;;
      @GNISTA@) extra="-I $gnista" ;;
      *) die "gen_programs.sh has a prog line this list cannot read: $line" ;;
    esac
    emit "${dir#"$root"/}/$src --elf" "$dir" "--elf -I $lib${extra:+ $extra}" "$src" ok
    echo "$name" >> "$scratch/prog_names"
    n=$((n + 1))
  done < <(grep -E '^prog ' "$progs")
  [ "$n" -gt 0 ] || die "gen_programs.sh has no prog lines"

  for f in tools/site_copy.c0 tools/site_images.c0; do
    [ -f "$ms/$f" ] || die "$f is missing"
    emit "${ms#"$root"/}/$f" "$ms" "" "$f" ok
  done

  # bake, and the ELF test programs it packs.
  [ -f "$ms/tools/bake.c0" ] || die "tools/bake.c0 is missing"
  emit "${ms#"$root"/}/tools/bake.c0" "$ms" "" tools/bake.c0 ok
  local egnista
  if [ -f "$ms/programs/infecteria.c0" ]; then egnista=programs; else egnista=../gnista; fi
  n=0
  for f in "$ms"/tests/elf/*.c0; do
    [ -f "$f" ] || continue
    src=tests/elf/$(basename "$f")
    emit "${ms#"$root"/}/$src --elf" "$ms" "--elf -I programs/lib -I $egnista" "$src" ok
    n=$((n + 1))
  done
  [ "$n" -gt 0 ] || die "tests/elf has no .c0 programs, but the kernel embeds their pack"

  # Every embed anywhere under kernel/ maps to an entry above. The path is
  # resolved against the embedding file's own directory, the way coff
  # resolves it, and must land in build/. There must be at least one
  # program and exactly one ELF test pack, or the scan itself has missed
  # something.
  local efile erest epath etarget buildreal nelf=0 npack=0
  buildreal=$(realpath -m "$ms/build")
  while IFS= read -r line; do
    efile=${line%%:*}
    erest=${line#*:}; erest=${erest#*:}
    epath=$(printf '%s\n' "$erest" | sed -n 's/^[[:space:]]*embed [a-z0-9_]* "\([^"]*\)";[[:space:]]*$/\1/p')
    [ -n "$epath" ] || die "an embed this list cannot read: $line"
    etarget=$(realpath -m "$(dirname "$efile")/$epath")
    case "$etarget" in
      "$buildreal"/elf_tests.pack) npack=$((npack + 1)) ;;
      "$buildreal"/*.elf)
        name=${etarget#"$buildreal"/}; name=${name%.elf}
        case "$name" in */*) die "an embed below build/ this list cannot map: $line" ;; esac
        grep -qxF "$name" "$scratch/prog_names" \
          || die "the kernel embeds build/$name.elf, which no prog line in gen_programs.sh compiles: $line"
        nelf=$((nelf + 1)) ;;
      *) die "an embed this list cannot map to a compile: $line" ;;
    esac
  done < <(grep -rnE --include='*.c0' '^[[:space:]]*embed[[:space:]]' "$ms/kernel")
  [ "$nelf" -gt 0 ] || die "no program embeds found under $ms/kernel, the embed scan cannot be trusted"
  [ "$npack" -eq 1 ] || die "$npack embeds of build/elf_tests.pack under $ms/kernel, expected exactly one"
}

case "${1:-}" in
  coff) coff_entries ;;
  moonshot) moonshot_entries "${2:-}" "${3:-}" ;;
  *) echo "usage: ddc_entries.sh coff | ddc_entries.sh moonshot <moonshot-dir> <scratch-dir>" >&2; exit 2 ;;
esac
