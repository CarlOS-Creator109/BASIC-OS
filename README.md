# PC BASIC

Un sistema operativo de **64 bits** (modo largo x86-64) sin base existente -- sin Linux,
sin libc, sin GRUB -- escrito en C y ensamblador, que arranca directo a un intérprete de
BASIC. Video gráfico VESA (framebuffer lineal) en la **resolución más alta que soporte el monitor**, texto blanco sobre
azul y paleta de 16 colores al estilo C64.

```
BIOS -> boot.asm (512 B) -> stage2.asm (modo real) -> entry.asm (32 -> 64 bits) -> kmain() en C -> BASIC
```

Al arrancar muestra los datos reales de la máquina:

```
PC BASIC V1.2 (64-BIT)

CPU:   AMD TURION(TM) II P520 DUAL-CORE MOBILE @ 2300 MHZ
RAM:   7616 MB
VIDEO: 800X600, 32-BIT COLOR

38911 BASIC BYTES FREE

READY.
```

## Compilar y probar

Requiere `gcc`, `nasm`, `ld`, `objcopy`, `python3` y `make`. En Arch:

```
sudo pacman -S --needed nasm gcc make python qemu-system-x86
```

```
make            # genera pc-basic.img (disquete de 1.44 MB)
make run        # la arranca en QEMU (qemu-system-x86_64)
make test       # prueba el intérprete en el host (con sanitizers), sin arrancar el SO
```

En QEMU hay que usar `qemu-system-x86_64` (o `-cpu qemu64`): `qemu-system-i386` emula una CPU
de 32 bits y PC BASIC se detiene con "SE NECESITA CPU DE 64 BITS".

### En hardware real (solo BIOS legacy / CSM)

```
sudo dd if=pc-basic.img of=/dev/sdX bs=512 conv=fsync    # CUIDADO: sdX es el USB
```

Necesita una CPU con x86-64. El teclado se lee por sondeo del controlador PS/2: el de una
laptop suele funcionar; uno USB solo si la BIOS tiene emulación de teclado legacy.

**Video:** `stage2.asm` recorre todos los modos VBE de la BIOS (16, 24 y 32 bits) y elige el de mayor
área que quepa en el monitor. La resolución nativa se lee del EDID (INT 10h AX=4F15h). Si la BIOS no da
EDID se limita a 1024x768, que cualquier monitor acepta; si la BIOS rechaza un modo, prueba el siguiente.
En pantallas de 1600 px de ancho o más el texto se dibuja al doble (o triple desde 2880 px). Si no hay
ningún modo VBE usable, cae a texto VGA de 80x25.

## Estructura

