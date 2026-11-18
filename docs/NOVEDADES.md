# Novedades: instalador, PKG con disco, hub en Render y HTTPS

Resumen de todo lo agregado en esta etapa de BASIC-OS ("PC BASIC").

## 1. Resumen

| Novedad | Donde | Estado |
|---|---|---|
| Hub de programas en Render | `hub/server.py`, `render.yaml`, `store/` | Funciona (`https://basic-os.onrender.com`) |
| `INSTALL`: particion + FAT16 "BASIC-OS FS" | `sys/install.c` -> `INSTALL.APP` | Probado en QEMU |
| `PKG` guarda, ejecuta sin red, actualiza y borra | `sys/pkg.c` -> `PKG.APP` | Probado en QEMU; y contra Render real |
| Escritura de disco por BIOS (`kapi->blk`) | `blk.c`, `kapi.h`, `kernel.c` | Probado en QEMU |
| HTTPS con la cadena cruzada de Render | `tools/net/x509-cross-signed-roots.patch` | Funciona |
| Compila con GCC nuevo | `Makefile` (`-std=gnu17`), casts en `kernel.c` | Funciona |
| Kernel con mas espacio | `KSECT 224`, `/SYS` en LBA 232 | Ver aviso de compatibilidad |

## 2. Compilar

```
sudo pacman -S gcc nasm make python binutils   # Arch
make net                                       # compila NET.BIN (iPXE con HTTPS)
rm -f pc-basic-doom.img                        # NET.BIN y HUB.CFG no son dependencias de make
make pc-basic-doom.img HUB_URL=https://basic-os.onrender.com
qemu-system-x86_64 -m 256 -drive file=pc-basic-doom.img,format=raw -nic user,model=e1000
```

`-m 256` es necesario: INSTALL usa RAM desde 0xC000000 (~200 MB) y PKG hasta 0xB300000.

## 3. Mapa de la imagen (nuevo)

| LBA | Contenido |
|---|---|
| 0 | Sector de arranque (la tabla MBR en 0x1BE la anaden mkbashimg/INSTALL) |
| 1-4 | stage2 |
| 5 .. | kernel (`KSECT 224`, maximo 226 sectores) |
| 232-235 | directorio `/SYS` |
| 236-511 | datos de `/SYS` |
| 512 .. | area SAVE |
| 4096 | particion FAT16 "BASIC-OS FS" |

**Compatibilidad:** las imagenes hechas con el esquema anterior (`/SYS` mas abajo) no arrancan con el kernel nuevo. Hay que regenerarlas con `make`. Cambios en `tools/mksys.py` (`DIR_LBA, DIR_SECT, DATA_LBA, END_LBA = 232, 4, 236, 512`) y `sys.h` (`SYS_DIR_LBA 232`, `SYS_DATA_LBA 236`).

## 4. INSTALL

Comando `INSTALL` en BASIC (o `EXEC "INSTALL.APP"`), base 0x6C0000.

1. Lista los discos que ve la BIOS y pides el numero.
2. Confirmas escribiendo el nombre del disco (p. ej. `HD1`).
3. Lee a RAM los primeros 4096 sectores y toda la particion FAT16 (archivos, carpetas, nombres largos y fechas). Como esta todo en memoria, el destino puede ser **el mismo USB** del que arrancaste.
4. Crea la tabla de particiones y una particion FAT16 con etiqueta **BASIC-OS FS**. El tamano de cluster se calcula para el disco (spc 1..64, hasta 65000 clusters, particion maxima 4.160.000 sectores = ~2 GB). Se recolocan los archivos como flujo de bytes y se reparan las entradas `.` y `..`.
5. Escribe en orden: particion, kernel, y al final el sector 0. Cada bloque se relee y se compara (`wr`).

Funciones principales: `list_drives`, `load_head`, `open_source_fs`, `geometry`, `alloc`, `read_chain`, `clone_dir`, `build_fs`, `wr`, `do_write`, `mod_main`.

**Limites:** solo escribe en discos visibles por INT 13h (BIOS/legacy). El USB nativo (EHCI) y el CD son de solo lectura. No probado todavia en un USB real ni en el Vaio. **Pruebalo en una memoria USB, no en el SSD con Arch.**

## 5. API del kernel: `kapi->blk`

Campo nuevo al final de `kapi.h`:

