#!/bin/sh
# Compila iPXE para BASIC-OS y deja NET.BIN (se carga en 0x700000 con NET.APP desde la particion FAT).
# uso: tools/net/build.sh [directorio-de-trabajo]       (necesita: git, make, gcc, binutils, perl)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
W=${1:-$HERE/work}
REV=$(cat "$HERE/IPXE_REV")
mkdir -p "$W"
[ -d "$W/ipxe/.git" ] || git clone https://github.com/ipxe/ipxe.git "$W/ipxe"
git -C "$W/ipxe" checkout -q "$REV"
cp -r "$HERE/overlay/." "$W/ipxe/src/"
S="$W/ipxe/src"
# la API de memoria de usuario: enganchar la variante basicos
grep -q basicos_umalloc "$S/include/ipxe/umalloc.h" || \
  sed -i 's|#include <ipxe/uheap.h>|#include <ipxe/uheap.h>\n#include <ipxe/basicos/basicos_umalloc.h>|' "$S/include/ipxe/umalloc.h"
cp "$HERE/local/"*.h "$S/config/local/" 2>/dev/null || true
# HTTPS (Render, GitHub...): iPXE solo confia en los certificados raiz que se le den al compilar.
#   por defecto: el paquete de CAs de Mozilla del sistema (/etc/ssl/certs/ca-certificates.crt), cada raiz separada
#   TRUST=ruta.crt   usa otro paquete PEM (p.ej. una CA de pruebas);   TRUST=none  deja los valores por defecto de iPXE
TRUST=${TRUST:-/etc/ssl/certs/ca-certificates.crt}
TRUSTARG=
if [ "$TRUST" != none ] && [ -f "$TRUST" ]; then
  R="$W/roots"; rm -rf "$R"; mkdir -p "$R"
  awk -v d="$R" 'BEGIN{n=0} /BEGIN CERTIFICATE/{n++; f=sprintf("%s/ca%03d.crt",d,n)} f{print > f} /END CERTIFICATE/{close(f); f=""}' "$TRUST"
  LIST=$(ls "$R"/ca*.crt | tr '\n' ',' | sed 's/,$//')
  echo "CAs raiz embebidas: $(ls "$R"/ca*.crt | wc -l)"
  TRUSTARG="CERT=$LIST TRUST=$LIST"
fi
make -C "$S" -j4 bin-x86_64-basicos/ipxe.bos BOS_ROOT="$ROOT" NO_WERROR=1 $TRUSTARG 2>&1 | tail -60
cp "$S/bin-x86_64-basicos/ipxe.bos" "$HERE/NET.BIN"
ls -l "$HERE/NET.BIN"
