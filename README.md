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

## Versión de 32 bits (ARCH=32)

El mismo código se compila también para **x86 de 32 bits** (modo protegido plano, sin paginación). Sirve para CPUs
i686 y para emuladores de 32 bits como **v86**, que corre en el navegador.

```
make            # PC BASIC de 64 bits  -> pc-basic.img
make 32         # PC BASIC de 32 bits  -> pc-basic32.img  (se arma en build32/, no pisa los .o de 64 bits)
make run32      # la arranca en qemu-system-i386
```

Qué cambia en 32 bits: `entry32.asm` (GDT plana, sin PAE/LME), `thunk32.asm` (INT 13h desde modo protegido), `lib32.c`
(división de 64 bits, sin libgcc), `map_mmio` sin paginación (`mmio.c`) y `u64` ahora es `unsigned long long` en ambas.
El banner dice `(32-BIT)` o `(64-BIT)`.

No van en la imagen de 32 bits (necesitan modo largo): `BASHTERM.APP`, `TINYCORE.APP` (arrancan Linux de 64 bits),
`NET.APP` (iPXE de 64 bits) y `DOOM.APP` (motor de 64 bits). El resto (escritorio, PONG, CALC, FILES, YAAI, PLAY, SAY,
PKG, INSTALL) se compila para 32 bits.

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
| `ram.c` | Bancos de RAM de 32 MB (`K->ram`, comando `RAM`) y mapeo de toda la RAM sobre 1 GB |
| `kbd.c` | Teclado PS/2 (set 1) con tres distribuciones: ES (España), LA (latinoamericana) y US. Se elige al arrancar con las flechas o con `KEYB`; sin teclas muertas. Letras en MAYÚSCULAS; Shift o Bloq Mayús = minúsculas. Cursor parpadeante |
| `lib.c` | `memset`, `memcpy`, `memmove`, `memcmp`, `strlen` |
| `tools/cosmos/` + `sys/cosmosapp.c` | Navegador COSMOS: motor MM de Cosmos Browser (`html.c`, `layout.c`), libc mínima, pintado y red (`cosmosmm.c`) → `COSMOS.BIN` en la partición FAT; `COSMOS.APP` lo carga |
| `basic.c` | El intérprete de BASIC (sin dependencias de hardware) |
| `tools/mplay/` | MPLAY.BIN: reproductor en ventana con FFmpeg + capa Linux (syscalls de musl sobre la kapi); `av1/` = plugin MPLAYAV1.BIN (dav1d) |
| `sys/binicon.h` | icono de escritorio dentro de un `.BIN` de la tienda |
| `sys/wtk.h` | WTK: kit de ventanas (botones, cajas, listas, menús...) para apps y programas de la tienda |
| `sys/binwin.h` | aviso compartido de "programa de la tienda con ventana abierta" |
| `sys/foro.c` | FORO.APP: foro de texto (lista de hilos, leer, responder y abrir hilos por el hub) |
| `sys/fatw.h` | escritor FAT16 mínimo (guarda en `HDn.1:/APPS/`), compartido por PKG y BCC |
| `tools/bcc/` | BCC, el compilador de C: `bcc.c` (+ `bcc_pp.c`, `bcc_parse.c`, `bcc_gen.c`), la biblioteca en `lib/`, `bccos.c` = BCC.BIN, `bcc_host.c` = versión de Linux, `bccrun.c` = corre un .BIN en Linux |
| `sys/pkg.c` | PKG.APP: tienda de paquetes (lista, descarga por HTTPS, SHA-256, ejecuta `.BIN` o carga `.BAS`; **S** sube paquetes al hub) |
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
de la imagen (sectores 232-511, de solo lectura), que el kernel carga como módulos (`GUI.SYS`, `*.APP`) con
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
- **Fondos** (botón FONDO o tecla B): liso (la terminal de abajo nunca se ve), azul, plasma, estrellas, barras, fuego y
  **cubo** (el de por defecto): un cubo 3D con los colores de la terminal que gira y se inclina hacia donde está el ratón.
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
- comando `bospost <url>`: sube al hub (POST) lo que `PKG` dejó en memoria; necesita `tools/net/http-post.patch` (lo aplica `build.sh`).

> Resumen de las ultimas novedades: [`docs/NOVEDADES.md`](docs/NOVEDADES.md)

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

> HTTPS con Render: Render manda la raiz "GTS Root R4" con firma cruzada de GlobalSign; iPXE solo reconoce raices por huella, asi que `tools/net/build.sh` aplica `x509-cross-signed-roots.patch` (acepta un certificado cuyo nombre y llave publica coinciden con una raiz de confianza). Probado en QEMU con una cadena igual a la de Render.

