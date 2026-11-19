#!/bin/sh
# build.sh - compila el plugin MPLAYAV1.BIN (dav1d + musl). Uso: tools/mplay/av1/build.sh   (desde pc-basic/)
# Necesita: musl-gcc, nasm, git, meson y ninja (Arch: pacman -S musl nasm meson ninja). Sale store/MPLAYAV1.BIN.
set -e
cd "$(dirname "$0")"
TOP=$(cd ../../.. && pwd)
D=$PWD/dav1d
OUT=$PWD/dav1dout
command -v musl-gcc >/dev/null || { echo "falta musl-gcc (Arch: sudo pacman -S musl)"; exit 1; }
if [ ! -f "$OUT/lib/libdav1d.a" ] && [ ! -f "$OUT/lib64/libdav1d.a" ]; then
    [ -d "$D" ] || git clone --depth 1 -b 1.5.1 https://github.com/videolan/dav1d.git "$D"
    cat > cross.txt <<CROSS
[binaries]
c = 'musl-gcc'
ar = 'ar'
strip = 'strip'
[built-in options]
c_args = ['-fno-pic', '-fno-pie', '-fno-stack-protector', '-fcf-protection=none', '-fno-asynchronous-unwind-tables']
c_link_args = ['-static', '-no-pie']
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS
    (cd "$D" && meson setup build --cross-file ../cross.txt --default-library=static --buildtype=release \
        -Denable_tools=false -Denable_tests=false -Denable_examples=false -Db_staticpic=false -Dbitdepths=8,16 \
        --prefix="$OUT" --libdir=lib && ninja -C build install)
fi
MUSL=$(dirname "$(musl-gcc -v 2>&1 | grep -o '[^ "]*musl-gcc.specs' | head -1)")
[ -f "$MUSL/libc.a" ] || MUSL=/usr/lib/musl/lib
[ -f "$MUSL/libc.a" ] || MUSL=/usr/lib/x86_64-linux-musl
LIBGCC=$(gcc -print-libgcc-file-name)
musl-gcc -O2 -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables -I"$OUT/include" -I"$TOP" -c av1.c -o av1.o
ld -m elf_x86_64 -T ../mplay.ld --defsym=MODBASE=0xAA00000 -nostdlib -z noexecstack --no-warn-rwx-segments -o av1.elf \
   av1.o "$OUT/lib/libdav1d.a" "$MUSL/libc.a" "$LIBGCC"
if [ -n "$(readelf -S av1.elf | awk '/\.tdata|\.tbss/ {print $6}' | grep -v '^0*$')" ]; then
    echo "AVISO: el plugin usa variables por hilo (TLS)"; exit 1
fi
objcopy -O binary av1.elf "$TOP/store/MPLAYAV1.BIN"
END=$(nm av1.elf | awk '/ __mod_end$/ {print $1}')
echo "store/MPLAYAV1.BIN: $(stat -c %s "$TOP/store/MPLAYAV1.BIN") bytes, termina en 0x$END (limite 0xb000000)"
[ $((0x$END)) -le $((0xB000000)) ] || { echo "ERROR: el plugin no cabe"; exit 1; }
