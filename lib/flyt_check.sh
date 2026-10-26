#!/usr/bin/env bash
#
# flyt_check.sh - check flyt against the machine's own doubles
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# Checks flyt.c0 against the doubles of the machine it runs on, which
# Python's floats are. Random bit patterns, which find the subnormals, the
# infinities and the NaNs, and random ordinary numbers, which find the
# rounding, through add, sub, mul and div, compare, and the conversions to
# and from int. Every result has to be the same bits, except that any NaN
# matches any NaN, through square roots too. Reading decimal text has to give
# exactly Python's double, and writing with 0 to 6 places exactly Python's
# digits, long inputs, halfway cases, subnormals and huge numbers included.
# `./flyt_check.sh time` also times 100 rounds.
set -eu
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
../coff1 flyt_check.c0 "$tmp/check.s"
as "$tmp/check.s" -o "$tmp/check.o" && ld "$tmp/check.o" -o "$tmp/check"
python3 - "$tmp" "${1:-}" <<'PY'
import random, struct, subprocess, sys, time, math
tmp, mode = sys.argv[1], sys.argv[2]
random.seed(20261015)
def bits(f): return struct.unpack("<Q", struct.pack("<d", f))[0]
def flo(b): return struct.unpack("<d", struct.pack("<Q", b))[0]
def s64(u): return u - (1 << 64) if u >= 1 << 63 else u
def u64(s): return s & ((1 << 64) - 1)
special = [0, 1 << 63, bits(1.0), bits(-1.0), bits(float("inf")), bits(-float("inf")),
           bits(float("nan")), 1, (1 << 52) - 1, 1 << 52, bits(2.0 ** 1023),
           bits(1.7976931348623157e308), bits(2.2250738585072014e-308), bits(0.5),
           bits(3.0), bits(0.1), bits(1e-310), bits(-1e-320)]
def rnd():
    k = random.random()
    if k < 0.15: return random.choice(special)
    if k < 0.45: return random.getrandbits(64)
    if k < 0.65:
        e = random.randint(-60, 60)
        return bits(random.uniform(-1, 1) * 2.0 ** e)
    if k < 0.8: return bits(float(random.randint(-10 ** 6, 10 ** 6)) / random.choice([1, 3, 7, 10, 1024]))
    # neighbours in exponent, for cancellation and ties
    b = bits(random.uniform(1, 2))
    return b ^ random.getrandbits(3)
cases = []
for _ in range(40000):
    op = random.choice("asmdcitq")
    a, b = rnd(), rnd()
    if op == "i":
        a = u64(random.choice([random.getrandbits(63) * random.choice([1, -1]),
                               random.randint(-10 ** 6, 10 ** 6), -(1 << 63), (1 << 53) + 1,
                               (1 << 62) + 3]))
        if random.random() < 0.5: a = u64(random.randint(-(1 << 63), (1 << 63) - 1))
    if op == "a":
        # sometimes b close to -a, for cancellation
        if random.random() < 0.2: b = (a ^ (1 << 63)) ^ random.getrandbits(4)
    cases.append((op, a, b))
from decimal import Decimal
for _ in range(6000):
    kind = random.random()
    if kind < 0.3: v = random.uniform(-1e6, 1e6)
    elif kind < 0.5: v = random.uniform(-1, 1) * 10.0 ** random.randint(-300, 300)
    elif kind < 0.6: v = flo(random.getrandbits(52) | (random.choice([0, 1]) << 63))  # subnormal
    else: v = float(random.randint(-10 ** 9, 10 ** 9)) / 1000
    if math.isinf(v) or math.isnan(v): continue
    r = random.random()
    if r < 0.4: text = repr(v)
    elif r < 0.6: text = "%.6g" % v
    elif r < 0.8:
        # exactly halfway between v and its neighbour, as long digits
        nb = flo(bits(v) + 1)
        if math.isinf(nb): continue
        mid = (Decimal(v) + Decimal(nb)) / 2
        text = "%.30e" % mid
        text = format(mid, ".26e")
    else:
        text = format(Decimal(v), ".25e")
    if len(text) <= 33:
        cases.append(("p", text, 0))