## Atajos para gente de UNIX/Linux
`LS` = `DIR`, `CAT`/`MORE`/`LESS` = `TYPE`, `RM` = `SCRATCH`, `CLEAR` = `CLS`, `MAN` = `HELP`, `LSBLK`/`DF` = `DRIVES`,
`LSPCI` = `PCI`. Aceptan el argumento sin comillas: `LS HD0.1:`, `CAT HD0.1:LEEME.TXT`, `RM JUEGO`. `UNIX` muestra la
tabla de equivalencias; `CP`, `MV`, `MKDIR`, `CD`... avisan que no existen todavia (para bash de verdad: `BASHTERM`).

## Compilar BASIC a .BIN (bas2bin)
`tools/bas2bin` convierte un programa de PC BASIC en un `.BIN` de la tienda (módulo enlazado a 0x9000000) que PKG
ejecuta y que se puede subir al hub. Mismo código en Linux y dentro del sistema:

```
make bas2bin                                   # tools/bas2bin/bas2bin
tools/bas2bin/bas2bin JUEGO.BAS JUEGO.BIN      # nativo (por defecto)
tools/bas2bin/bas2bin -p JUEGO.BAS JUEGO.BIN   # empaquetado
```
En el sistema: `PKG`, tecla **C**, el nombre de un programa guardado con `SAVE` o la ruta de un `.BAS`
(`HD0.1:DEMO.BAS`), y el modo. El `.BIN` queda en `HDn.1:/APPS/` y aparece en la lista; con **S** se sube.

- **Nativo:** traduce el BASIC a código x86-64 (unas 30 veces más rápido que el intérprete en bucles). Compila
  aritmética, `IF/GOTO/GOSUB/FOR/NEXT`, `PRINT`, `INPUT`, cadenas y sus funciones. Las sentencias que no conoce
  (`PLOT`, `SOUND`, `LOCATE`, `POKE`, `CLS`...) y funciones como `PEEK` o `XMAX` las calcula en nativo y se las
  pasa al intérprete ya con números (kapi 5: `basic_do`/`basic_eval`). Errores como `GOTO` a una línea que no existe
  o mezclar números y cadenas salen al compilar, no al correr. Solo PC BASIC de 64 bits.
- **Empaquetado:** el `.BIN` lleva el texto y lo corre el intérprete: todo el lenguaje, misma velocidad. Funciona
  también en 32 bits.

## Tienda de programas (PKG + hub)
`PKG` (o `EXEC "PKG.APP"`) baja por Ethernet y HTTPS la lista de programas de un **hub** (`hub/server.py`, pensado para Render, que a su vez los toma de la
carpeta `store/` de tu repo de GitHub), te deja elegir uno, comprueba su SHA-256 y lo ejecuta.
Si arrancaste de un disco FAT16 que ve la BIOS (el que crea `INSTALL`), PKG guarda cada programa en `HDn.1:/APPS/` y la proxima vez lo ejecuta desde el disco sin red (`+N` lo baja otra vez, `-N` lo borra). El kernel sigue siendo de solo lectura: el escritor FAT16 vive dentro de `PKG.APP`.

**Buscar (cuando hay muchos paquetes):** la lista sale de 12 en 12 (las flechas, o `N` y `A`, cambian de página). `F texto` filtra por nombre o descripción (`F` solo quita el filtro), `T` lista las etiquetas con cuántos paquetes tiene cada una y `T juego` muestra solo esa. La etiqueta es el `[texto]` con que empieza la descripción al subir (`[juego] Mi juego`). Los números no cambian al filtrar: son los de la lista completa. Hasta 400 paquetes.

**Paquetes:** `NOMBRE.BIN` (módulo, se ejecuta) o `NOMBRE.BAS` (programa BASIC en texto: PKG lo carga y sale; escribe `RUN`).
**Subir (tecla S en PKG):** escribe el nombre de un programa guardado con `SAVE` (se sube como `NOMBRE.BAS`) o la ruta de un
archivo de la partición (`HD0.1:APPS/MIAPP.BIN`), el nombre en el hub y una descripción. Viaja por HTTPS como POST a
`HUB/upload` (comando `bospost` de `NET.BIN`, máximo 2 MB). El hub pide la clave `HUB_KEY`: ponla como segunda línea de
`HUB.CFG` (`make super HUB_URL=https://... HUB_KEY=...`) o PKG te la pregunta. Ojo: quien tenga una imagen con la clave puede subir.
Los programas son modulos PCBM enlazados a **0x9000000**; `make store` compila `apps/*.c` a `store/*.BIN`. Necesita ~180 MB de RAM (QEMU: `-m 256 -nic user,model=e1000`).
La direccion del hub va en `HUB.CFG` (`make pc-basic-doom.img HUB_URL=https://...`) o se escribe con la tecla `U`; no hay una por defecto a proposito.
Guia completa de despliegue y seguridad: [`hub/README.md`](hub/README.md).

