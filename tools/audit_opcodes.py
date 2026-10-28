#!/usr/bin/env python3
# audit_opcodes.py - scan every scr_seq member against the opcode width table
# and report which opcodes the VM in cpp/src/script.cpp doesn't implement.
#
#   python tools/audit_opcodes.py <rom.nds> [header_id ...]
#
# Walking the stream needs the correct param widths: a wrong width makes
# the parser lose sync and report phantom opcodes. The table in
# cpp/src/op_table.inc was rebuilt from the ScrCmd_* bodies in
# pokeplatinum, so widths are exact. Unknown ops halt the scan of that
# function rather than guessing.
#
# End markers: ScrCmd_End=0x0002 and ScrCmd_Return=0x0024 terminate a
# function body; bytes after them belong to the next function.

import re, struct, sys
from romlib import Rom, narc

W = {'b': 1, 'h': 2, 'w': 4}

OP_TABLE = 'cpp/src/op_table.inc'
VM_SRC = 'cpp/src/script.cpp'
HEADERS = 'cpp/data/headers.bin'


def u16(d, p): return struct.unpack_from('<H', d, p)[0]
def u32(d, p): return struct.unpack_from('<I', d, p)[0]


def load_table():
    tbl = {}
    for m in re.finditer(r'\{0x([0-9A-Fa-f]+),"([^"]*)","([bhw]*)"\}',
                         open(OP_TABLE, encoding='utf-8').read()):
        tbl[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return tbl


def implemented_ops():
    return {int(m.group(1), 16)
            for m in re.finditer(r'case 0x([0-9A-Fa-f]+)', open(VM_SRC).read())}


def functions(d):
    """A script member = u32 jump table then function bodies."""
    fns, p = [], 0
    while p + 4 <= len(d):
        if u16(d, p) == 0xFD13:
            break
        fns.append(p + 4 + u32(d, p))
        p += 4
    fns.append(len(d))
    return fns


def scan(d, tbl):
    """Yield opcodes used but not implemented by the VM."""
    fns = functions(d)
    for i in range(len(fns) - 1):
        s, e = fns[i], fns[i + 1]
        if s >= len(d) or e > len(d) or s >= e:
            continue
        q = s
        while q + 2 <= e:
            op = u16(d, q); q += 2
            if op in (0x0002, 0x0024):
                break
            a = tbl.get(op)
            if a is None:
                break          # opcode absent de la table: on arrete ce corps
            for c in a[1]:
                q += W[c]
            if op not in IMPL:
                yield op


def hdr_name(hd, h):
    p = u32(hd, 4)
    for i in range(h):
        p = hd.index(b'\0', p) + 1
    e = hd.index(b'\0', p)
    return hd[p:e].decode(errors='replace')


def main():
    global IMPL
    rom = Rom(sys.argv[1] if len(sys.argv) > 1 else
              'rom/Pokemon - Version Platine (France).nds')
    scr = narc(rom.file('/fielddata/script/scr_seq.narc'))
    hd = open(HEADERS, 'rb').read()
    tbl = load_table()
    IMPL = implemented_ops()

    zones = {}
    for h in range(596):
        s = u16(hd, 8 + h * 32 + 6)
        if s >= len(scr):
            continue
        for op in scan(scr[s], tbl):
            if op is not None:
                zones.setdefault(h, set()).add(op)

    for h in sorted(zones):
        print('hdr%-3d scr[%-4d] %-38s %s' %
              (h, u16(hd, 8 + h * 32 + 6), hdr_name(hd, h),
               ' '.join('%04X' % x for x in sorted(zones[h]))))


if __name__ == '__main__':
    main()
