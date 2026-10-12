#!/usr/bin/env python3
"""Extract PTX (and optionally cubin) entries from CUDA fatbins embedded in a PE/ELF file.

Usage: fatx.py <file> <outdir> [--cubin]

Fatbin layout (reverse engineered, see ZLUDA's fatbin handling):
  header: u32 magic 0xBA55ED50, u16 version 1, u16 header_size 16, u64 size of entries
  entry : u16 kind (1=PTX, 2=cubin), u16 version, u32 header_size, u32 padded_payload_size,
          u32, u32 payload_size, u32, u32, u32 sm_version, u32, u32, u64 flags, u64,
          u64 uncompressed_size        (flags & 0x2000 -> LZ4 block compressed payload)
"""
import os
import struct
import sys

MAGIC = b'\x50\xed\x55\xba'
ENTRY = struct.Struct('<HHIIIIIIIIIQQQ')
FLAG_LZ4 = 0x2000


def lz4_block(src, out_size):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]; i += lit
        if i >= n or len(dst) >= out_size:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += 4
        start = len(dst) - off
        if off == 0 or start < 0:
            raise ValueError('bad LZ4 match offset')
        for k in range(ml):
            dst.append(dst[start + k])
    return bytes(dst[:out_size])


def extract(path, outdir, want_cubin=False):
    data = open(path, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    pos = nfat = nptx = 0
    while True:
        pos = data.find(MAGIC, pos)
        if pos < 0:
            break
        ver, hsz, fsz = struct.unpack_from('<HHQ', data, pos + 4)
        if ver != 1 or hsz != 16 or fsz > len(data):
            pos += 4
            continue
        nfat += 1
        e, end, idx = pos + hsz, pos + hsz + fsz, 0
        while e + ENTRY.size <= end:
            (kind, _ever, ehsz, padded, _u0, psz, _u1, _u2, sm, _u3, _u4,
             flags, _u5, unc) = ENTRY.unpack_from(data, e)
            if ehsz == 0 or padded == 0:
                break
            payload = data[e + ehsz: e + ehsz + padded]
            payload = payload[:psz] if psz else payload
            if flags & FLAG_LZ4:
                payload = lz4_block(payload, unc)
            if kind == 1:
                nptx += 1
                with open(f'{outdir}/f{nfat:02d}_e{idx:02d}_sm{sm}.ptx', 'wb') as f:
                    f.write(payload.rstrip(b'\0'))
            elif kind == 2 and want_cubin:
                with open(f'{outdir}/f{nfat:02d}_e{idx:02d}_sm{sm}.cubin', 'wb') as f:
                    f.write(payload)
            idx += 1
            e += ehsz + padded
        pos = end
    return nfat, nptx


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    nfat, nptx = extract(sys.argv[1], sys.argv[2], '--cubin' in sys.argv[3:])
    print(f'{sys.argv[1]}: {nfat} fatbins, {nptx} PTX modules')
