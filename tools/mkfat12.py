#!/usr/bin/env python3
"""mkfat12.py - crea una imagen de disquete FAT12 de 1.44 MB: mkfat12.py salida.img ETIQUETA ARCHIVO..."""
import struct, sys, os

out, label, files = sys.argv[1], sys.argv[2], sys.argv[3:]
SS, TOT, FATSZ, ROOT = 512, 2880, 9, 224
img = bytearray(TOT * SS)
bs = bytearray(SS)
bs[0:3] = b"\xEB\x3C\x90"
bs[3:11] = b"MSDOS5.0"
struct.pack_into("<HBHBHHBHHHII", bs, 11, SS, 1, 1, 2, ROOT, TOT, 0xF0, FATSZ, 18, 2, 0, 0)
struct.pack_into("<BBBI", bs, 36, 0, 0, 0x29, 0x12345678)
bs[43:54] = label.upper().encode().ljust(11)[:11]
bs[54:62] = b"FAT12   "
bs[510:512] = b"\x55\xAA"
img[0:SS] = bs
fat = bytearray(FATSZ * SS)
def setfat(n, v):
    o = n * 3 // 2
    if n & 1:
        fat[o] = (fat[o] & 0x0F) | ((v << 4) & 0xF0)
        fat[o + 1] = (v >> 4) & 0xFF
    else:
        fat[o] = v & 0xFF
        fat[o + 1] = (fat[o + 1] & 0xF0) | ((v >> 8) & 0x0F)
setfat(0, 0xFF0); setfat(1, 0xFFF)
root = bytearray(ROOT * 32)
data_start = 1 + 2 * FATSZ + ROOT * 32 // SS
nxt = 2
for i, path in enumerate(files):
    raw = open(path, "rb").read()
    name, _, ext = os.path.basename(path).upper().partition(".")
    nm = name.ljust(8)[:8].encode() + ext.ljust(3)[:3].encode()
    n = max(1, (len(raw) + SS - 1) // SS)
    first = nxt
    for k in range(n):
        c = nxt + k
        setfat(c, 0xFFF if k == n - 1 else c + 1)
        sec = data_start + (c - 2)
        img[sec * SS: sec * SS + SS] = raw[k * SS:(k + 1) * SS].ljust(SS, b"\0")
    nxt += n
    root[i * 32: i * 32 + 32] = nm + struct.pack("<BBBHHHHHHHI", 0x20, 0, 0, 0, 0, 0x5A8A, 0, 0, 0x5A8A, first, len(raw))
for f in range(2):
    img[(1 + f * FATSZ) * SS:(1 + (f + 1) * FATSZ) * SS] = fat
img[(1 + 2 * FATSZ) * SS:(1 + 2 * FATSZ) * SS + len(root)] = root
open(out, "wb").write(img)
print("%s: %d archivo(s)" % (out, len(files)))