## Bancos de RAM ("salto de RAM", comando RAM)
La memoria alta se reparte en **bancos de 32 MB** a partir de `0x9000000`. Un programa pide N bancos seguidos y el kernel le da
los primeros que estén libres; si ya están ocupados salta a los siguientes, hasta que se acabe la RAM. Al terminar los devuelve.

- `RAM` (o `BANCOS`) en BASIC muestra el mapa: cuánta RAM hay, cuántos bancos están libres y quién tiene cada uno. Los huecos
  donde la BIOS dice que no hay RAM (por ejemplo el hueco PCI debajo de 4 GB) salen como `(SIN RAM)`.
- **Banco 0** (`0x9000000`) = `TIENDA`: el programa de la tienda que está corriendo. **Banco 1** (`0xB000000`) = `SISTEMA`: buzón de
  NET, lista de PKG, escritor FAT, tablas de PLAY y las tablas de página. Esos dos son fijos; del banco 2 en adelante se prestan.
- El kernel ahora mapea **toda** la RAM que reporta la BIOS (antes solo el primer GB), con páginas de 2 MB. `PEEK`/`POKE` también
  llegan ahí. En 32 bits el límite es lo que cabe debajo de 4 GB.
- Para los módulos: `K->ram(RAM_ALLOC, 0, n, "NOMBRE")` da la dirección del primer banco (0 si no hay), `K->ram(RAM_FREE, dir, 0, 0)`
  los devuelve, `RAM_FREEALL` devuelve todo lo de un dueño y `RAM_INFO` da el total, los libres, el tamaño y el final de la RAM (kapi v7).
  En C con BCC: `bos_ram(n)`, `bos_ram_free(p)` y `bos_ram_info(...)`.
- **MPLAY** ya pide sus bancos (hasta 1 GB, lo que haya libre; mínimo 64 MB) y los devuelve al cerrar, en vez de quedarse con toda la RAM.
- Lo siguiente: que PKG, FORO, COSMOS, PLAY y NET.BIN también pidan bancos (así se podrán usar mientras MPLAY está abierto) y que los
  `.BIN` traigan tabla de reubicación para correr en cualquier banco, no solo en el 0.

## Teclado (KEYB)
Al arrancar, después de la pantalla de carga, aparece un menú para elegir el teclado con las **flechas** y **Enter**:
**ES** (España, el de siempre), **LA** (latinoamericano) o **US** (Estados Unidos). Sale marcado el que elegiste la última vez y,
si no tocas nada, arranca solo con ese en 8 segundos. La elección se guarda en el disco de arranque (sector 514, el hueco libre
entre el directorio de `SAVE` y sus datos), así que se recuerda al apagar; desde un CD solo dura hasta reiniciar.

Para cambiarlo después: `KEYB` (abre el mismo menú), o directo `KEYB ES`, `KEYB LA`, `KEYB US`. Afecta a todo: BASIC, el escritorio,
FORO, COSMOS y los programas de la tienda. Todavía no hay teclas muertas (los acentos con ´ salen como apóstrofo).

## Foro de texto (FORO + hub + GitHub)
`FORO` (o `EXEC "FORO.APP"`) es un foro de texto que usa el mismo hub que PKG. El hub guarda cada hilo como un archivo
`foro/NNNNNN.txt` del repo de GitHub (un commit por mensaje), así que no necesita base de datos y todo queda a la vista en GitHub.

- **Leer:** la lista muestra los hilos con más actividad primero (12 por página, flechas o `+` / `-` para cambiar de página).
  Escribe el número para leer un hilo; se pausa cada pantalla (Enter sigue, Q sale).
- **Escribir:** `N` = hilo nuevo (título y mensaje), y dentro de un hilo `R` = responder. El mensaje se escribe en un editor de
  pantalla completa: el texto **pasa solo al siguiente renglón** (se corta por palabras), **Enter** hace un renglón nuevo (se pueden
  dejar renglones en blanco), **Retroceso** puede volver al renglón de arriba, **TAB envía** y **ESC dos veces** descarta. Abajo siempre
  se ve un contador `[312/1400]` que se pone rojo cerca del límite. La ñ, ¿, ¡ y º del teclado se mandan como UTF-8 (en GitHub y en
  el cliente SDL se ven bien). Tu nombre se pide una vez por sesión. Se usa la clave de `HUB.CFG` (2a línea), la misma de PKG.
