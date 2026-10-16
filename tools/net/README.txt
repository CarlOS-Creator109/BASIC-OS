NET: red para BASIC-OS (iPXE portado, GPL-2.0-or-later, compatible con GPLv3)
  tools/net/build.sh     compila iPXE (necesita git, gcc, make, binutils, perl) -> tools/net/NET.BIN
  NET.BIN va en la particion FAT (raiz de HD0.1:). En BASIC:  NET   o   NET "dhcp; imgfetch http://..."
  Probado en QEMU: e1000, e1000e, rtl8139, pcnet, virtio-net (DHCP, ping, HTTP 8 MB).
  Sin probar en hardware real. Teclado: sin Shift = minusculas dentro de NET.