```c
int (*blk)(const char *name, int op, unsigned long long lba, unsigned n, void *buf);
```

| op | Accion |
|---|---|
| 0 | leer `n` sectores |
| 1 | escribir `n` sectores |
| 2 | devolver el numero de sectores |
| 3 | banderas: 1 = arranque, 2 = escribible |

En `blk.c`: `bios_xfer(b, lba, n, buf, wr)` (INT 13h AH=0x42 lectura / 0x43 escritura, con buffer intermedio), `blk_write()` (rechaza USB nativo, CD y sectores que no son de 512 bytes) y `blk_api()`. En `kernel.c` el inicializador de la kapi termina con el cast `(int (*)(const char *, int, unsigned long long, unsigned, void *))blk_api`.

## 6. PKG con disco

- Si arrancaste de un FAT16 visible por la BIOS (el que crea INSTALL), cada programa se guarda en `HDn.1:/APPS/`.
- Menu: numero = ejecutar (del disco si esta, si no baja), `+N` = bajar otra vez (actualizar), `-N` = borrar, `R` = recargar lista, `U` = cambiar URL del hub, `Q` = salir.
- Sin red, PKG muestra y ejecuta lo guardado.
- El escritor FAT16 vive en `PKG.APP` (`fw_init`, `fw_wr`, `fat_set`, `fw_flush_fat`, `fw_alloc`, `fw_free_chain`, `fw_find`, `put_entry`, `fw_apps_dir`, `fw_save`, `fw_delete`). Actualiza la FAT antes que la entrada de directorio y verifica cada escritura leyendola de nuevo.
- Mapa de RAM: programa en 0x9000000, lista en 0xB000000, buzon 0xB100000, buffers del escritor 0xB200000-0xB250000, `NEED_RAM 0xB300000`.

## 7. Hub en Render

1. Sube el repo a GitHub; los programas van en `store/*.BIN` (`make store` compila `apps/*.c`).
2. En Render: **New > Blueprint** (usa `render.yaml`), o un Web Service con **Language = Python 3**, no Node.
3. Variables: `GH_REPO` (tu repo). `/list` debe mostrar los programas (hoy `HELLO.BIN`).
4. En el sistema: `make pc-basic-doom.img HUB_URL=https://tu-hub.onrender.com` o la tecla `U` en PKG.

Detalles y seguridad: `hub/README.md`.

## 8. HTTPS con Render (raiz cruzada)

Cadena de Render: hoja (P-256) <- WE1 (P-256) <- **GTS Root R4** (P-384), con firma cruzada de GlobalSign Root CA (RSA). iPXE solo reconoce raices por huella, y daba `Permission denied (0216eb3c)` ("No usable certificates").

`tools/net/x509-cross-signed-roots.patch` anade a `x509_validate_chain` un bloque que, para cada certificado de la cadena que no es raiz, busca en el almacen un certificado raiz de confianza con el mismo nombre y la misma llave publica, y lo sustituye. `tools/net/build.sh` aplica el parche (si no tiene la marca `BASICOS: cross-signed roots`) y embebe las raices de `/etc/ssl/certs/ca-certificates.crt`. Tambien incluye hora por RTC y entropia de respaldo (jitter del TSC).

## 9. Compilacion con GCC nuevo

- `-std=gnu17` en `CFLAGS` (el defecto C23 rompe el codigo).
- `incompatible-pointer-types` es error: casts explicitos para `map_mmio` y `blk_api` en la kapi.
- Falta de `nasm`: `sudo pacman -S nasm`.
- "El kernel no cabe": subir `KSECT` (ahora 224).

## 10. Pruebas hechas

- QEMU: INSTALL en disco vacio y sobre el mismo disco (subcarpetas, nombres largos; sha256 de todos los archivos iguales), arranque desde el disco instalado.
- QEMU: PKG guardar, ejecutar sin red, borrar; prueba de estres del escritor FAT16.
- TLS con una cadena igual a la de Render; PKG contra Render real funciona.

## 11. Pendiente

- Probar INSTALL en un USB real (BIOS/legacy).
- Escritura en USB nativo (EHCI).
- Opcionales: suavizado/scanlines en Doom, reproductor de video (`PLAY.APP`), volumen separado musica/efectos, prueba con DOOM2.WAD.