- **Límites del hub:** 1500 letras por mensaje, 60 mensajes por hilo, 500 hilos, un mensaje cada 15 s y 20 por hora por conexión.
  Como la consola solo pinta ASCII, el hub quita los acentos al mandarlo al sistema (en GitHub se guarda el texto original).
- **Moderar:** borra o edita el archivo del hilo en GitHub; con el webhook el hub lo nota al instante.
- **Desde Linux (cliente SDL):** `make tools/forosdl` y `tools/forosdl` (usa `https://basic-os.onrender.com`; otro hub: `tools/forosdl https://mi-hub...`).
  Ventana con la lista de hilos, lectura con rueda del ratón, y escribir con acentos y ñ (`Ctrl+Enter` envía). Necesita SDL2, SDL2_ttf,
  una fuente monoespaciada y el programa `curl` (Arch: `sudo pacman -S sdl2 sdl2_ttf ttf-dejavu curl`). La clave la busca en
  `~/BASIC-OS/hub.key`, en `HUB.CFG` o en `BASICOS_HUB_KEY`, o te la pide; tu nombre y la clave se guardan en `~/.config/basicos-foro.cfg`.
  En GitHub el texto queda con acentos; la consola de BASIC-OS los ve sin acento. Los renglones largos se parten a 78 letras.
- Variables del hub: `FORO=0` lo apaga, `FORO_PATH` (carpeta, `foro`), `FORO_KEY` (si no, usa `HUB_KEY`), `FORO_OPEN=1`
  (escribir sin clave), `FORO_MIN_GAP` y `FORO_PER_HOUR`.

## Ventanas para los programas (WTK)
`sys/wtk.h` es el **kit de ventanas** de BASIC-OS: el programa declara sus controles y WTK los dibuja y atiende el teclado, el ratón
y el cambio de tamaño. Es una cabecera, así que sirve igual para las apps de `/SYS` y para los programas de la tienda (`.BIN`).

| Control | Función | Avisa (callback) |
|---|---|---|
| Etiqueta | `wt_label(x, y, texto)` | — |
| Botón | `wt_button(x, y, w, h, texto, cb)` | al pulsarlo (clic, Enter o Espacio) |
| Casilla | `wt_check(x, y, texto, &valor, cb)` | al marcarla o desmarcarla |
| Caja de texto | `wt_input(x, y, w, buffer, tamaño, cb)` | con Enter (lo escrito siempre está en el buffer) |
| Lista | `wt_list(x, y, w, h, renglones, n, &seleccion, cb)` | al cambiar la selección y con Enter o doble clic (`wt_ev`) |
| Barra de progreso | `wt_progress(x, y, w, h, &valor, maximo)` | — |
| Marco con título | `wt_box(x, y, w, h, titulo)` | — |
| Lienzo | `wt_canvas(x, y, w, h, dibujar, clic)` | clic y arrastre con coordenadas del lienzo |
| Menú | `wt_menu(titulo, opciones, n, cb)` | con el número de opción |

- **Anclas** (`wt_anchor(id, ...)`): `WT_FILLW` / `WT_FILLH` estiran hasta el borde, `WT_RIGHT` / `WT_BOTTOM` pegan el control a la derecha o abajo.
- **Más:** `wt_text`, `wt_color`, `wt_show`, `wt_enable`, `wt_items`, `wt_focus`, `wt_msg` (aviso con botón OK; el texto se parte solo),
  `wt_title`, `wt_close`, `wt_on_tick` (~60 veces por segundo), `wt_on_key`, `wt_on_close`, `wt_on_resize`, `wt_background`.
- **Teclado:** Tab cambia de control, flechas en listas y menús, Enter/Espacio pulsan. En las cajas, sin Shift = minúsculas.
- **Abrir la ventana:** `mod_main` hace `K = k;` y `return wt_window("Titulo", ancho, alto, redimensionable, armar);`, donde `armar()` crea los controles.
  El ejemplo completo es `apps/demowin.c` (`DEMOWIN.BIN` en la tienda): menú, caja de texto, lista de colores, lienzo para pintar con el ratón y barra animada.

**Programas de la tienda con ventana.** Si un `.BIN` devuelve `MOD_WINAPP`, PKG le pasa su ventana al escritorio. Desde la consola,
el escritorio se abre solo con esa ventana (lo mismo pasa con `EXEC "COSMOS.APP"` o cualquier app de ventana); desde una terminal del escritorio aparece al lado.
Mientras esa ventana está abierta, su código ocupa la memoria de la tienda (0x9000000), así que PKG, FORO, COSMOS y PLAY esperan
a que la cierres (`sys/binwin.h`). Solo puede haber un programa de la tienda con ventana a la vez.

