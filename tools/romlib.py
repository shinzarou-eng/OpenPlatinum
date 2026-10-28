# romlib.py - minimal NDS ROM reader shared by the audit tools.
#
# Walks the FNT (file name table) + FAT (file allocation table), then
# carves files out of the cartridge image. Also unpacks NARC archives.
# Pure python, no deps.

import struct


def _u16(d, o): return struct.unpack_from('<H', d, o)[0]
def _u32(d, o): return struct.unpack_from('<I', d, o)[0]


class Rom:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.fnt = _u32(self.data, 0x40)
        self.fat = _u32(self.data, 0x48)
        self.files = {}
        self._walk()

    def _walk(self):
        dirs = [(0xF000, '')]
        while dirs:
            did, prefix = dirs.pop(0)
            off, fid0, _ = struct.unpack_from('<IHH', self.data,
                                              self.fnt + (did & 0xFFF) * 8)
            p = self.fnt + off
            fid = fid0
            while True:
                b = self.data[p]; p += 1
                if b == 0:
                    break
                if b & 0x80:
                    n = b & 0x7F
                    name = self.data[p:p + n].decode('latin-1'); p += n
                    sub = _u16(self.data, p); p += 2
                    dirs.append((sub, prefix + '/' + name))
                else:
                    name = self.data[p:p + b].decode('latin-1'); p += b
                    self.files[prefix + '/' + name] = fid
                    fid += 1

    def file(self, path):
        s, e = struct.unpack_from('<II', self.data,
                                  self.fat + self.files[path] * 8)
        return self.data[s:e]


def narc(d):
    """NARC -> list of member bytes."""
    n = _u32(d, 0x18)
    fatb = 0x1C
    btnf = fatb + n * 8
    gmif = btnf + _u32(d, btnf + 4)
    base = gmif + 8
    return [d[base + _u32(d, fatb + i * 8):
              base + _u32(d, fatb + i * 8 + 4)] for i in range(n)]