for _ in range(4000):
    r = random.random()
    if r < 0.5: v = random.uniform(-1e6, 1e6)
    elif r < 0.7: v = random.uniform(-1, 1)
    elif r < 0.85: v = random.randint(-10 ** 6, 10 ** 6) / 8 + 1 / 16   # exact halves
    else: v = random.uniform(-1, 1) * 10.0 ** random.randint(10, 300)
    places = random.randint(0, 6)
    if abs(v) >= 1e30: places = random.randint(0, 2)
    if len("%.*f" % (places, v)) <= 47:
        cases.append(("f", bits(v), places))
def expect(op, a, b):
    fa, fb = flo(a), flo(b)
    if op == "a": return bits(fa + fb)
    if op == "s": return bits(fa - fb)
    if op == "m": return bits(fa * fb)
    if op == "d":
        if fb == 0:
            if fa == 0 or math.isnan(fa): return bits(float("nan"))
            return bits(math.copysign(float("inf"), fa) * math.copysign(1, fb))
        return bits(fa / fb)
    if op == "c":
        if math.isnan(fa) or math.isnan(fb): return 2
        return u64(-1) if fa < fb else (0 if fa == fb else 1)
    if op == "i": return bits(float(s64(a)))
    if op == "q":
        if math.isnan(fa): return bits(float("nan"))
        if fa == 0: return a
        if fa < 0: return bits(float("nan"))
        return bits(math.sqrt(fa))
    if op == "t":
        if math.isnan(fa): return 0
        if fa >= 2.0 ** 63: return (1 << 63) - 1
        if fa < -(2.0 ** 63): return 1 << 63
        return u64(int(fa))
with open(tmp + "/in", "w") as f:
    for op, a, b in cases:
        if op == "p":
            f.write("p " + a.ljust(33) + "\n")
        else:
            f.write("%s %016x %016x\n" % (op, a, b))
rc = subprocess.run([tmp + "/check", tmp + "/in", tmp + "/out"]).returncode
if rc != 0:
    print("FAIL: the checker exited %d" % rc); sys.exit(1)
lines = open(tmp + "/out").read().split("\n")
bad = []
def ulps(x, y):
    sx, sy = s64(x), s64(y)
    if sx < 0: sx = -(1 << 63) - sx
    if sy < 0: sy = -(1 << 63) - sy
    return abs(sx - sy)
for (op, a, b), line in zip(cases, lines):
    if op == "p":
        g = int(line[:16], 16)
        if g != bits(float(a)):
            bad.append((op, 0, 0, bits(float(a)), g))
            print("  p %s: want %016x got %016x" % (a, bits(float(a)), g))
        continue
    if op == "f":
        g = line.strip()
        want = "%.*f" % (b, flo(a))
        if g != want:
            bad.append((op, a, b, 0, 0))
            print("  f %r %d: want %s got %s" % (flo(a), b, want, g))
        continue
    g = int(line[:16], 16)
    e = expect(op, a, b)
    isnan = lambda x: op in "asmdq" and (x >> 52) & 2047 == 2047 and x & ((1 << 52) - 1)
    if e == g or (isnan(e) and isnan(g)):
        continue
    bad.append((op, a, b, e, g))
counts = {}
for op, a, b in cases: counts[op] = counts.get(op, 0) + 1
print("cases: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
for op, a, b, e, g in bad[:10]:
    if op in "pf": continue
    print("  %s %016x %016x  want %016x got %016x  (%r %r)" % (op, a, b, e, g, flo(a), flo(b)))
if bad:
    print("FAIL: %d of %d differ" % (len(bad), len(cases))); sys.exit(1)
print("PASS: all %d agree with the machine's doubles" % len(cases))
if mode == "time":
    t0 = time.time()
    subprocess.run([tmp + "/check", tmp + "/in", tmp + "/out", "100"])
    dt = time.time() - t0
    print("%d operations in %.2f s, %.0f ns each" % (100 * len(cases), dt, dt / (100 * len(cases)) * 1e9))
PY
