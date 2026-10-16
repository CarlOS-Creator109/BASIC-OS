# Licencias de terceros

El codigo propio de BASIC-OS (kernel, `sys/`, `apps/`, `hub/`, herramientas) lo publica su autor; **elige y agrega aqui la licencia que quieras** (por ejemplo MIT).
Este repo incluye o usa:

| Componente | Donde | Licencia |
|---|---|---|
| iPXE (NET.BIN se compila de el; los parches estan en `tools/net/overlay`) | `tools/net/` | GPL-2.0-or-later (algunas partes con UBDL). Si distribuyes `NET.BIN` ofrece tambien las fuentes (este repo + el commit de `tools/net/IPXE_REV`). |
| PureDOOM (parcheado) | `tools/doom/PureDOOM.h` | DOOM Source License de id Software / licencia de PureDOOM (Daivuk) |
| `doom1.wad` (DOOM shareware) | `tools/doom/` | Shareware de id Software: se puede redistribuir sin modificar, no vender. **No subas tu `DOOM.WAD` / `DOOM2.WAD` completos.** |
| Certificados raiz (se embeben en `NET.BIN`) | generados por `tools/net/build.sh` | Mozilla CA bundle (MPL-2.0) |
| Linux (BZIMAGE) y Tiny Core (INITRD) para BASHTERM/TINYCORE | `tools/linux/` | GPL-2.0 y las de cada paquete |
