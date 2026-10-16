#!/bin/sh
# Receta para regenerar BZIMAGE e INITRD.GZ de BashTerminal (Ubuntu/Debian con gcc, flex, bison, bc, libelf-dev, cpio, kbd, xkb-data)
# 1) kernel:   linux-6.12.50 -> make ARCH=x86_64 tinyconfig; scripts/kconfig/merge_config.sh -m .config kernel.frag;
#              make ARCH=x86_64 olddefconfig; make ARCH=x86_64 -j bzImage     (RELOCATABLE=y es obligatorio)
# 2) bash:     bash-5.2.37  CFLAGS="-Os -std=gnu17" ./configure --enable-static-link --without-bash-malloc --disable-nls --with-curses
#              make TERMCAP_LIB="-ltinfo"
# 3) busybox:  defconfig + CONFIG_STATIC=y, sin CONFIG_TC
# 4) teclados: ckbcomp -compact latam | (solo lineas keymaps/keycode, U+ > 0xFF -> VoidSymbol) | loadkeys -b  -> la.bmap (igual us.bmap)
# 5) initrd:   rootfs con /init (este directorio), /bin/{bash,busybox,kmap}, /etc/{la,us}.bmap, /usr/share/terminfo/l/linux;
#              gen_init_cpio (de usr/ del kernel) con nodos /dev/console (c 5 1), /dev/null (c 1 3), /dev/tty0 (c 4 0); gzip -9
# 6) imagen:   tools/mkbashimg pc-basic-linux.img pc-basic.img BZIMAGE INITRD.GZ --mib 32
echo "ver los comentarios de este archivo"
