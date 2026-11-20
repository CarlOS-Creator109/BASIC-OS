# Makefile - PC BASIC
#
#   make          construye todas las versiones: pc-basic.img (64 bits), pc-basic32.img (32 bits),
#                 pc-basic-doom.img (con DOOM) y pc-basic-video.img (solo si existe el video, ver VIDEO)
#   make 64 / make 32 / make doom / make video   una sola version
#   make bas2bin  tools/bas2bin/bas2bin: compila .BAS a .BIN de la tienda (tambien esta en PKG, tecla C)
#   make super    super-basicos.img: 64 y 32 bits en un solo disco con menu, y una sola particion con
#                 DOOM, el video (si existe), Linux y NET.BIN que ven los dos (ver tools/super/super.asm)
#   make run      la arranca en QEMU
#   make test     prueba el interprete en el host (sin arrancar el SO)
#   make clean

KSECT   := 224                       # sectores reservados para el kernel (112 KB); max 226 (/SYS empieza en el LBA 232)
# ARCH=64 (por defecto): PC BASIC de 64 bits.  ARCH=32: variante de 32 bits (modo protegido, sin paginacion);
# corre en cualquier CPU i686 y en emuladores x86 de 32 bits (v86, en el navegador). 'make 32' la arma en build32/
# con el mismo codigo, sin tocar los .o de 64 bits.
ARCH    ?= 64
CC      := gcc
ifeq ($(ARCH),32)
CFLAGS  := -std=gnu17 -m32 -march=i686 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none \
           -mgeneral-regs-only -O2 -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
LDEMU   := elf_i386
ENTRYO  := entry32.o
LIBO    := lib.o lib32.o
else
CFLAGS  := -std=gnu17 -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none \
           -mgeneral-regs-only -mno-red-zone -mcmodel=small -O2 -Wall -Wextra
LDEMU   := elf_x86_64
ENTRYO  := entry.o
LIBO    := lib.o
endif
LDFLAGS := -m $(LDEMU) -z max-page-size=0x1000 -T linker.ld -nostdlib -z noexecstack --no-warn-rwx-segments

OBJS := $(ENTRYO) kernel.o vga.o kbd.o $(LIBO) ram.o task.o irq.o bmg.o sysinfo.o basic.o store.o pci.o mouse.o bios.o blk.o fs.o mmio.o usb.o \
        hda.o ac97.o audio.o atapi.o cd.o sys.o module.o fb.o

# todas las versiones; la de video solo si hay un .BVC (make VIDEO=mi.bvc)
VIDEO ?= VIDEO.BVC
ALLIMGS := pc-basic.img pc-basic32.img pc-basic-doom.img $(if $(wildcard $(VIDEO)),pc-basic-video.img)
all: $(ALLIMGS) tools/bas2bin/bas2bin
	@echo "Listo: $(ALLIMGS)"
	@$(if $(wildcard $(VIDEO)),,echo "(sin pc-basic-video.img: no existe $(VIDEO); usa make VIDEO=archivo.bvc)")
64: pc-basic.img
doom: pc-basic-doom.img
video: pc-basic-video.img

boot.bin: boot.asm
	nasm -f bin -DKERNEL_SECTORS=$(KSECT) $< -o $@

stage2.bin: stage2.asm
	nasm -f bin $< -o $@
	@[ $$(stat -c %s $@) -eq 2048 ] || { echo "stage2 debe medir 2048 bytes"; exit 1; }

thunk.bin: thunk.asm
	nasm -f bin $< -o $@

thunk32.bin: thunk32.asm
	nasm -f bin $< -o $@

entry32.o: entry32.asm thunk32.bin
	nasm -f elf32 $< -o $@

entry.o: entry.asm thunk.bin
	nasm -f elf64 $< -o $@

%.o: %.c kernel.h basic.h store.h fs.h kapi.h audio.h cd.h sys.h module.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) -o $@

kernel.bin: kernel.elf
	objcopy -O binary $< $@
	@size=$$(stat -c %s $@); max=$$(( $(KSECT) * 512 )); \
	 echo "kernel.bin: $$size bytes (maximo $$max)"; \
	 [ $$size -le $$max ] || { echo "El kernel no cabe: sube KSECT"; exit 1; }