## BCC: compilador de C (BASIC-OS COMPILER C)
BCC compila C a programas `.BIN` de la tienda, para portar programas sin reescribirlos en BASIC. Escribe el código x86-64
directo (sin ensamblador ni enlazador) y trae su propia biblioteca de C, así que un `.c` normal con `printf`, `malloc`, `strcpy`,
`fopen`... compila tal cual. Hay dos versiones del mismo compilador:

- **Dentro de BASIC-OS:** `store/BCC.BIN` (paquete de la tienda). Se corre desde PKG, pide el archivo (`HD0.1:/SRC/HOLA.C`), lo compila
  (unos 50 ms en QEMU) y guarda `HD0.1:/APPS/HOLA.BIN`; luego sale en la lista de PKG con su número. Los `#include "x.h"` se buscan en
  la misma carpeta del `.c`. Necesita ~180 MB de RAM (como PKG). Todavía no hay un editor de texto en el sistema: el `.c` se copia a la
  partición desde Linux con mtools: en `super-basicos.img` la FAT empieza en el byte 6291456
  (`mmd -i super-basicos.img@@6291456 ::/SRC` la primera vez y `mcopy -i super-basicos.img@@6291456 hola.c ::/SRC/HOLA.C`); en
  `pc-basic-doom.img` y `completo64.img` empieza en 2097152.
- **En Linux:** `make bcc` hace `tools/bcc/bcc` y `tools/bcc/bccrun`. `tools/bcc/bcc hola.c -o HOLA.BIN` da el mismo `.BIN`, y
  `tools/bcc/bccrun HOLA.BIN` lo corre en Linux (imita la consola, el teclado y los archivos) para probar sin arrancar el sistema.
  `make bcc-test` compila las pruebas de `tools/bcc/tests/` con gcc y con BCC y compara las salidas.

**Qué entiende:** `char`/`short`/`int`/`long` con y sin signo, `_Bool`, punteros, arreglos (también de varias dimensiones), `struct`,
`union`, `enum`, `typedef`, punteros a función, funciones variádicas (`stdarg.h`), `static`, `goto`, `switch`, valores iniciales con
`{...}` y `.campo =`, cadenas en UTF-8 (se ven bien en la consola: `printf("Año ñandú")`), y el preprocesador completo
(`#include`, `#define` con argumentos, `#`, `##`, `__VA_ARGS__`, `#if`/`#elif`/`#ifdef`, `#pragma once`).
**Todavía no:** `float`/`double`, pasar o devolver `struct` por valor (usa punteros), campos de bits y literales compuestos.
Los errores salen como `HOLA.C:12: error: falta un ';' antes de esto`.

**Biblioteca** (todos los encabezados estándar son el mismo, `lib/bcc.h`): `stdio` (`printf` y familia con `%d %u %x %o %c %s %p`, ancho,
precisión y `l`/`h`; `puts`, `getchar`, `fgets`, `scanf`/`sscanf` simples, `fopen`/`fgets`/`fread`/`fseek` **solo para leer**),
`stdlib` (`malloc`/`free`/`realloc`/`calloc`, `atoi`, `strtol`, `qsort`, `bsearch`, `rand`, `exit`), `string.h` completo, `ctype.h`,
`assert.h`, `time.h` (`clock`, `time` en segundos desde el arranque). Lo que se escribe en la consola sale sin Shift en minúsculas
(como en FORO). Solo entra al `.BIN` lo que el programa usa (un "hola mundo" pesa ~11 KB).

**Para BASIC-OS** (`#include <bos.h>`): `bos_basic("CIRCLE 100,100,50,7")` ejecuta cualquier sentencia de BASIC (así se dibuja:
`PLOT`, `LINE`, `BOX`, `CIRCLE`, `COLOR`, `LOCATE`, `CLS`, `SOUND`...), `bos_eval("XMAX", &w)` evalúa una expresión, `bos_key()` espera
una tecla, `bos_inkey()` sin esperar, `bos_esc()`, `bos_ms()`, `bos_sleep(ms)`, `bos_sound(...)`, `bos_mouse(&x, &y, &b)`,
`bos_read_file(...)`. Ejemplos en `tools/bcc/ejemplos/` (`hola.c`, `adivina.c`, `dibujo.c`).

**Cómo está hecho:** convención de llamada propia (argumentos en la pila de 8 en 8 bytes, resultado en `rax`), así `stdarg` es trivial;
para llamar al kernel está la intrínseca `__kcall(__KO_campo, ...)` (convención SysV sobre `struct kapi`), y una función marcada
`__callback` recibe argumentos SysV (para pasarle funciones al kernel). Cada programa tiene su propia pila de 1 MB en su `.bss` (la del
kernel es de 32 KB) y el montón va del final del programa hasta 0xB000000. La biblioteca está escrita en C (`lib/bcclib.c`) y viaja dentro
del compilador (`mklib.py` la convierte en `bcclib.h`); se compila junto con cada programa. El analizador sigue el estilo de chibicc.

