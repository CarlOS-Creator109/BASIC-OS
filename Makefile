# Makefile - PC BASIC
#
#   make          construye pc-basic.img (imagen de disquete de 1.44 MB)
#   make run      la arranca en QEMU
#   make test     prueba el interprete en el host (sin arrancar el SO)
#   make clean

KSECT   := 160                       # sectores reservados para el kernel (80 KB); max 162 (/SYS empieza en el LBA 168)
CC      := gcc
CFLAGS  := -std=gnu17 -m64 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
           -fno-builtin -fno-asynchronous-unwind-tables -fcf-protection=none \
           -mgeneral-regs-only -mno-red-zone -mcmodel=small -O2 -Wall -Wextra
LDFLAGS := -m elf_x86_64 -z max-page-size=0x1000 -T linker.ld -nostdlib -z noexecstack --no-warn-rwx-segments

OBJS := entry.o kernel.o vga.o kbd.o lib.o sysinfo.o basic.o store.o pci.o mouse.o bios.o blk.o fs.o mmio.o usb.o \
        hda.o ac97.o audio.o atapi.o cd.o sys.o module.o fb.o

all: pc-basic.img

boot.bin: boot.asm
	nasm -f bin -DKERNEL_SECTORS=$(KSECT) $< -o $@

stage2.bin: stage2.asm
	nasm -f bin $< -o $@
	@[ $$(stat -c %s $@) -eq 2048 ] || { echo "stage2 debe medir 2048 bytes"; exit 1; }

thunk.bin: thunk.asm
	nasm -f bin $< -o $@

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
MODLD := ld -m elf_x86_64 -T mod.ld -nostdlib -z noexecstack --no-warn-rwx-segments
SYSDIR := sys
SYSFILES := GUI.SYS=$(SYSDIR)/GUI.SYS PONG.APP=$(SYSDIR)/PONG.APP CALC.APP=$(SYSDIR)/CALC.APP \
            FILES.APP=$(SYSDIR)/FILES.APP YAAI.APP=$(SYSDIR)/YAAI.APP BASHTERM.APP=$(SYSDIR)/BASHTERM.APP TINYCORE.APP=$(SYSDIR)/TINYCORE.APP NET.APP=$(SYSDIR)/NET.APP DOOM.APP=$(SYSDIR)/DOOM.APP PKG.APP=$(SYSDIR)/PKG.APP INSTALL.APP=$(SYSDIR)/INSTALL.APP PONG.BAS=$(SYSDIR)/pong.bas \
            $(foreach i,$(wildcard icons/*.icb),$(shell echo $(notdir $(i)) | tr a-z A-Z)=$(i))

$(SYSDIR)/%.o: $(SYSDIR)/%.c kapi.h mod_head.h $(SYSDIR)/ui.h
	$(CC) $(CFLAGS) -I. -c $< -o $@

$(SYSDIR)/GUI.SYS: $(SYSDIR)/gui.o lib.o mod.ld
	$(MODLD) --defsym=MODBASE=0x300000 $(SYSDIR)/gui.o lib.o -o $(SYSDIR)/gui.elf
	objcopy -O binary $(SYSDIR)/gui.elf $@

# cada app de ventana se enlaza a su propia direccion para poder estar abierta a la vez
define APP
$(SYSDIR)/$(1).APP: $(SYSDIR)/$(2).o lib.o mod.ld
	$(MODLD) --defsym=MODBASE=$(3) $(SYSDIR)/$(2).o lib.o -o $(SYSDIR)/$(2).elf
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
$(eval $(call APP,INSTALL,install,0x6C0000))

pc-basic.img: boot.bin stage2.bin kernel.bin $(SYSDIR)/GUI.SYS $(SYSDIR)/PONG.APP $(SYSDIR)/CALC.APP \
              $(SYSDIR)/FILES.APP $(SYSDIR)/YAAI.APP $(SYSDIR)/BASHTERM.APP $(SYSDIR)/TINYCORE.APP $(SYSDIR)/NET.APP $(SYSDIR)/DOOM.APP $(SYSDIR)/PKG.APP $(SYSDIR)/INSTALL.APP $(SYSDIR)/pong.bas $(wildcard icons/*.icb) tools/mksys.py
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

DOOM.BIN: $(SYSDIR)/doom.o lib.o mod.ld
	$(MODLD) --defsym=MODBASE=0x4000000 $(SYSDIR)/doom.o lib.o -o $(SYSDIR)/doom.elf
	objcopy -O binary $(SYSDIR)/doom.elf $@
	@ls -l $@ | awk '{print "DOOM.BIN: " $$5 " bytes"}'

# pc-basic-doom.img = PC BASIC + particion FAT16 con DOOM.BIN y el WAD (DOOM1.WAD shareware por defecto; si hay DOOM.WAD se usa)
WAD ?= $(DOOMDIR)/doom1.wad
HUB_URL ?=                      # make pc-basic-doom.img HUB_URL=https://mi-hub.onrender.com  -> HUB.CFG en la imagen
pc-basic-doom.img: pc-basic.img DOOM.BIN tools/mkbashimg $(WAD)
	tools/mkbashimg $@ pc-basic.img tools/linux/BZIMAGE tools/linux/INITRD.GZ \
	  --copy DOOM.BIN=DOOM.BIN --copy $(WAD)=$(shell echo $(notdir $(WAD)) | tr a-z A-Z) \
	  $$( [ -f tools/net/NET.BIN ] && echo --copy tools/net/NET.BIN=NET.BIN ) \
	  $$( [ -n "$(HUB_URL)" ] && { echo "$(HUB_URL)" > HUB.CFG; echo --copy HUB.CFG=HUB.CFG; } ) --mib 64
	@echo "Imagen lista: $@  (QEMU: qemu-system-x86_64 -drive format=raw,file=$@,if=ide -m 128)"

tools/mkbashimg: tools/mkbashimg.c
	cc -O2 -o $@ $<

# ---- la tienda: programas para el hub (PKG.APP los baja por HTTPS). Un programa = apps/NOMBRE.c -> store/NOMBRE.BIN,
#      enlazado a 0x9000000. Sube store/ a tu repo de GitHub; hub/server.py lo sirve (ver hub/README.md) ----
STOREAPPS := hello
define STOREAPP
store/$(shell echo $(1) | tr a-z A-Z).BIN: apps/$(1).c kapi.h mod_head.h lib.o mod.ld
	$(CC) $(CFLAGS) -w -I. -c apps/$(1).c -o apps/$(1).o
	$(MODLD) --defsym=MODBASE=0x9000000 apps/$(1).o lib.o -o apps/$(1).elf
	objcopy -O binary apps/$(1).elf $$@
endef
$(foreach a,$(STOREAPPS),$(eval $(call STOREAPP,$(a))))
store: $(foreach a,$(STOREAPPS),store/$(shell echo $(a) | tr a-z A-Z).BIN)

# NET.BIN (iPXE con HTTPS): baja iPXE de GitHub y lo compila con los parches de tools/net/overlay (~2 min). Hace falta para PKG.
net:
	sh tools/net/build.sh

run: pc-basic.img
	qemu-system-x86_64 -drive format=raw,file=pc-basic.img,if=floppy

test:
	$(MAKE) -C test test

clean:
	rm -f *.o *.elf *.bin pc-basic.img pc-basic-doom.img $(SYSDIR)/*.o $(SYSDIR)/*.elf $(SYSDIR)/*.SYS $(SYSDIR)/*.APP apps/*.o apps/*.elf
	$(MAKE) -C test clean

.PHONY: all run test clean store net
