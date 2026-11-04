#!/usr/bin/env python3
"""Find inline OS calls whose register arguments a helper call can clobber.

GCC (m68k-amigaos, -Os) can move a library helper call such as __mulsi3
(32-bit multiply on the 68000) or strlen between loading an inline OS
call's register arguments (a0/a1/d0/d1, base in a6) and the call itself.
The helper then clobbers a0/a1/d0/d1 and the OS call gets garbage: this
made CopyMem copy from UtilityBase onto the stack when list entries were
18 bytes (count * 18 became a __mulsi3 call).

The pattern in the compiler's assembly: a load into a6, then a jsr/bsr to
anything other than a6@(...), then the #APP block of the OS call, with no
reload of a6 in between. Usage: check_syscalls.py file.s ...  (exit 1 if
any are found)
"""
import re
import sys

LOAD_A6 = re.compile(r"^\s+move(a)?\.l\s+[^,]+,a6\s*$")
CALL = re.compile(r"^\s+(jsr|jbsr|bsr)\s+(\S+)")
APP = re.compile(r"^#APP")
LABEL = re.compile(r"^[A-Za-z_.$][\w.$]*:")


def scan(path):
    found = []
    lines = open(path).read().splitlines()
    func = "?"
    armed = None            # line number of the last a6 load
    calls = []
    for i, line in enumerate(lines):
        if LABEL.match(line) and not line.startswith("L"):
            func = line.rstrip(":")
            armed, calls = None, []
        if LOAD_A6.match(line):
            armed, calls = i, []
            continue
        m = CALL.match(line)
        if m and armed is not None and not m.group(2).startswith("a6@"):
            calls.append(m.group(2))
            continue
        if APP.match(line) and armed is not None:
            nxt = next((l for l in lines[i + 1:i + 4] if "jsr" in l), "")
            if calls and "a6@" in nxt:
                found.append((func, i + 1, ", ".join(calls), nxt.strip()))
            armed, calls = None, []
    return found


def main():
    bad = 0
    for path in sys.argv[1:]:
        for func, line, calls, osc in scan(path):
            print(f"{path}:{line}: {func}: {calls} called between loading a6 and '{osc}'")
            bad += 1
    if bad:
        print(f"{bad} inline OS call(s) can receive clobbered arguments")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