## MPLAY: video con FFmpeg (paquete opcional de la tienda)
`MPLAY.BIN` es un reproductor de video hecho con **FFmpeg 7.1** que corre dentro de BASIC-OS como una **ventana del escritorio**,
con su propio **icono**. Se instala desde `PKG` (queda en `APPS` del disco) y desde ahí aparece el icono MPLAY en el escritorio.

- **Ventana:** lista de los videos que encuentra en los discos (en la raíz y en la carpeta `VIDEOS`). Barra con Abrir,
  reproducir/pausa, −10 s / +10 s, barra de avance (clic para saltar), tiempo y volumen. La ventana se puede mover y agrandar.
- **Teclas:** Espacio = pausa, ← / → = −10 / +10 s, ↑ / ↓ = volumen, A = abrir otro. Un clic en el video también pausa.
- **A todo color:** con la ventana activa, MPLAY le pide al kernel un "hueco" en el escritorio (`K->overlay`, kapi 6) y pinta el
  video directo en la pantalla. Sin foco se ve en los 16 colores del escritorio, con tramado.
- **Formatos:** MP4/MOV, MKV/WebM, AVI, OGG, FLV, MPEG-TS/PS, y audio suelto MP3, WAV, FLAC, AAC.
  **Video:** H.264, H.265 (HEVC), MPEG-4, MPEG-1/2, VP8, VP9, Theora, MJPEG, y AV1 con el plugin. **Audio:** AAC, MP3, MP2, Vorbis, Opus, FLAC, AC-3, PCM.
- **Plugin AV1 (`MPLAYAV1.BIN`):** va aparte, porque es otra librería (dav1d, ~1.9 MB). Se baja con `PKG` como cualquier paquete
  y MPLAY lo carga solo cuando un video es AV1 (YouTube manda AV1 muy seguido). Sin el plugin, MPLAY avisa y deja oír el audio.
  Los plugins siguen `tools/mplay/plugin.h`: un `.BIN` enlazado en 0xAA00000 que devuelve una tabla de funciones (abrir, mandar, sacar cuadros…).
- **RAM:** el archivo se carga entero en memoria. MPLAY pide bancos de RAM al kernel (hasta 1 GB de lo que esté libre, mínimo 64 MB) y
  los devuelve al cerrar; el video tiene que caber en eso con unos 60 MB de sobra (en QEMU usa `-m 512` o más).
- **Cómo funciona:** FFmpeg se compila con **musl** como si fuera para Linux. `tools/mplay/linux.c` es una capa de "Linux falso":
  `SYSCALL` cae en ella y traduce lo que pide musl a la kapi (`write` a la consola, `mmap` a sus bancos de RAM, `clock_gettime` a `K->ms`).
  En modo ventana musl se queda "residente": cada callback entra con `bos_enter()` y corre en una pila propia de 4 MB (la del kernel es de 32 KB).
- **Iconos para programas de la tienda:** cualquier `.BIN` puede traer su icono con `BIN_ICON("NOMBRE", "ICB\n...")` (`sys/binicon.h`).
  El escritorio mira la carpeta `APPS` y pone un icono por cada uno; con un clic lo carga y, si devuelve `MOD_WINAPP`, abre su ventana.
- **Compilar:** `make mplay` y `make mplayav1` (la primera vez bajan y compilan FFmpeg y dav1d; necesitan `musl-gcc`, `nasm`, `git`,
  y para dav1d `meson` y `ninja`). Salen `store/MPLAY.BIN` (~7 MB, se publica con git) y `store/MPLAYAV1.BIN`.

## Navegador COSMOS (Minimal Motor de Cosmos Browser)
Icono **COSMOS** en el escritorio (tecla **N**) o `EXEC "COSMOS.APP"` desde BASIC. Es una **ventana** del escritorio (se puede mover,
maximizar y cambiar de tamaño: la página se vuelve a maquetar al ancho nuevo).

- **Motor:** el *Minimal Motor (MM)* de Cosmos Browser (COS-Mods): `html.c` (HTML + CSS: selectores, cascada, variables, `@media`, `calc()`)
  y `layout.c` (bloques, líneas, listas, tablas, flex/grid básico) **tal cual**, copiados en `tools/cosmos/`. Lo que en Linux hacen `gfx.c`,
  `net.c` y `main.c` está en `tools/cosmos/cosmosmm.c`: texto con la fuente 8x16 del sistema (CP437, acentos y ñ incluidos; títulos grandes a 2x/3x,
  negrita con doble trazo), colores `#RRGGBB` al más cercano de los 16 del C64 (los fondos pálidos se tramean con dos colores) y corrección de
  contraste para que el texto siempre se lea. `libc.c` es la libc mínima que pide el motor (malloc con listas libres en un montón de 27 MB que se
  vacía de golpe al cambiar de página, `snprintf`, `strtod`...). El motor usa `float`: se compila con SSE2 y lo enciende en cada llamada (como SAY).
