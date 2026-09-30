#!/usr/bin/env python3
"""Resolve the frames from a crash_handler backtrace to function names.

The handler prints each frame as "module + 0xoffset (0xabsolute)". A MinGW build
has no PDB, so dbghelp cannot name the frames at runtime, but the executable's
COFF symbol table is still there -- which is all `nm` needs.

This works on the *module-relative offset*, not the absolute address: the offset
does not depend on where the loader happened to place the image, so it can be
matched against nm's link-time addresses by adding the image base. Passing the
absolute address here would silently resolve to whatever section symbol sits
highest, which is why it is not accepted.

Usage:
    python tools/resolve_backtrace.py build/win/wetrix.exe < log.txt
    python tools/resolve_backtrace.py build/win/wetrix.exe 0x22e1c3 0x15109d
"""

import bisect
import re
import subprocess
import sys

# MinGW's default image base for x64. Read from the PE header at runtime rather
# than assumed, because a link with a different base would shift every symbol.
IMAGE_SIZE_LIMIT = 0x10000000

FRAME_RE = re.compile(r"\+\s*0x([0-9a-fA-F]+)")


def image_base(exe):
    out = subprocess.run(
        ["objdump", "-p", exe], capture_output=True, text=True, check=True
    ).stdout
    for line in out.splitlines():
        if line.strip().lower().startswith("imagebase"):
            return int(line.split()[-1], 16)
    raise SystemExit(f"could not find ImageBase in {exe}")


def load_symbols(exe):
    out = subprocess.run(
        ["nm", "--numeric-sort", "--defined-only", exe],
        capture_output=True,
        text=True,
        check=True,
    ).stdout

    symbols = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        symbols.append((address, parts[2]))

    symbols.sort()
    return symbols, [s[0] for s in symbols]


def demangle(names):
    if not names:
        return {}
    out = subprocess.run(
        ["c++filt"], input="\n".join(names), capture_output=True, text=True
    ).stdout
    return dict(zip(names, out.splitlines()))


def collect_offsets(argv, stdin_text):
    """Offsets from argv if given, else every '+ 0x...' in the piped log."""
    if len(argv) > 2:
        return [int(token, 16) for token in argv[2:]]

    offsets = [int(match, 16) for match in FRAME_RE.findall(stdin_text)]
    if not offsets:
        raise SystemExit("no '+ 0x<offset>' frames found on stdin")
    return offsets


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    exe = sys.argv[1]
    base = image_base(exe)
    symbols, addresses = load_symbols(exe)

    offsets = collect_offsets(sys.argv, sys.stdin.read())

    names = []
    rows = []
    for offset in offsets:
        if offset >= IMAGE_SIZE_LIMIT:
            rows.append((offset, f"<0x{offset:x} is too large to be an offset -- "
                                 f"pass the '+ 0x...' value, not the absolute address>", 0))
            names.append("?")
            continue
        address = base + offset
        index = bisect.bisect_right(addresses, address) - 1
        if index < 0:
            rows.append((offset, "?", 0))
            names.append("?")
            continue
        symbol_address, symbol = symbols[index]
        rows.append((offset, symbol, address - symbol_address))
        names.append(symbol)

    demangled = demangle([name for _, name, _ in rows])

    for offset, symbol, delta in rows:
        print(f"+0x{offset:<8x} {demangled.get(symbol, symbol)} + 0x{delta:x}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
