#!/usr/bin/env python3
"""mksuper.py - arma super-basicos.img: PC BASIC de 64 y de 32 bits en un solo disco con menu.

uso: mksuper.py salida.img super.bin completo64.img pc-basic32.img

completo64.img es una imagen de mkbashimg (pc-basic.img + particion FAT16 en el LBA 4096) con
todo lo que se quiera compartir: DOOM.BIN, el WAD, el video, BZIMAGE, INITRD.GZ, NET.BIN...
Esquema del disco resultante (mismos numeros que super.asm):
  LBA 0        MBR del menu, con la particion apuntando a la FAT compartida
  LBA 1..      etapa 2 (menu + gancho de INT 13h)
  LBA 4096     zona de PC BASIC 64 (sus primeros 4096 sectores)
  LBA 8192     zona de PC BASIC 32
  LBA 12288    la particion FAT16
"""
import os
import struct
import sys

SEC = 512
ZONE = 4096          # sectores de cada zona: lo que va antes de la FAT en una imagen de mkbashimg
B64, B32, PART = 4096, 8192, 12288


def die(msg):
    sys.exit("mksuper: " + msg)


def main():
    if len(sys.argv) != 5:
        die("uso: mksuper.py salida.img super.bin completo64.img pc-basic32.img")
    out, superbin, full64, img32 = sys.argv[1:]
    sb = open(superbin, "rb").read()
    if len(sb) < SEC or sb[510:512] != b"\x55\xaa":
        die(superbin + " no es un sector de arranque")
    if len(sb) - SEC > (B64 - 1) * SEC:
        die("la etapa 2 no cabe")

    with open(full64, "rb") as f:
        mbr64 = f.read(SEC)
    entry = mbr64[446:462]
    ptype = entry[4]
    pstart, psize = struct.unpack_from("<II", entry, 8)
    if mbr64[510:512] != b"\x55\xaa" or ptype == 0 or pstart != ZONE:
        die(full64 + " no parece una imagen de mkbashimg (particion FAT en el LBA 4096)")
    fsize = os.path.getsize(full64)
    if fsize < (pstart + psize) * SEC:
        die(full64 + " esta incompleta")

    d32 = open(img32, "rb").read()
    if len(d32) > ZONE * SEC or d32[510:512] != b"\x55\xaa":
        die(img32 + " no es una imagen de PC BASIC 32")
    if any(d32[446:510]):
        die(img32 + ": el sector de arranque no tiene libre la tabla de particiones")

    total = PART + psize
    with open(out, "wb") as o:
        o.truncate(total * SEC)
        # MBR: codigo del menu + una particion (la FAT compartida) para Linux
        mbr = bytearray(sb[:SEC])
        e = bytearray(entry)
        e[1:4] = b"\xfe\xff\xff"
        e[5:8] = b"\xfe\xff\xff"
        struct.pack_into("<II", e, 8, PART, psize)
        mbr[446:462] = e
        mbr[462:510] = bytes(48)
        o.seek(0); o.write(mbr)
        o.seek(SEC); o.write(sb[SEC:])
        # zona 64: tal cual (su propio MBR ya dice "FAT en el LBA virtual 4096")
        with open(full64, "rb") as f:
            o.seek(B64 * SEC); o.write(f.read(ZONE * SEC))
            # la FAT
            f.seek(ZONE * SEC)
            o.seek(PART * SEC)
            left = psize * SEC
            while left:
                b = f.read(min(left, 8 << 20))
                if not b:
                    die("lectura corta de " + full64)
                o.write(b); left -= len(b)
        # zona 32: su imagen + la misma tabla que la de 64, asi tambien ve la FAT
        z = bytearray(d32)
        z[446:510] = mbr64[446:510]
        o.seek(B32 * SEC); o.write(z)
    print("super: %s  (%d MB; 64 bits en LBA %d, 32 bits en LBA %d, FAT %d MB en LBA %d)"
          % (out, total * SEC >> 20, B64, B32, psize * SEC >> 20, PART))


main()