- **Red:** carga `NET.BIN` (iPXE) por su cuenta y le pasa `dhcp; set u0 ...; imgfetch ${u0}${u1}...; bosstash` (la URL va troceada en variables de
  iPXE, así no hay límite de 160 letras). HTTP y HTTPS (los mismos certificados que PKG). La salida de iPXE se guarda y aparece en la página de error.
  Cada página tarda ~3 s de más por el `dhcp`.
- **Archivos locales:** escribe `HD0.1:PAGINA.HTM` (o `USB0.1:...`) en la barra.
- **Barra:** `<` atrás · `>` adelante · `R` recargar · casa = inicio. Si escribes palabras sueltas (sin punto) se buscan en DuckDuckGo Lite;
  `algo.com` se abre como `https://algo.com`; `10.x`/`192.168.x`/`localhost` como `http://`.
- **Teclas:** **Enter** ir · **Tab** al siguiente campo de la página · con la página activa, empezar a escribir abre una dirección nueva ·
  **↑ ↓** desplazar · **Espacio** página siguiente · **←** o **Retroceso** atrás · **→** adelante. Ratón: clic en enlaces (la barra de estado
  muestra a dónde van), clic/arrastre en la barra de desplazamiento.
- **Videos:** un enlace a un `.BVC` no se abre como página: COSMOS lo baja (hasta 32 MB) y lo reproduce `PLAY` a pantalla completa;
  al terminar vuelves a la misma página. El hub tiene su lista en `https://TU-HUB/videos` (también en la página de inicio de COSMOS).
- **Formularios:** clic en un campo de texto (o **Tab** desde la página/barra, que salta al primer campo y luego al siguiente) y se
  escribe ahí, con borde azul y cursor; **Enter** envía el formulario (como GET: `accion?nombre=valor&...`, igual que un navegador).
  Las casillas (`checkbox`/`radio`) se marcan con un clic; un `<select>` cambia a la siguiente opción con cada clic; el botón de enviar
  también funciona. Los `<textarea>` aceptan Enter como salto de línea. Así ya se puede buscar en FrogFind o DuckDuckGo Lite desde la página.
  Los formularios con `method=post` se mandan como GET (iPXE solo sabe pedir con GET desde COSMOS).
- **No hay (todavía):** JavaScript, imágenes, hojas de estilo externas (`<link rel=stylesheet>`), POST y scroll horizontal.
  iPXE no entrega las páginas de error del servidor (404...), solo dice que falló.
- **Imagen:** `COSMOS.BIN` (~110 KB, enlazado a 0x7000000; montón 0x7400000-0x8F00000) va en la partición FAT de `pc-basic-doom.img`,
  `pc-basic-video.img` y `super-basicos.img`, junto a `NET.BIN`. Necesita ~180 MB de RAM por `bosstash` (QEMU: `-m 256 -nic user,model=e1000`).
  Solo 64 bits. Para probar sin internet: `python3 -m http.server 8000` en el host y en COSMOS `10.0.2.2:8000/`.

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


## Video (PLAY, formato .BVC)
Formato propio con gracia: **cada cuadro se dibuja del centro hacia afuera**, en circulos, y no por filas desde el borde. Los pixeles van guardados en ese orden
(`dx^2+dy^2` creciente), con ordenes de "pixeles nuevos" / "pixeles sin cambio" entre cuadros. Si la maquina va justa, lo que queda sin actualizar es el borde y no
el centro. El primer cuadro se revela despacio desde el centro. Audio: **PCM de 8 bits** sin signo, mono, intercalado cuadro por cuadro; sale por la tarjeta HDA/AC'97
(`K->stream`) y marca el reloj del video; sin tarjeta el reloj es el del sistema.
- Convertir (necesita `ffmpeg`): `make tools/mkbvc` y `tools/mkbvc entrada.mp4 salida.bvc [-w 240] [-h 180] [-f 20] [-r 11025] [-g] [-t 10]` (`-g` = gris de 8 bits, ideal para Bad Apple; maximo 640x480).
- Imagen: `make pc-basic-video.img VIDEO=salida.bvc`. En BASIC: `PLAY "SALIDA.BVC"` (o `EXEC "PLAY.APP"` para `VIDEO.BVC`). **Espacio** = pausa, **ESC** = salir.
- **Videos del hub:** el hub sirve los `.BVC` de la carpeta `videos/` del repo en `/videos`; en COSMOS un clic los reproduce (ver COSMOS).
  Para publicar uno: `tools/mkbvc entrada.mp4 NOMBRE.BVC` (nombre 8.3, hasta 32 MB) y súbelo a `videos/` con git, con `NOMBRE.TXT` como descripción.
