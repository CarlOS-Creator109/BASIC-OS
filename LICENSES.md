# Licencias

## Codigo propio de BASIC-OS

El codigo propio (kernel, `sys/`, `apps/`, `hub/`, herramientas) es software libre bajo la **GNU GPL version 2 o posterior (GPL-2.0-or-later)**.
El texto completo esta en `LICENSE`. Puedes usarlo, estudiarlo, modificarlo y redistribuirlo bajo esas condiciones.

```
BASIC-OS - sistema operativo con intérprete de BASIC
Copyright (C) COS-Mods (Carlos)

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.
This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
```

"o posterior" es a proposito: asi tu codigo se puede combinar con codigo GPL-3.0 (como los formantes de espeak-ng de `SAY.APP`).
Los programas que la gente sube al hub (`store/`, `liberty/`) son de sus autores y llevan la licencia que ellos pongan.

## Componentes de terceros

Este repo incluye o usa:

| Componente | Donde | Licencia |
|---|---|---|
| iPXE (NET.BIN se compila de el; los parches estan en `tools/net/overlay`) | `tools/net/` | GPL-2.0-or-later (algunas partes con UBDL). Si distribuyes `NET.BIN` ofrece tambien las fuentes (este repo + el commit de `tools/net/IPXE_REV`). |
| PureDOOM (parcheado) | `tools/doom/PureDOOM.h` | DOOM Source License de id Software / licencia de PureDOOM (Daivuk) |
| `doom1.wad` (DOOM shareware) | `tools/doom/` | Shareware de id Software: se puede redistribuir sin modificar, no vender. **No subas tu `DOOM.WAD` / `DOOM2.WAD` completos.** |
| Certificados raiz (se embeben en `NET.BIN`) | generados por `tools/net/build.sh` | Mozilla CA bundle (MPL-2.0) |
| FFmpeg 7.1 (libavformat, libavcodec, libswscale, libswresample, libavutil) dentro de `store/MPLAY.BIN` | se baja con `tools/mplay/build.sh` (la configuracion exacta esta ahi) | LGPL-2.1-or-later (sin partes GPL). MPLAY entero se distribuye bajo GPL-2.0-or-later con sus fuentes en este repo. |
| dav1d 1.5.1 (VideoLAN) dentro de `store/MPLAYAV1.BIN` | se baja con `tools/mplay/av1/build.sh` | BSD-2-Clause |
| musl libc (enlazada dentro de `store/MPLAY.BIN` y `store/MPLAYAV1.BIN`) | la del sistema (`musl-gcc`) | MIT |
| Linux (BZIMAGE) y Tiny Core (INITRD) para BASHTERM/TINYCORE | `tools/linux/` | GPL-2.0 y las de cada paquete |
| FTTS (motor de voz de `sys/say.c`) y los formantes de espeak-ng (es-419) incrustados en su tabla `OV[]` | `sys/say.c` | codigo de FTTS: del autor; los valores de formantes vienen de espeak-ng (GPL-3.0-or-later), asi que `SAY.APP` se distribuye bajo GPL-3.0-or-later |

BCC (`tools/bcc/`, `store/BCC.BIN`) es codigo propio del proyecto (GPL-2.0-or-later). Su analizador sigue la forma de
chibicc de Rui Ueyama (MIT), pero no copia su codigo; la biblioteca de C (`tools/bcc/lib/`) tambien es propia.
Ojo: las partes de esa biblioteca que usa un programa quedan dentro de su `.BIN`, asi que hoy un programa compilado con
BCC se distribuye bajo la GPL. Si se quiere que la gente pueda usar otra licencia para sus programas, hay que agregar
una excepcion para la biblioteca (como la "GCC Runtime Library Exception"); todavia no esta puesta.
