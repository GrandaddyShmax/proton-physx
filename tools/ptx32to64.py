#!/usr/bin/env python3
"""PROTOTYPE: rewrite 32-bit-address PTX (nvcc 6.5-8.0 / PhysX / APEX) into 64-bit-address PTX.

Superseded by the C converter in src/translator (which also handles generic local/shared
pointers); kept as a reference. It is line based and only understands the PTX that nvcc
emitted for the PhysX runtime.

Model: the 32-bit app sees "fake" 32-bit device pointers; real address = BASE + zext(fake).
BASE is baked in as an immediate. Module-scope .global variables are moved out of the module
into the same window: their declarations are dropped and every reference becomes the fake
address assigned to them (the host side must copy initializers and answer cuModuleGetGlobal
with that address). Global and generic accesses through 32-bit registers get translated;
shared/local/const/param keep 32-bit state-space offsets (allowed in 64-bit PTX).

Not handled yet: generic pointers into local/shared memory
(cvta.local.u32 / cvta.shared.u32), surface references, .func ABI edge cases.

Usage: ptx32to64.py in.ptx out.ptx [base_hex] [first_global_fake_hex]
"""
import re
import sys

MEM_RE = re.compile(r'^(\s*(?:@!?%\w+\s+)?)((?:ld|st|atom|red)(?:\.[a-z0-9]+)*)(\s+)(.*)$')
ADDR_RE = re.compile(r'\[\s*([%\w$.]+)\s*(?:\+\s*(-?\w+))?\s*\]')
GVAR_RE = re.compile(r'^\s*(?:\.visible\s+|\.extern\s+|\.weak\s+)?\.global\s+(?!\.(?:texref|surfref|samplerref)\b)'
                     r'(?:\.align\s+(\d+)\s+)?\.(\w+)\s+([\w$]+)(?:\[(\d*)\])?\s*(=\s*\{?[^;]*\}?)?\s*;', re.M)
SPACES_KEEP = {'shared', 'local', 'const', 'param'}
TYPE_SIZE = {'b8': 1, 'u8': 1, 's8': 1, 'b16': 2, 'u16': 2, 's16': 2, 'f16': 2, 'b32': 4, 'u32': 4,
             's32': 4, 'f32': 4, 'b64': 8, 'u64': 8, 's64': 8, 'f64': 8}


def rewrite(src, base=0x7f0000000000, gfake=0xF0000000):
    """Return (new_ptx, {global_name: (fake_addr, size, initializer)})."""
    if '.address_size 32' not in src:
        return src, {}
    gvars = {}
    for m in GVAR_RE.finditer(src):
        align, ty, name, count, init = m.groups()
        size = TYPE_SIZE.get(ty, 4) * (int(count) if count else 1)
        align = int(align or TYPE_SIZE.get(ty, 4))
        gfake = (gfake + align - 1) & ~(align - 1)
        gvars[name] = (gfake, size, init)
        gfake += size

    def sym_fake(tok):
        name, _, off = tok.partition('+')
        if name in gvars:
            return gvars[name][0] + (int(off, 0) if off else 0)
        return None

    out = []
    need_prologue = False
    for line in src.splitlines():
        s = line.strip()
        if s.startswith('.address_size'):
            out.append('.address_size 64')
            continue
        if GVAR_RE.match(line):
            out.append('// moved to window: ' + s)
            continue
        if '.param' in s and '.ptr' in s:
            line = re.sub(r'\.ptr(\s+\.(global|shared|const|local))?(\s+\.align\s+\d+)?', '', line)
        if re.match(r'^\s*(\.visible\s+|\.weak\s+)?\.(entry|func)\b', line):
            need_prologue = True
        if need_prologue and s == '{':
            out += [line, '\t.reg .u64 %pxb;', '\t.reg .u64 %pxa;', f'\tmov.u64 %pxb, {base:#x};']
            need_prologue = False
            continue
        m = re.match(r'^(\s*(?:@!?%\w+\s+)?)mov\.u32\s+(%\w+),\s*([\w$]+(?:\+\d+)?)\s*;', line)
        if m and sym_fake(m.group(3)) is not None:
            out.append(f'{m.group(1)}mov.u32 {m.group(2)}, {sym_fake(m.group(3)):#x};')
            continue
        m = re.match(r'^(\s*(?:@!?%\w+\s+)?)cvta\.(to\.)?global\.u32\s+(%\w+),\s*([%\w$]+)\s*;', line)
        if m:
            f = sym_fake(m.group(4))
            srcop = f'{f:#x}' if f is not None else m.group(4)
            out.append(f'{m.group(1)}mov.u32 {m.group(3)}, {srcop};')
            continue
        m = MEM_RE.match(line)
        if m:
            pred, op, sp, rest = m.groups()
            mods = op.split('.')[1:]
            space = next((x for x in mods if x in SPACES_KEEP | {'global'}), None)
            am = ADDR_RE.search(rest)
            if space not in SPACES_KEEP and am:
                a, off = am.group(1), am.group(2)
                f = sym_fake(a)
                if f is not None:
                    out.append(f'\tadd.u64 %pxa, %pxb, {f:#x};')
                elif a.startswith('%r') and not a.startswith('%rd'):
                    out += [f'\tcvt.u64.u32 %pxa, {a};', '\tadd.u64 %pxa, %pxa, %pxb;']
                else:
                    out.append(line)
                    continue
                rest = rest[:am.start()] + (f'[%pxa+{off}]' if off else '[%pxa]') + rest[am.end():]
                out.append(f'{pred}{op}{sp}{rest}')
                continue
        out.append(line)
    return '\n'.join(out) + '\n', gvars


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x7f0000000000
    gf = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0xF0000000
    with open(sys.argv[1]) as f:
        text, gv = rewrite(f.read(), base, gf)
    with open(sys.argv[2], 'w') as f:
        f.write(text)
