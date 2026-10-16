DOOM en BASIC-OS
================
PureDOOM.h   = motor Doom (https://github.com/Daivuk/PureDOOM, port de una sola cabecera del codigo fuente de id Software,
               licencia "DOOM Source Code License"). Cambio respecto al original: FixedDiv2() usa enteros de 64 bits en vez
               de double, porque el kernel de BASIC-OS no activa SSE (se compila con -mgeneral-regs-only).
doom1.wad    = DOOM shareware 1.9 (episodio 1, redistribuible libremente). Si tienes DOOM.WAD / DOOM2.WAD, copialo a la
               particion FAT de la imagen (junto a DOOM.BIN) y se usa ese en vez del shareware.

Construir:        make pc-basic-doom.img                      (usa tools/doom/doom1.wad)
                  make pc-basic-doom.img WAD=/ruta/DOOM.WAD   (otro WAD; se copia con su nombre 8.3 en mayusculas)
Probar en QEMU:   qemu-system-x86_64 -drive format=raw,file=pc-basic-doom.img,if=ide -m 128
USB real:         sudo dd if=pc-basic-doom.img of=/dev/sdX bs=4M conv=fsync

OJO con el nombre del WAD: el sistema de archivos de BASIC-OS solo lee nombres cortos 8.3. Si lo copias desde Linux/Windows
a la particion FAT, "doom.wad" o "DOOM.WAD" valen; un nombre largo (p. ej. "doom1_shareware.wad") no se encuentra.
Busca, en este orden: DOOM2.WAD, DOOM.WAD, DOOMU.WAD, DOOM1.WAD, en HD0.1: HD1.1: HD2.1: HD0: HD1: FD1: FD0: USB0.1: USB0: CD0:
