#!/bin/sh
# Arma pc-basic-tinycore.img: PC BASIC + BashTerminal + Tiny Core CorePlus (x86, 32 bits) en la misma particion FAT.
# uso: [EDITION=core|tinycore|coreplus] tools/linux/mktinycore.sh [salida.img] [MiB]     (necesita: aria2c, bsdtar, cc)
set -e
# EDITION=core|tinycore|coreplus (por defecto coreplus). Cada edicion = su propia ISO y su propia imagen.
EDITION=${EDITION:-coreplus}
case "$EDITION" in
  core)     NAME=Core;     DEF=64  ;;
  tinycore) NAME=TinyCore; DEF=128 ;;
  coreplus) NAME=CorePlus; DEF=384 ;;
  *) echo "EDITION debe ser core, tinycore o coreplus"; exit 1 ;;
esac
OUT=${1:-pc-basic-$EDITION.img}
MIB=${2:-$DEF}
ISO=${ISO:-$NAME-current.iso}
URL=http://www.tinycorelinux.net/17.x/x86/release/$NAME-current.iso
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
[ -f "$ISO" ] && [ ! -f "$ISO.aria2" ] || { echo "bajando $URL ..."; command -v aria2c >/dev/null || { echo "falta aria2c (pacman -S aria2 / apt install aria2)"; exit 1; }; aria2c -x16 -s16 -c -o "$ISO" "$URL"; }
echo "verificando md5..."; EXP=$( (curl -fsL "$URL.md5.txt" || wget -qO- "$URL.md5.txt") 2>/dev/null | cut -d' ' -f1)
if [ -n "$EXP" ]; then GOT=$(md5sum "$ISO" | cut -d' ' -f1); [ "$EXP" = "$GOT" ] || { echo "md5 distinto: la ISO esta truncada. Corre otra vez: aria2c -x16 -s16 -c -o $ISO $URL"; exit 1; }; fi
T=$(mktemp -d); trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
[ "$ROOT/tools/mkbashimg" -nt "$ROOT/tools/mkbashimg.c" ] || cc -O2 -o "$ROOT/tools/mkbashimg" "$ROOT/tools/mkbashimg.c" || exit 1
echo "extrayendo la ISO..."; bsdtar --no-same-permissions -xf "$ISO" -C "$T" || { echo "la ISO esta incompleta o corrupta: borra $ISO y vuelve a correr (aria2c -c reanuda)"; exit 1; }
"$ROOT/tools/mkbashimg" "$OUT" "$ROOT/pc-basic.img" "$HERE/BZIMAGE" "$HERE/INITRD.GZ" \
  --copy "$T/boot/vmlinuz=BOOT/VMLINUZ" --copy "$T/boot/core.gz=BOOT/CORE.GZ" $( [ -d "$T/cde" ] && echo --copy "$T/cde=CDE" ) $( [ -f "$ROOT/tools/net/NET.BIN" ] && echo --copy "$ROOT/tools/net/NET.BIN=NET.BIN" ) --mib "$MIB"
echo "listo: $OUT  (arranca con QEMU: -drive format=raw,file=$OUT,if=ide -m 512; escribe TINYCORE o BASHTERM)"
