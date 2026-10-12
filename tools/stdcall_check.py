#!/usr/bin/env python3
"""Checks that a 32-bit DLL's __stdcall exports pop the same number of argument bytes as a reference DLL's.

    stdcall_check.py OURS.dll REFERENCE.dll

For every export of OURS that REFERENCE also has, the immediate of the first `ret` after the entry point
(`ret 8` = two dword arguments) must match: a wrong count corrupts the caller's stack. Exports that are
jump stubs (`jmp *table`, as in NVIDIA's nvcuda.dll) or reach another export first can't be read and are
skipped. Reads both files with llvm-objdump, changes nothing. Exit status 1 on any mismatch. Python 3.10+.
"""
import re
import subprocess
import sys


def pops(dll):
    head = subprocess.run(["llvm-objdump", "-p", dll], capture_output=True, text=True, check=True).stdout
    base = int(re.search(r"ImageBase\s+([0-9a-f]+)", head).group(1), 16)
    exports = re.findall(r"^\s+\d+\s+0x([0-9a-f]+)\s+(\S+)$", head.split("Export Table:")[1], re.M)
    dis = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", dll], capture_output=True, text=True,
                         check=True).stdout
    ins = [(int(a, 16), t) for a, t in re.findall(r"^\s*([0-9a-f]+):\s+(.*)$", dis, re.M)]
    at = {a: i for i, (a, _) in enumerate(ins)}
    entries = {base + int(rva, 16) for rva, _ in exports}
    out = {}
    for rva, name in exports:
        i = at.get(base + int(rva, 16))
        if i is None or ins[i][1].startswith("jmp"):
            continue
        for a, t in ins[i:i + 4000]:
            if a in entries and a != ins[i][0]:
                break
            m = re.match(r"ret[lw]?\b\s*(?:\$0x([0-9a-f]+))?", t)
            if m:
                out[name] = int(m.group(1), 16) if m.group(1) else 0
                break
    return out


def main():
    ours, ref = pops(sys.argv[1]), pops(sys.argv[2])
    common = sorted(set(ours) & set(ref))
    bad = [n for n in common if ours[n] != ref[n]]
    for n in bad:
        print(f"MISMATCH {n}: pops {ours[n]} bytes, reference {ref[n]}")
    print(f"{len(common) - len(bad)}/{len(common)} exports pop the same argument bytes as the reference"
          + (f"; {len(set(ours) - set(ref))} not readable there" if set(ours) - set(ref) else ""))
    return 1 if bad or not common else 0


if __name__ == "__main__":
    sys.exit(main())