| Archivo | Qué hace |
|---|---|
| `boot.asm` | Sector de arranque: carga `stage2` (0x7E00) y el kernel (0x10000) |
| `stage2.asm` | Modo real: mapa de RAM (E820), copia la fuente 8x16 de la BIOS, elige el modo VBE, A20, GDT, modo protegido. Deja todo en `bootinfo` (0x6000) |
| `entry.asm` | Comprueba CPUID, arma la paginación (identidad 1 GB + framebuffer como write-combining vía PAT) y salta a 64 bits |
| `linker.ld` | Kernel enlazado en `0x10000` |
| `kernel.c` | `kmain()`, capa de plataforma (`plat_*`) y las direcciones mágicas del C64 |
| `vga.c` | Consola: dibuja la fuente en el framebuffer (o usa texto 80x25), paleta C64, códigos PETSCII, solo redibuja celdas que cambian |
| `sysinfo.c` | CPU (CPUID), MHz (TSC calibrado con el PIT), RAM (E820) y video para el banner |
| `audio.c` / `ac97.c` / `hda.c` | Motor de audio (voces, baterías, anillo DMA) y los drivers AC'97 e Intel HDA |
| `atapi.c` / `cd.c` | Lectoras de CD IDE (ATAPI por PIO) y reproducción de CD-DA (TOC + READ CD) |
| `sys.c` / `module.c` / `kapi.h` / `mod.ld` | Lectura de `/SYS` y cargador de módulos con su tabla de funciones |
| `sys/` + `icons/` + `tools/mksys.py` | `GUI.SYS`, `PONG/CALC/FILES.APP`, `pong.bas` y los `.icb`: van a `/SYS` de la imagen, no al kernel |
| `mouse.c` | Ratón PS/2 por sondeo; `vga_mouse()` (vga.c) dibuja el cursor, selecciona y pega |
| `pci.c` | Enumeración PCI (puertos 0xCF8/0xCFC) para el comando `PCI` y los futuros drivers |
| `store.c` / `thunk.asm` | Directorio y ranuras de SAVE/LOAD sobre sectores crudos; `thunk.asm` salta de modo largo a modo real para usar INT 13h |
| `bios.c` / `blk.c` | INT 13h desde modo largo; lista de discos `FDn/HDn/CDn/USBn` (`DRIVES`) |
| `fs.c` | Lectura de FAT12/16/32 e ISO9660 sobre cualquier disco |
| `usb.c` / `mmio.c` | Driver EHCI + almacenamiento masivo (BOT/SCSI); `map_mmio` mapea registros sin caché |
| `hda.c` | Audio Intel HDA: CORB/RIRB, descubre la ruta pin→DAC y reproduce un tono por DMA |
| `kbd.c` | Teclado PS/2 (set 1, distribución español ISO: Shift+2 = `"`, AltGr+2 = `@`, ñ ç ¿ ¡, AltGr para `# | ~ [ ] { } \`; sin teclas muertas). Letras en MAYÚSCULAS; Shift o Bloq Mayús = minúsculas. Cursor parpadeante |
| `lib.c` | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen` |
| `basic.c` | El intérprete de BASIC (sin dependencias de hardware) |
| `sys/pkg.c` | PKG.APP: tienda de programas (lista, descarga por HTTPS, SHA-256, ejecuta) |
| `sys/install.c` | INSTALL.APP: instala el sistema en otro disco o en el mismo USB (particion + FAT16 "BASIC-OS FS") |
| `sys/doomapp.c` / `sys/doom.c` | DOOM: `DOOM.APP` (en /SYS) carga `DOOM.BIN` (motor PureDOOM + pantalla/teclado/raton) de la partición FAT; ver sección DOOM |
| `test/` | Arnés que compila `basic.c` en el host y compara salidas |

## El BASIC

Enteros de 32 bits, variables `A`-`Z`, hasta 304 líneas. Mensajes de error al estilo
clásico (`?SYNTAX  ERROR IN 10`, `BREAK IN 20`, ...). **ESC** interrumpe un programa.

**Comandos:** `RUN [n]`, `LIST`, `NEW`, `CLS`, `HELP`, `REBOOT`

**Sentencias:** `PRINT` (o `?`), `LET`, `IF cond THEN línea|sentencia`, `IF cond GOTO n`, `GOTO`,
`GOSUB`, `RETURN`, `FOR`/`TO`/`STEP`, `NEXT`, `INPUT ["texto";] A[,B...]`, `REM` (o `'`), `END`,
`STOP`, `COLOR fg[,bg]`, `POKE dir,valor`, `SLEEP ms`. Varias sentencias por línea con `:`.
`INPUT A$` y `A$ = ...` funcionan con cadenas.

**Gráficos** (coordenadas lógicas, origen arriba a la izquierda; `XMAX`/`YMAX` dan el máximo):
`PLOT x,y[,c]`, `LINE x1,y1,x2,y2[,c]`, `BOX x1,y1,x2,y2[,c[,relleno]]`, `CIRCLE x,y,r[,c]`.
El color `c` es del C64 (0-15); sin él se usa el color del texto. Los gráficos se dibujan sobre la pantalla:
el texto tapa los píxeles de su celda y `CLS` o el scroll los borran.

**Guardar y cargar:** `SAVE "nombre"`, `LOAD "nombre"`, `DIR`, `SCRATCH "nombre"` (o `DELETE`). Los programas se
guardan como texto en una zona reservada de la propia imagen de arranque (sectores 512 a 2759 de la imagen,
hasta 28 programas de 40 KB), así que **solo escribe en el USB desde el que arrancaste**, nunca en otros
discos. El kernel vuelve un instante a modo real para llamar a la BIOS (INT 13h, `thunk.asm`). Si vuelves a
grabar la imagen con `dd`, lo guardado se borra.

**Hardware:** `PCI` lista los dispositivos PCI (bus:dev.fn, ID de fabricante:dispositivo, tipo y marca): es la base para los drivers.

**Ratón** (PS/2, como la consola de los BSD): aparece un cursor de bloque sobre el texto. Arrastra con el
botón izquierdo para seleccionar (se copia al soltar) y pulsa el botón derecho o el central para pegar en la
línea de comandos. Los programas pueden leer `MOUSEX`, `MOUSEY` (mismas coordenadas que `PLOT`) y `MOUSEB`
(1 = izquierdo, 2 = derecho, 4 = central). Un ratón USB solo funciona si la BIOS lo emula como PS/2.

**Discos** (`DRIVES` los lista): disqueteras `FD0..`, discos ATA/SATA/IDE `HD0..` (por la BIOS, INT 13h con
EDD), lectoras de CD y memorias USB `CD0..`/`USB0..` (driver propio EHCI + mass storage, solo lectura).
Se leen FAT12/16/32 y ISO9660 (nombres 8.3, sin nombres largos):
`DIR "HD0:"`, `DIR "CD0:CARPETA"`, `TYPE "HD0:LEEME.TXT"`, `LOAD "HD1.2:PROG.BAS"` (`HDn.m` = partición m).
Cualquier nombre con `:` va a esos discos; sin `:` sigue siendo SAVE/LOAD en el USB de arranque.
USB: solo controladores EHCI (USB 2.0); dispositivos lentos/rápidos pasan al controlador compañero, que no
tiene driver; tampoco xHCI (USB 3).

**Sonido:** el motor de audio (`audio.c`) mezcla hasta 8 voces sobre un anillo DMA de 64 KB y lo manda por
Intel HDA o por AC'97 (la que haya; `AUDIO` dice cuál). Sin tarjeta, `BEEP` y las ondas usan el altavoz del PC.
- `BEEP [ms[,hz]]` (por defecto 200 ms, 880 Hz; espera a que termine).
- `SOUND onda,hz,ms` con onda = `SINE` (`SENOIDAL`), `SQUARE`, `TRIANGLE` (`TRIANGULE`), `SAW`. **No espera**:
  vuelve de inmediato y el sonido sigue mientras tu programa corre (se pueden sumar varias voces).
- `SOUND KICK,hz,ms` bombo, `SOUND SNARE,hz,ms` caja, `SOUND HIHAT,ms` charles (si no pones hz/ms usa valores por defecto).
- `SOUND DRUMKIT,hz,tipo,ms` batería automática: `hz` = tono del bombo, `tipo` 0 rock, 1 disco, 2 hip hop,
  3 drum and bass, `ms` = cuánto dura (el patrón se repite). Ej.: `SOUND DRUMKIT,60,1,8000`.
- `SOUND OFF` corta todo; `SNDWAIT` espera a que acaben las voces (ESC corta).
Las voces se alimentan desde el sondeo del teclado y las esperas (sin interrupciones): si el programa se queda
mucho tiempo sin tocar el teclado (un bucle larguísimo sin `INKEY$`) el sonido puede entrecortarse.

**CD de audio:** `CDTOC` lista las pistas, `CDPLAY [n]` toca desde la pista `n` hasta el final (por la tarjeta
de sonido; no hace falta cable analógico), `CDSTOP`, `CDEJECT`, `CDSTAT`; las funciones `CDTRACK` y `CDTIME`
dan la pista y los segundos. Lee los sectores crudos (READ CD) de lectoras ATAPI IDE (puertos clásicos, modo
IDE/compatible) o USB y remuestrea de 44.1 a 48 kHz. En QEMU no se puede probar con un disco real (solo con datos).

**Escritorio:** `DESKTOP` abre el escritorio gráfico (mismo azul de fondo) con iconos `.icb` de bloques de color:
Terminal (vuelve a BASIC; ESC también), Pong, Calculadora y Archivos. Clic o T/P/C/F. El explorador lista las
unidades y `SYS:`, entra en carpetas FAT/ISO, abre `.BAS` en BASIC, `.ICB` como imagen y el resto como texto.
`EXEC "PONG.APP"` abre una app sin pasar por el escritorio. `LOAD ""` carga un Pong de texto (`PONG.BAS`);
luego `RUN`, W/S mueven. `LOCATE col,fila` mueve el cursor de texto.

**/SYS:** el escritorio, las apps, los iconos y el Pong de texto **no están en el kernel**: son archivos dentro
de la imagen (sectores 168-511, de solo lectura), que el kernel carga como módulos (`GUI.SYS`, `*.APP`) con
una tabla de funciones (`kapi.h`). `DIR "SYS:"` los lista. Se arman con `tools/mksys.py` al construir la imagen.
**Iconos `.icb`** (ICono de BASIC-OS): texto; primera línea `ICB`, luego una fila por línea con un dígito hex por
bloque (color del C64 0-F) y `.` para transparente (hasta 24x24). Pon el tuyo en `icons/` y se incluye.

**Expresiones:** `+ - * / % MOD`, `= <> < > <= >=` (verdadero = -1), `AND OR XOR NOT`, `&HFF`.
**Funciones numéricas:** `ABS SGN SQR RND(n) PEEK(dir) FRE(0) LEN(a$) ASC(a$) VAL(a$) XMAX YMAX`.
**Cadenas:** variables `A$`-`Z$` (hasta 127 caracteres), `"literal"`, concatenación con `+`,
comparación (`= <> < > <= >=`) y `CHR$ STR$ LEFT$ RIGHT$ MID$(a$,i[,n]) INKEY$`.

### Colores y pantalla

- Colores con los números del C64 (0-15): `POKE 53280,c` borde, `POKE 53281,c` fondo,
  `POKE 646,c` color del texto, o `COLOR fg,bg`. Por defecto: texto 1 (blanco), fondo 6 (azul),
  borde 14 (azul claro).
- Códigos de control con `CHR$`: `147` limpiar, `19` home, `17/145/29/157` cursor,
  `18/146` reverso, y los colores `5 28 30 31 144 156 158 159 129 149 150 151 152 153 154 155`.
- Los números se imprimen con espacio inicial y final.

```basic
10 PRINT CHR$(147);CHR$(158);"HOLA DESDE 64 BITS"
20 FOR C=0 TO 15:POKE 53280,C:FOR T=1 TO 300:NEXT T:NEXT C
```

## Notas y límites

- `POKE`/`PEEK` acceden a memoria física directa (hasta 1 GB): un `POKE` mal puesto puede colgar la máquina.
- La memoria se reparte así: código del kernel desde 0x10000, bss y pila desde 0x100000, módulos de `/SYS` en 0x300000 (escritorio) y 0x380000 (apps).
- Sin interrupciones (todo por sondeo). Los programas viven en RAM; SAVE/LOAD solo en el USB de arranque, el resto de discos es de solo lectura.
- Aún no hay arreglos (`DIM`), `^` ni números de punto flotante.

## Entorno de ventanas (V1.2)

`DESKTOP` abre el escritorio **encima** de la consola (no la reemplaza; `SALIR`/ESC regresa con el texto intacto).

- Ventanas movibles, **redimensionables** (bordes/esquina), maximizar y cerrar; barra de tareas con botón por ventana y reloj.
- **Terminal en ventana**: mini consola real (hasta 2), ejecuta BASIC (PRINT, PLOT, LINE…). El WM sigue vivo mientras corre el programa.
- Apps (módulos `.APP` en /SYS): PONG, CALC, FILES. Atajos sin ventana enfocada: T/P/C/F/B.
- **Fondos** (botón FONDO o tecla B): liso (la terminal de abajo nunca se ve), azul, plasma, estrellas, barras, fuego.
- Requiere ~48 MB de RAM (pool de capas en 0x1000000).

### Framebuffers / capas
| FB | Uso |
|----|-----|
| FB0 | texto de la terminal (consola principal) |
| FB1 | área de dibujo de programas (PLOT) sobre el kernel |
| FB2, FB3 | libres para que las apps dibujen sobre sí mismas |

Más una capa interna del escritorio entre FB1 y FB2. Índices 0-15 (paleta C64), 255 = transparente. API de módulos: `K->layer(n)`, `K->dirty()`, `K->flush()`.

### ABI de módulos (kapi v2)
Cabecera `PCBM`, fin, `mod_main`, dirección base. Bases: GUI.SYS 0x300000, PONG 0x400000, CALC 0x480000, FILES 0x500000. Una app con ventana llama `K->wm_publish(&wapp)` y devuelve `MOD_WINAPP`; no se puede usar con `EXEC` fuera del escritorio.

### Pantalla de carga
Al arrancar muestra "BOS. BASIC OS": gabinete de torre beige amarillento con tornillos, chapas, rejilla, bahía de disquete y una barra de carga por segmentos. Mientras se ve, la consola no dibuja; al terminar aparece el texto normal.

## YAAI (You Are An Idiot)
Programa de broma: un video convertido a texto (120x45 caracteres, 12.5 cuadros/s, glifos elegidos comparando cada celda 8x16 con la fuente VGA) y su audio en ADPCM IMA de 4 bits a 8 kHz. Escribe `YAAI` (o `EXEC "YAAI.APP"`) con `yaai.img` puesto como segundo disquete (FD1). Necesita pantalla de al menos 120x45 caracteres (1024x768 o más), ≥ 32 MB de RAM y tarjeta HDA/AC'97 para el sonido; ESC sale.
- `YAAI.APP` (módulo de /SYS, base 0x580000) lee `FD1:YAAI.DAT`, descomprime el video (inflate propio) y usa los ms de audio ya sonados como reloj.
- Kernel: `K->pcm_play/pcm_stop/pcm_ms` (flujo PCM mezclado en el anillo DMA) y `K->vid` (video de texto), `K->print`.
- `tools/yaai/enc.py` y `pack.py` regeneran `YAAI.DAT` desde un video; `tools/mkfat12.py` arma el disquete.
- Los módulos que no son `GUI.SYS` ya no abren el escritorio al ejecutarse con `EXEC`.

## BashTerminal (Linux sobre PC BASIC)
`BASHTERM` carga un **kernel Linux 6.12 real** y su initramfs (bash 5.2 + busybox, estáticos) desde la partición FAT del mismo disco/USB y **le entrega la máquina**: misma RAM (mapa E820), CPU, pantalla (el framebuffer VBE que ya dejó PC BASIC) y el mismo disco, que Linux monta en `/mnt`. Es un traspaso: no hay vuelta a PC BASIC sin reiniciar. Así Linux no desperdicia nada: es PC BASIC arrancando Linux, "linux-like".
- Imagen: `pc-basic-linux.img` (32 MiB, se arranca como disco duro/USB). LBA 0-4095 = PC BASIC (boot, kernel, /SYS, SAVE); partición 1 FAT16 `BASICOS` desde LBA 4096 con `BZIMAGE` e `INITRD.GZ`; lo demás es espacio libre que ven los dos sistemas.
- `sys/bashterm.c` (BASHTERM.APP, base 0x600000): lee BZIMAGE a 0x1000000, copia el núcleo a 0x2000000, carga INITRD.GZ sobre `init_size`, arma `boot_params` (E820, `screen_info` VLFB, línea de comandos), pone una GDT con `__BOOT_CS=0x10`/`__BOOT_DS=0x18` y salta a `startup_64` (entrada de 64 bits +0x200). Necesita ~64 MB de RAM.
- Linux: tinyconfig + fbcon/vesafb, teclado PS/2 y USB HID, EHCI/OHCI/UHCI/xHCI, AHCI/PIIX/PATA, usb-storage, VFAT. Consola serie en ttyS0 (debug) y tty0.
- En Linux: teclado latinoamericano por defecto (`kmap us` / `kmap la`), `poweroff`, `reboot`, `/mnt` = el USB/IMG.
- Receta para regenerarlo: `tools/linux/` (`build.sh`, `kernel.frag`, `init`) y `tools/mkbashimg.c` (`cc -O2 -o tools/mkbashimg tools/mkbashimg.c`).
- QEMU: `qemu-system-x86_64 -drive format=raw,file=pc-basic-linux.img,if=ide -m 128`
- USB: `sudo dd if=pc-basic-linux.img of=/dev/sdX bs=4M conv=fsync` (¡borra el USB!).

### Tiny Core Linux (CorePlus) sobre PC BASIC
`TINYCORE` hace lo mismo que `BASHTERM` pero con **Tiny Core 17.1 CorePlus (x86, 32 bits)** como instalación "frugal" en la partición FAT: `BOOT/VMLINUZ` + `BOOT/CORE.GZ` y las extensiones en `CDE/` (escritorio FLWM/JWM/IceWM/Fluxbox/Openbox, wifi con firmwares, instalador `tc-install`, gestor `tce`). PC BASIC hace de bootloader.
- `[EDITION=core|tinycore|coreplus] tools/linux/mktinycore.sh [salida.img] [MiB]` baja `<Edicion>-current.iso` (una imagen por edicion; por defecto CorePlus) de tinycorelinux.net y arma `pc-basic-tinycore.img` (384 MiB por defecto, dentro de la meta de 700 MB). Necesita aria2c (16 conexiones, reanuda si se corta: `aria2c -x16 -s16 -c`), bsdtar y un compilador de C (la FAT16 la escribe mkbashimg.c solo, ya no usa mtools ni dosfstools).
- Kernels de 32 bits: el módulo vuelve de modo largo a modo protegido sin paginación (GDT plana, `CR0.PG=0`, `EFER.LME=0`, `CR4.PAE=0`) y salta al inicio del núcleo protegido con `ESI = boot_params`. Los de 64 bits siguen usando la entrada `startup_64`.
- Opciones editables sin recompilar: `TINYCORE.CFG` / `BASHTERM.CFG` en la partición (`kernel=RUTA`, `initrd=RUTA`, `cmdline=TEXTO`). Por defecto Tiny Core arranca con `loglevel=3 cde showapps desktop=flwm_topside kmap=qwerty/la-latin1 waitusb=10 tz=CST6 host=basic-os`.

## Red (NET)
`NET` carga `NET.BIN` (iPXE portado: drivers e1000/e1000e/rtl81xx/pcnet/virtio y muchos mas, TCP/IP, HTTP, **HTTPS**, DHCP, DNS, NTP, ping)
desde la particion FAT. Compilar con `tools/net/build.sh`; `mktinycore.sh` y `mkbashimg` lo incluyen si existe `tools/net/NET.BIN`.
`NET "dhcp;imgfetch -n x https://..."` ejecuta varios comandos separados por `;` (se para en el primero que falle). Dentro de BASIC las letras
salen en mayuscula: para minusculas en la cadena usa Shift (o escribe en la consola interactiva `NET`, donde es al reves).

**HTTPS** (hace falta para Render, GitHub...): `build.sh` embebe los certificados raiz del sistema (`/etc/ssl/certs/ca-certificates.crt`, 126 CAs de Mozilla;
`TRUST=otro.pem` usa otro paquete, `TRUST=none` los de fabrica de iPXE). Ademas, en `tools/net/overlay/interface/basicos/basicos.c`:
- **hora** del RTC (puertos CMOS 0x70/0x71): sin fecha correcta TLS rechaza todo certificado;
- **entropia** de respaldo (jitter del TSC en cada tic de 1024 Hz del RTC) para maquinas sin RDRAND, como el Turion II o QEMU `qemu64`;
- comando `bosstash <imagen> app|list`: copia lo bajado a memoria fija para `PKG`.

## Instalar (INSTALL)

`INSTALL` (o `EXEC "INSTALL.APP"`) copia el sistema del disco de arranque a otro disco, o al mismo USB:

1. Lee a RAM los primeros 4096 sectores (arranque, stage2, kernel, /SYS) y toda la particion FAT16: archivos, carpetas y nombres largos.
   Como ya esta todo en memoria, el destino puede ser el mismo USB del que arrancaste.
2. Crea una tabla de particiones con una particion FAT16 llamada **BASIC-OS FS**, con el tamano de cluster que le toque al disco
   (hasta ~2 GB de particion), y recoloca los archivos.
3. Escribe primero la particion, luego el kernel y al final el sector 0; cada bloque se relee y se compara.

Pide el numero del disco y que escribas su nombre (por ejemplo `HD1`) antes de borrar nada. Solo escribe en discos que ve la BIOS
(INT 13h): un USB de arranque lo es; el USB nativo (EHCI) y los CD no. Hace falta RAM hasta ~200 MB (QEMU: `-m 256`).
Probado en QEMU: disco vacio, y el mismo disco con subcarpetas y nombres largos (sha256 de todos los archivos iguales).
**Pruebalo primero en una memoria USB, no en el SSD con tu Linux.**

## Tienda de programas (PKG + hub)
`PKG` (o `EXEC "PKG.APP"`) baja por Ethernet y HTTPS la lista de programas de un **hub** (`hub/server.py`, pensado para Render, que a su vez los toma de la
carpeta `store/` de tu repo de GitHub), te deja elegir uno, comprueba su SHA-256 y lo ejecuta.
Si arrancaste de un disco FAT16 que ve la BIOS (el que crea `INSTALL`), PKG guarda cada programa en `HDn.1:/APPS/` y la proxima vez lo ejecuta desde el disco sin red (`+N` lo baja otra vez, `-N` lo borra). El kernel sigue siendo de solo lectura: el escritor FAT16 vive dentro de `PKG.APP`.
Los programas son modulos PCBM enlazados a **0x9000000**; `make store` compila `apps/*.c` a `store/*.BIN`. Necesita ~180 MB de RAM (QEMU: `-m 256 -nic user,model=e1000`).
La direccion del hub va en `HUB.CFG` (`make pc-basic-doom.img HUB_URL=https://...`) o se escribe con la tecla `U`; no hay una por defecto a proposito.
Guia completa de despliegue y seguridad: [`hub/README.md`](hub/README.md).

## DOOM
Icono **DOOM** en el escritorio (tecla **D**) o `EXEC "DOOM.APP"` desde BASIC. Corre **a pantalla completa**, sin ventana: escribe directo
en el framebuffer VBE (color real de 16/24/32 bits, escalado a 4:3 centrado) y devuelve la pantalla **exactamente como estaba**
(escritorio con sus ventanas, o consola de texto) al salir.

- **Motor:** [PureDOOM](https://github.com/Daivuk/PureDOOM) (código fuente de id Software, una sola cabecera) sin libc ni coma flotante. Ocupa ~0.4 MB, no cabe en `/SYS`:
  vive en la partición FAT como `DOOM.BIN` (módulo PCBM enlazado a 0x4000000, igual que `NET.BIN`) y `DOOM.APP` solo lo carga. El WAD se lee
  entero a RAM (0x5400000) y **se queda en caché**: la segunda vez que abres DOOM en la misma sesión entra al instante.
- **Imagen:** `make pc-basic-doom.img` (necesita `tools/doom/PureDOOM.h` y un WAD; trae el shareware `doom1.wad`). Si pones un `DOOM.WAD`/`DOOM2.WAD` en la
  partición FAT se usa ese. Los nombres deben ser 8.3. Detalles en `tools/doom/README.txt`.
- **RAM:** ~100 MB libres (el motor reserva 12 MB de zona + heap + WAD). En QEMU usa `-m 128`.
- **Teclas:** flechas = mover/girar · **Ctrl** disparar · **Espacio** abrir/usar · **Shift** correr · **Alt** esquivar · 1-7 armas · **Tab** mapa ·
  **Esc** menú · **F11** alterna WASD (W/S avanzar, A/D esquivar; ojo: con WASD activo los trucos `idkfa`... no funcionan) · **F12** salir al instante.
  Salir también con el menú de Doom (Esc, Quit Game, Y). Ratón PS/2: mover = girar, botón izq. = disparar.
- **Sonido:** al arrancar Doom muestra si hay tarjeta y suena un pitido de prueba (880 Hz). Efectos por la tarjeta Intel HDA o AC'97 (la que detecte `AUDIO`), mezclados con el motor de audio del kernel. Sin tarjeta, Doom corre mudo y oculta las opciones de sonido.
  **Música:** la sintetiza `sys/mussyn.h`, el `chip.c` de DoomProject (osciladores de 8 bits, ruido LFSR de NES, ADSR y bateria de CTTS) pasado a enteros; las tablas salen de `tools/gen_mustab.py` -> `sys/mustab.h`. PureDOOM avisa por `BASICOS_MUS_*` (parche en `tools/doom/PureDOOM.h`) y Doom junta musica + efectos a 22050 Hz. Sin guardar partidas (el FS es de solo lectura para el WAD).
- **Cambios en el kernel:** `K->rawin()` (kapi v3): entrada cruda de scancodes set 1 y ratón (`kbd.c`, `mouse.c`); mientras está activa el kernel no interpreta ni
  el teclado ni el ratón, así que el escritorio no se entera de nada. `gui.c`: icono DOOM, iconos en varias columnas si no caben, y al volver de un módulo de
  pantalla completa reactiva el gancho del escritorio y lo repinta. `K->stream()` (`audio.c`): cola de audio estéreo s16 (11025 Hz por defecto; `stream(4,0,hz)` la cambia) que la aplicación empuja y el kernel remuestrea
  a 48 kHz y suma a las voces (latencia ~100-150 ms).