- El archivo se carga entero a RAM (desde 0x8800000): en QEMU usa `-m 512` o mas. Formato completo en la cabecera de `sys/play.c`.
- Probado en QEMU (con y sin Intel HDA); no probado en hardware real.
- **Depurar en Linux sin reiniciar:** `make tools/bvcplay` (necesita SDL2: `sudo pacman -S sdl2`) y `tools/bvcplay video.bvc`. Espacio = pausa, flechas = cuadro a cuadro (izq/der) o ±5 s (arriba/abajo),
  `b` = simula una maquina lenta (`--budget 3000`: solo se pintan 3000 pixeles por cuadro, del centro hacia afuera), `r` = repite la revelacion del primer cuadro.
  `tools/bvcplay video.bvc --info [-v]` verifica el archivo y da estadisticas (bytes y % de pixeles nuevos por cuadro); `--dump N cuadro.ppm` guarda un cuadro.

## Voz (SAY, TTS por formantes)

`SAY "hola mundo"` habla el texto. Es un port de **FTTS** (el sintetizador de formantes estilo Klatt de Carlos, `ftts.c`) a un módulo de
`/SYS` sin libc: reglas de pronunciación del español → fonemas X-SAMPA → duraciones, acento, entonación y transiciones → fuente glotal +
cascada de resonadores F1..F5 + ruido paralelo → `K->stream()` (HDA o AC'97) a 22050 Hz. No necesita archivos de datos: los formantes
que `ftts.c` lee de espeak-ng (es-419) van incrustados (tabla `OV[]` de `sys/say.c`). La salida es prácticamente la de `ftts`
(correlación de envolvente 0.99 contra el binario de escritorio, mismo texto y voz).

```
SAY "hola mundo, esto es una prueba."       SAY A$              (cualquier expresión de cadena, máx. 127 caracteres)
SAY "@TEXTO.TXT"                            lee un archivo (UTF-8 o CP437; se busca en USB0.1:, HD0.1:, ...), hasta 64 KB
SAY                                         modo conversación: una frase por línea, línea vacía o ESC = salir
SAY "-v f -r 1.3 buenos dias"               opciones antes del texto
```

| opción | efecto |
|---|---|
| `-v m\|f\|c\|o\|w\|r` | voz: hombre (por defecto), mujer, niño, ogro, susurro, robot |
| `-r N` | velocidad (0.3 .. 4, por defecto 1) |
| `-p HZ` | tono base |
| `-l es\|ja\|xs` | idioma: español, japonés (romaji) o X-SAMPA directo |
| `-x` | muestra los fonemas y cuánto tardó la síntesis |

Los números se leen en voz alta (`25`, `1.250`, `3,5`, `-4`), y `% + = @ &` también. ESC corta la voz. Requiere tarjeta de audio (HDA / AC'97).

Detalles técnicos:
- El kernel se compila con `-mgeneral-regs-only` (sin SSE). `SAY.APP` se compila con SSE2 (`sys/say.o` tiene su propia regla en el Makefile) y
  **activa SSE por su cuenta** (CR0.EM/TS = 0, CR4.OSFXSR y OSXMMEXCPT = 1, MXCSR con flush-to-zero) al arrancar y antes de cada frase, porque
  una llamada a la BIOS (`thunk`) deja CR4 sin esos bits. Solo usa SSE2 (nada de SSSE3/SSE4: corre en un Turion II).
- Sin libm: `sin/cos/exp` son polinomios; sin libc: cadenas y memoria propias.
- Memoria: segmentos en 0xA000000, texto en 0xA200000, audio en 0xA900000 (necesita ≥ 192 MB de RAM). Se sintetiza por frases y se
  reproduce por tandas de ~25 s para que empiece a hablar enseguida.
- kapi: `K->stream(5, 0, g)` fija la ganancia del flujo (1..16; 6 por defecto, pensada para los efectos de Doom). SAY usa 1 y muestras a escala completa.
- La consola entrega MAYÚSCULAS: las opciones no distinguen caso. En el texto da igual.

## Licencia

Codigo propio: **GPL-2.0-or-later** (ver `LICENSE`). Los componentes de terceros (iPXE, espeak-ng, PureDOOM, doom1.wad, Linux/Tiny Core)
conservan su licencia: detalle en `LICENSES.md`.
