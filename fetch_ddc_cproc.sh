#!/usr/bin/env bash
#
# fetch_ddc_cproc.sh - fetch and build cproc for the diverse double-compiling check
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# Fetches and builds cproc, a third C compiler for verify_bootstrap.sh's
# diverse double-compiling check, at the exact versions it was checked with.
#
# cproc (Michael Forney) is a small C11 compiler that uses QBE (Quentin
# Carbonneaux) as its backend. Neither shares code or history with gcc or
# tcc, which is the point. For a backdoor in the C compiler that builds
# coff0 to survive DDC, it has to be in three unrelated compilers and
# behave identically in all of them.
#
# Both are built with tcc, not gcc, so the third witness does not pass
# through gcc on its way into existence. cproc does NOT replace everything,
# and TRUST.md says so. It runs the system's `cpp` (gcc's preprocessor) and
# GNU as/ld, and links against glibc. It diversifies the compilation
# proper, C to machine code, which is where a Thompson-style payload lives.
#
# Installs into ~/.local/opt/c0-ddc (or $1), with no root and nothing system-wide.
# verify_bootstrap.sh looks there.
set -eu

DEST=${1:-$HOME/.local/opt/c0-ddc}
QBE_COMMIT=e786f06032fefa2e3790d6b1c9e31ed138f475a6     # 2026-06-08
CPROC_COMMIT=d1c53ddf56571573a7025324c8dd5c6d547a4d1f   # 2026-06-01

command -v tcc >/dev/null 2>&1 || { echo "needs tcc to build them"; exit 1; }
mkdir -p "$DEST"
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

git clone -q git://c9x.me/qbe.git "$src/qbe"
git -C "$src/qbe" checkout -q "$QBE_COMMIT"
git clone -q https://git.sr.ht/~mcf/cproc "$src/cproc"
git -C "$src/cproc" checkout -q "$CPROC_COMMIT"

make -C "$src/qbe" CC=tcc >/dev/null
(cd "$src/cproc" && ./configure CC=tcc >/dev/null && make CC=tcc >/dev/null)

cp "$src/qbe/qbe" "$src/cproc/cproc" "$src/cproc/cproc-qbe" "$DEST/"
printf 'qbe %s\ncproc %s\n' "$QBE_COMMIT" "$CPROC_COMMIT" > "$DEST/VERSIONS"
echo "built qbe and cproc into $DEST:"
cat "$DEST/VERSIONS"