# ---- modulos de /SYS: el escritorio y las apps NO van en el kernel, van como archivos dentro de la imagen ----
MODLD := ld -m $(LDEMU) -T mod.ld -nostdlib -z noexecstack --no-warn-rwx-segments
SYSDIR := sys
SYSFILES := GUI.SYS=$(SYSDIR)/GUI.SYS PONG.APP=$(SYSDIR)/PONG.APP CALC.APP=$(SYSDIR)/CALC.APP \
            FILES.APP=$(SYSDIR)/FILES.APP YAAI.APP=$(SYSDIR)/YAAI.APP BASHTERM.APP=$(SYSDIR)/BASHTERM.APP TINYCORE.APP=$(SYSDIR)/TINYCORE.APP NET.APP=$(SYSDIR)/NET.APP DOOM.APP=$(SYSDIR)/DOOM.APP PKG.APP=$(SYSDIR)/PKG.APP FORO.APP=$(SYSDIR)/FORO.APP INSTALL.APP=$(SYSDIR)/INSTALL.APP PLAY.APP=$(SYSDIR)/PLAY.APP SAY.APP=$(SYSDIR)/SAY.APP COSMOS.APP=$(SYSDIR)/COSMOS.APP PONG.BAS=$(SYSDIR)/pong.bas \
            $(foreach i,$(wildcard icons/*.icb),$(shell echo $(notdir $(i)) | tr a-z A-Z)=$(i))

# ARCH=32: estas apps dependen del modo largo (arrancan Linux de 64 bits, iPXE de 64 bits o el motor DOOM de 64 bits)
ifeq ($(ARCH),32)
NO32 := BASHTERM TINYCORE NET DOOM COSMOS
SYSFILES := $(filter-out $(addsuffix .APP=%,$(NO32)),$(SYSFILES))
endif
SYSPATHS := $(foreach f,$(SYSFILES),$(lastword $(subst =, ,$(f))))

# los modulos de /SYS se compilan con -Os: el area /SYS de la imagen es fija (276 sectores) y con -O2 los
# GCC nuevos (Arch) ya no cabian. La velocidad casi no cambia: el trabajo pesado lo hace el kernel.
$(SYSDIR)/%.o: $(SYSDIR)/%.c kapi.h mod_head.h $(SYSDIR)/ui.h $(SYSDIR)/wtk.h $(SYSDIR)/binwin.h $(SYSDIR)/fatw.h
	$(CC) $(filter-out -O2,$(CFLAGS)) -Os -I. -c $< -o $@

$(SYSDIR)/GUI.SYS: $(SYSDIR)/gui.o $(LIBO) mod.ld
	$(MODLD) --defsym=MODBASE=0x300000 $(SYSDIR)/gui.o $(LIBO) -o $(SYSDIR)/gui.elf
	objcopy -O binary $(SYSDIR)/gui.elf $@

# cada app de ventana se enlaza a su propia direccion para poder estar abierta a la vez
define APP
$(SYSDIR)/$(1).APP: $(SYSDIR)/$(2).o $(LIBO) mod.ld
	$(MODLD) --defsym=MODBASE=$(3) $(SYSDIR)/$(2).o $(LIBO) -o $(SYSDIR)/$(2).elf
	objcopy -O binary $(SYSDIR)/$(2).elf $$@
endef
$(eval $(call APP,PONG,pong,0x400000))
$(eval $(call APP,CALC,calc,0x480000))
$(eval $(call APP,FILES,files,0x500000))
$(eval $(call APP,YAAI,yaai,0x580000))
$(eval $(call APP,BASHTERM,bashterm,0x600000))
$(eval $(call APP,TINYCORE,tinycore,0x680000))
$(eval $(call APP,NET,net,0x6F0000))
$(eval $(call APP,DOOM,doomapp,0x6A0000))
$(eval $(call APP,PKG,pkg,0x6B0000))
$(eval $(call APP,FORO,foro,0x4C0000))
$(eval $(call APP,INSTALL,install,0x6C0000))
$(eval $(call APP,PLAY,play,0x6D0000))
$(eval $(call APP,SAY,say,0x6E0000))
$(eval $(call APP,COSMOS,cosmosapp,0x690000))

# SAY usa SSE2 (el modulo lo activa solo): sin -mgeneral-regs-only y sin libm
$(SYSDIR)/say.o: $(SYSDIR)/say.c kapi.h mod_head.h
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -mfpmath=sse -fno-math-errno -fno-strict-aliasing -mstackrealign -I. -c $< -o $@

pc-basic.img: boot.bin stage2.bin kernel.bin $(SYSPATHS) tools/mksys.py
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=boot.bin   of=$@ conv=notrunc status=none
	dd if=stage2.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=kernel.bin of=$@ bs=512 seek=5 conv=notrunc status=none
	python3 tools/mksys.py $@ $(SYSFILES)
	@echo "Imagen lista: $@"

# ---- DOOM: el motor (PureDOOM) no cabe en /SYS: DOOM.BIN va en la particion FAT con el WAD (ver tools/doom/README.txt) ----
DOOMDIR := tools/doom
$(SYSDIR)/doom.o: $(SYSDIR)/doom.c kapi.h mod_head.h $(DOOMDIR)/PureDOOM.h
	$(CC) $(CFLAGS) -w -I. -I$(DOOMDIR) -c $< -o $@

DOOM.BIN: $(SYSDIR)/doom.o $(LIBO) mod.ld
	$(MODLD) --defsym=MODBASE=0x4000000 $(SYSDIR)/doom.o $(LIBO) -o $(SYSDIR)/doom.elf
	objcopy -O binary $(SYSDIR)/doom.elf $@
	@ls -l $@ | awk '{print "DOOM.BIN: " $$5 " bytes"}'

# ---- COSMOS: navegador con el Minimal Motor (MM) de Cosmos Browser (tools/cosmos/). Como DOOM, no cabe en /SYS:
#      COSMOS.BIN va en la particion FAT junto a NET.BIN y lo carga COSMOS.APP. El motor usa float: SSE2 como SAY ----
CMDIR := tools/cosmos
CMSRC := cosmosmm html layout urls libc
CMOBJS := $(foreach f,$(CMSRC),$(CMDIR)/$(f).o)
CMFLAGS := $(filter-out -mgeneral-regs-only -Wall -Wextra,$(CFLAGS)) -msse2 -mfpmath=sse -fno-math-errno \
           -fno-strict-aliasing -mstackrealign -D__thread= -I$(CMDIR)/inc -I$(CMDIR) -I.
$(CMDIR)/cosmosmm.o: $(CMDIR)/cosmosmm.c $(CMDIR)/cosmos.h kapi.h mod_head.h
	$(CC) $(CMFLAGS) -Wall -c $< -o $@
$(CMDIR)/%.o: $(CMDIR)/%.c $(CMDIR)/cosmos.h $(CMDIR)/inc/cstub.h
	$(CC) $(CMFLAGS) -w -c $< -o $@
COSMOS.BIN: $(CMOBJS) mod.ld
	$(MODLD) --defsym=MODBASE=0x7000000 $(CMOBJS) -o $(CMDIR)/cosmos.elf
	objcopy -O binary $(CMDIR)/cosmos.elf $@
	@ls -l $@ | awk '{print "COSMOS.BIN: " $$5 " bytes"}'

# pc-basic-doom.img = PC BASIC + particion FAT16 con DOOM.BIN y el WAD (DOOM1.WAD shareware por defecto; si hay DOOM.WAD se usa)
WAD ?= $(DOOMDIR)/doom1.wad
HUB_URL ?=                      # make pc-basic-doom.img HUB_URL=https://mi-hub.onrender.com  -> HUB.CFG en la imagen
HUB_KEY ?=                      # clave para subir paquetes con PKG (2a linea de HUB.CFG; quien tenga la imagen puede subir)
pc-basic-doom.img: pc-basic.img DOOM.BIN COSMOS.BIN tools/mkbashimg $(WAD)
	tools/mkbashimg $@ pc-basic.img tools/linux/BZIMAGE tools/linux/INITRD.GZ \
	  --copy DOOM.BIN=DOOM.BIN --copy COSMOS.BIN=COSMOS.BIN --copy $(WAD)=$(shell echo $(notdir $(WAD)) | tr a-z A-Z) \
	  $$( [ -f tools/net/NET.BIN ] && echo --copy tools/net/NET.BIN=NET.BIN ) \
	  $$( [ -n "$(HUB_URL)" ] && { { echo "$(HUB_URL)"; [ -z "$(HUB_KEY)" ] || echo "$(HUB_KEY)"; } > HUB.CFG; echo --copy HUB.CFG=HUB.CFG; } ) --mib 64
	@echo "Imagen lista: $@  (QEMU: qemu-system-x86_64 -drive format=raw,file=$@,if=ide -m 128)"

tools/mkbashimg: tools/mkbashimg.c
	cc -O2 -o $@ $<

# ---- la tienda: programas para el hub (PKG.APP los baja por HTTPS). Un programa = apps/NOMBRE.c -> store/NOMBRE.BIN,
#      enlazado a 0x9000000. Sube store/ a tu repo de GitHub; hub/server.py lo sirve (ver hub/README.md) ----
STOREAPPS := hello demowin
define STOREAPP
store/$(shell echo $(1) | tr a-z A-Z).BIN: apps/$(1).c kapi.h mod_head.h sys/ui.h sys/wtk.h $(LIBO) mod.ld
	$(CC) $(CFLAGS) -w -I. -c apps/$(1).c -o apps/$(1).o
	$(MODLD) --defsym=MODBASE=0x9000000 apps/$(1).o $(LIBO) -o apps/$(1).elf
	objcopy -O binary apps/$(1).elf $$@
endef
$(foreach a,$(STOREAPPS),$(eval $(call STOREAPP,$(a))))
store: $(foreach a,$(STOREAPPS),store/$(shell echo $(a) | tr a-z A-Z).BIN) store/COMETAS.BIN

# BCC (BASIC-OS COMPILER C): compilador de C que escribe .BIN de la tienda. tools/bcc/bcc es la version de Linux
#   (bcc hola.c -o HOLA.BIN), tools/bcc/bccrun corre un .BIN en Linux para probar y store/BCC.BIN es el que va
#   dentro de BASIC-OS. make bcc-test compara la salida de BCC con la de gcc en tools/bcc/tests.
BCCDIR := tools/bcc
BCCSRC := $(BCCDIR)/bcc.c $(BCCDIR)/bcc_pp.c $(BCCDIR)/bcc_parse.c $(BCCDIR)/bcc_gen.c $(BCCDIR)/bcclib.h kapi.h
$(BCCDIR)/bcclib.h: $(BCCDIR)/lib/bcc.h $(BCCDIR)/lib/bcclib.c $(BCCDIR)/mklib.py
	python3 $(BCCDIR)/mklib.py $(BCCDIR)/lib/bcc.h $(BCCDIR)/lib/bcclib.c > $@
store/BCC.BIN: $(BCCDIR)/bccos.c $(BCCSRC) sys/fatw.h mod_head.h $(LIBO) mod.ld
	$(CC) $(CFLAGS) -w -I. -I$(BCCDIR) -c $(BCCDIR)/bccos.c -o $(BCCDIR)/bccos.o
	$(MODLD) --defsym=MODBASE=0x9000000 $(BCCDIR)/bccos.o $(LIBO) -o $(BCCDIR)/bccos.elf
	objcopy -O binary $(BCCDIR)/bccos.elf $@
$(BCCDIR)/bcc: $(BCCDIR)/bcc_host.c $(BCCSRC)
	cc -O2 -Wall -Wno-unused-function -I. -o $@ $<
$(BCCDIR)/bccrun: $(BCCDIR)/bccrun.c kapi.h
	cc -O2 -Wall -I. -o $@ $<
bcc: store/BCC.BIN $(BCCDIR)/bcc $(BCCDIR)/bccrun
bcc-test: $(BCCDIR)/bcc $(BCCDIR)/bccrun
	$(BCCDIR)/tests/run.sh
.PHONY: bcc bcc-test

# MPLAY.BIN: reproductor de video con FFmpeg (paquete opcional de la tienda). La primera vez baja y compila FFmpeg (~5 min).
# Necesita musl-gcc (Arch: pacman -S musl), nasm y git. Ver tools/mplay/.
mplay:
	tools/mplay/build.sh
# MPLAYAV1.BIN: plugin AV1 de MPLAY (dav1d). Necesita ademas meson y ninja.
mplayav1:
	tools/mplay/av1/build.sh
.PHONY: mplay mplayav1

# NET.BIN (iPXE con HTTPS): baja iPXE de GitHub y lo compila con los parches de tools/net/overlay (~2 min). Hace falta para PKG.
# pc-basic-video.img = PC BASIC + FAT16 con un video .BVC:  make pc-basic-video.img VIDEO=salida.bvc   (ver tools/mkbvc.c)
# cliente del foro para Linux (SDL2 + SDL2_ttf; la red la hace el programa curl): tools/forosdl [https://hub] [clave]
tools/forosdl: tools/forosdl.c
	cc -O2 -Wall -Wno-format-truncation -o $@ $< $$(pkg-config --cflags --libs sdl2 SDL2_ttf)

tools/mkbvc: tools/mkbvc.c
	cc -O2 -o $@ $<
tools/bvcplay: tools/bvcplay.c
	cc -O2 -o $@ $< $$(pkg-config --cflags --libs sdl2)
pc-basic-video.img: pc-basic.img COSMOS.BIN tools/mkbashimg $(VIDEO)
	tools/mkbashimg $@ pc-basic.img tools/linux/BZIMAGE tools/linux/INITRD.GZ --copy $(VIDEO)=$(shell echo $(notdir $(VIDEO)) | tr a-z A-Z) --copy COSMOS.BIN=COSMOS.BIN \
	  $$( [ -f tools/net/NET.BIN ] && echo --copy tools/net/NET.BIN=NET.BIN ) --mib $$(( ( $$(stat -c %s $(VIDEO)) / 1048576 ) + 24 ))
	@echo "Imagen lista: $@  (QEMU: qemu-system-x86_64 -drive format=raw,file=$@,if=ide -m 512 ...; en BASIC: PLAY \"$(shell echo $(notdir $(VIDEO)) | tr a-z A-Z)\")"

net:
	sh tools/net/build.sh

# ---- bas2bin: compilador de .BAS a .BIN (modulo de la tienda). Runtimes embebidos + herramienta de Linux.
#      Dentro del sistema esta en PKG (tecla C). En la version de 32 bits solo existe el modo empaquetador. ----
B2B := tools/bas2bin
B2BRT := $(B2B)/rt_pack.bin $(if $(filter 32,$(ARCH)),,$(B2B)/rt_native.bin)
$(B2B)/rt_%.o: $(B2B)/rt_%.c kapi.h mod_head.h $(B2B)/b2b.h
	$(CC) $(filter-out -O2,$(CFLAGS)) -Os -I. -c $< -o $@
$(B2B)/rt_%.bin: $(B2B)/rt_%.o $(LIBO) mod.ld
	$(MODLD) --defsym=MODBASE=0x9000000 $< $(LIBO) -o $(B2B)/rt_$*.elf
	objcopy -O binary $(B2B)/rt_$*.elf $@
$(B2B)/b2b_blobs.h: $(B2BRT) $(B2B)/bin2h.py
	python3 $(B2B)/bin2h.py $@ $(B2BRT)
$(B2B)/bas2bin: $(B2B)/main.c $(B2B)/b2b.c $(B2B)/b2b.h $(B2B)/b2b_blobs.h
	cc -O2 -Wall -I. -o $@ $(B2B)/main.c
$(SYSDIR)/pkg.o: $(B2B)/b2b.c $(B2B)/b2b.h $(B2B)/b2b_blobs.h
bas2bin: $(B2B)/bas2bin

# ---- BMG (BASIC-OS Motor Game): juegos de ejemplo en BASIC (tools/bmg/*.bas) -> store/ con icono de escritorio.
#      Van empaquetados (-p): todo el BASIC y corren tambien en 32 bits. ----
store/COMETAS.BIN: tools/bmg/cometas.bas $(B2B)/bas2bin
	$(B2B)/bas2bin -p $< $@
juegos: store/COMETAS.BIN

# ---- super-basicos.img: menu de arranque + PC BASIC 64 + PC BASIC 32 + una FAT16 compartida ----
SUPERDIR := tools/super
$(SUPERDIR)/hook.bin: $(SUPERDIR)/hook.asm
	nasm -f bin $< -o $@
$(SUPERDIR)/super.bin: $(SUPERDIR)/super.asm $(SUPERDIR)/hook.bin
	cd $(SUPERDIR) && nasm -f bin super.asm -o super.bin
# la particion lleva todo: DOOM + WAD, el video si existe, NET.BIN si existe y Linux
build-super/completo64.img: pc-basic.img DOOM.BIN COSMOS.BIN tools/mkbashimg $(WAD) $(wildcard $(VIDEO))
	mkdir -p build-super
	tools/mkbashimg $@ pc-basic.img tools/linux/BZIMAGE tools/linux/INITRD.GZ \
	  --copy DOOM.BIN=DOOM.BIN --copy COSMOS.BIN=COSMOS.BIN --copy $(WAD)=$(shell echo $(notdir $(WAD)) | tr a-z A-Z) \
	  $(if $(wildcard $(VIDEO)),--copy $(VIDEO)=$(shell echo $(notdir $(VIDEO)) | tr a-z A-Z)) \
	  $$( [ -f tools/net/NET.BIN ] && echo --copy tools/net/NET.BIN=NET.BIN ) \
	  $$( [ -n "$(HUB_URL)" ] && { { echo "$(HUB_URL)"; [ -z "$(HUB_KEY)" ] || echo "$(HUB_KEY)"; } > HUB.CFG; echo --copy HUB.CFG=HUB.CFG; } ) \
	  --mib $$(( 64 + $(if $(wildcard $(VIDEO)),$$(stat -c %s $(VIDEO)) / 1048576,0) ))
super-basicos.img: build-super/completo64.img pc-basic32.img $(SUPERDIR)/super.bin $(SUPERDIR)/mksuper.py
	python3 $(SUPERDIR)/mksuper.py $@ $(SUPERDIR)/super.bin build-super/completo64.img pc-basic32.img
	@$(if $(wildcard $(VIDEO)),,echo "(sin video: no existe $(VIDEO); usa make super VIDEO=archivo.bvc)")
	@echo "Imagen lista: $@  (cosvm: ./cosvm super-basicos.img   QEMU: qemu-system-x86_64 -drive format=raw,file=$@ -m 256)"
super: super-basicos.img

# variante de 32 bits: mismo codigo, compilado con -m32 en build32/ (no pisa los .o de 64 bits) -> pc-basic32.img
# solo se rehace si cambia algun archivo fuente
SRC32 := $(wildcard *.c *.h *.asm *.ld *.py sys/*.c sys/*.h sys/*.bas tools/*.py icons/*) Makefile
32: pc-basic32.img
pc-basic32.img: $(SRC32)
	rm -rf build32 && mkdir build32
	tar cf - --exclude='*.o' --exclude='*.elf' --exclude='*.bin' --exclude='*.APP' --exclude='*.SYS' --exclude='*.img' \
	    --exclude='./build32' --exclude='./tools/linux' --exclude='./tools/net' --exclude='./tools/doom' --exclude='./DOOM.BIN' . | tar xf - -C build32
	$(MAKE) -C build32 ARCH=32 pc-basic.img
	cp build32/pc-basic.img pc-basic32.img
	@echo "Imagen lista: pc-basic32.img  (QEMU: qemu-system-i386 -drive format=raw,file=pc-basic32.img,if=floppy; web: v86)"

run32: 32
	qemu-system-i386 -drive format=raw,file=pc-basic32.img,if=floppy

run: pc-basic.img
	qemu-system-x86_64 -drive format=raw,file=pc-basic.img,if=floppy

test:
	$(MAKE) -C test test

clean:
	rm -rf build32 pc-basic32.img build-super super-basicos.img $(SUPERDIR)/*.bin
	rm -f tools/mkbashimg tools/mkbvc tools/bvcplay
	rm -f $(CMDIR)/*.o $(CMDIR)/cosmos.elf COSMOS.BIN
	rm -f $(B2B)/*.o $(B2B)/*.elf $(B2B)/*.bin $(B2B)/b2b_blobs.h $(B2B)/bas2bin
	rm -f *.o *.elf *.bin pc-basic.img pc-basic-doom.img $(SYSDIR)/*.o $(SYSDIR)/*.elf $(SYSDIR)/*.SYS $(SYSDIR)/*.APP apps/*.o apps/*.elf
	$(MAKE) -C test clean

.PHONY: all 64 32 doom video super bas2bin juegos run run32 test clean store net
