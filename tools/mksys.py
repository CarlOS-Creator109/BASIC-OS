#!/usr/bin/env python3
"""mksys.py - escribe la carpeta /SYS dentro de pc-basic.img (ver sys.h).

uso: mksys.py imagen.img NOMBRE=ruta [NOMBRE=ruta ...]
"""
import struct, sys

DIR_LBA, DIR_SECT, DATA_LBA, END_LBA = 312, 4, 316, 512   # antes del 312: el kernel (KSECT del Makefile)
# segunda zona: despues de las 16 ranuras de SAVE (520 + STORE_SLOTS*80 = 1800) y hasta el final del disquete.
# Las entradas de /SYS guardan el LBA de cada archivo, asi que el kernel los lee igual en cualquiera de las dos.
# Si cambias STORE_SLOTS (store.h) hay que mover esto y SYS_EXTRA_LBA (sys.h) con el.
EXTRA_LBA, EXTRA_END = 1800, 2880

def main():
    img = sys.argv[1]
    items = [a.split("=", 1) for a in sys.argv[2:]]
    if len(items) > (DIR_SECT * 512 - 16) // 32:
        sys.exit("mksys: demasiados archivos")
    zones = [[DATA_LBA, END_LBA, bytearray()], [EXTRA_LBA, EXTRA_END, bytearray()]]
    ents = bytearray()
    for name, path in items:
        raw = open(path, "rb").read()
        nb = name.upper().encode()
        if len(nb) > 19:
            sys.exit("mksys: nombre muy largo: " + name)
        sects = (len(raw) + 511) // 512
        for z in zones:                      # el primero en el que quepa
            if z[0] + len(z[2]) // 512 + sects <= z[1]:
                lba = z[0] + len(z[2]) // 512
                z[2] += raw.ljust(sects * 512, b"\0")
                break
        else:
            used = sum(len(z[2]) // 512 for z in zones)
            sys.exit("mksys: /SYS no cabe (%d sectores de %d)" % (used + sects, sum(z[1] - z[0] for z in zones)))
        ents += nb.ljust(20, b"\0") + struct.pack("<III", lba, len(raw), 0)
    d = (b"PCBSYS1\0" + struct.pack("<I", len(items)) + b"\0\0\0\0" + bytes(ents))
    d = d.ljust(DIR_SECT * 512, b"\0")
    with open(img, "r+b") as f:
        f.seek(DIR_LBA * 512)
        f.write(d)
        for z in zones:
            if z[2]:
                f.seek(z[0] * 512)
                f.write(z[2])
    print("/SYS: %d archivos, %d + %d de %d + %d sectores" % (len(items), len(zones[0][2]) // 512, len(zones[1][2]) // 512,
                                                              END_LBA - DATA_LBA, EXTRA_END - EXTRA_LBA))

main()
