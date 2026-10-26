# check_saved_regs.awk - check that coff's output saves the registers it uses
#
# Copyright (C) 2026 tavro
#
# This file is part of coff, the compiler toolchain for c0. coff is free
# software, distributed under the GNU General Public License version 3
# or (at your option) any later version, WITHOUT ANY WARRANTY; see the
# LICENSE file for the full text.
#
# Reads coff's assembly and prints every function that uses rbx or r12-r15
# without saving the register in its prologue and restoring it from the same
# slot before it returns. The System V ABI has a function preserve those
# five, and any caller, my OS kernel included, relies on that. run_tests.sh
# runs it over the whole test corpus and over the compiler itself.
function check(   r, n) {
    if (fn == "") { return }
    for (r in used) {
        if (!(r in saved) || !(r in restored) || saved[r] != restored[r]) {
            print fn " uses " r " without saving and restoring it"
            bad++
        }
    }
    delete used; delete saved; delete restored
}
/^[A-Za-z_][A-Za-z_0-9]*:$/ { check(); fn = substr($0, 1, length($0) - 1); next }
{
    line = $0
    if (match(line, /^    mov \[rbp - [0-9]+\], (rbx|r12|r13|r14|r15)$/)) {
        split(line, p, /[][]/); r = substr(line, length(line) - 2)
        saved[r] = p[2]
        next
    }
    if (match(line, /^    mov (rbx|r12|r13|r14|r15), \[rbp - [0-9]+\]$/)) {
        split(line, p, /[][]/); r = substr(line, 9, 3); restored[r] = p[2]
        next
    }
    while (match(line, /(^|[^.A-Za-z0-9_])(rbx|r1[2-5])([^0-9a-z]|$)/)) {
        line = substr(line, RSTART)
        match(line, /(rbx|r1[2-5])/)
        used[substr(line, RSTART, 3)] = 1
        line = substr(line, RSTART + 3)
    }
}
END { check(); exit bad > 0 }
