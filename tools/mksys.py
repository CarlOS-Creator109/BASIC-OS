#!/usr/bin/env python3
"""mksys.py - escribe la carpeta /SYS dentro de pc-basic.img (ver sys.h).

uso: mksys.py imagen.img NOMBRE=ruta [NOMBRE=ruta ...]
"""
import struct, sys

DIR_LBA, DIR_SECT, DATA_LBA, END_LBA = 168, 4, 172, 512

def main():
    img = sys.argv[1]
    items = [a.split("=", 1) for a in sys.argv[2:]]
    if len(items) > (DIR_SECT * 512 - 16) // 32:
        sys.exit("mksys: demasiados archivos")
    data = bytearray()
    ents = bytearray()
    lba = DATA_LBA
    for name, path in items:
        raw = open(path, "rb").read()
        nb = name.upper().encode()
        if len(nb) > 19:
            sys.exit("mksys: nombre muy largo: " + name)
        sects = (len(raw) + 511) // 512
        ents += nb.ljust(20, b"\0") + struct.pack("<III", lba, len(raw), 0)
        data += raw.ljust(sects * 512, b"\0")
        lba += sects
    if lba > END_LBA:
        sys.exit("mksys: /SYS no cabe (%d sectores de %d)" % (lba - DATA_LBA, END_LBA - DATA_LBA))
    d = (b"PCBSYS1\0" + struct.pack("<I", len(items)) + b"\0\0\0\0" + bytes(ents))
    d = d.ljust(DIR_SECT * 512, b"\0")
    with open(img, "r+b") as f:
        f.seek(DIR_LBA * 512)
        f.write(d)
        f.write(data)
    print("/SYS: %d archivos, %d de %d sectores" % (len(items), lba - DATA_LBA, END_LBA - DATA_LBA))

main()
