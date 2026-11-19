#!/bin/sh
# build.sh - compila MPLAY.BIN (FFmpeg + musl + capa Linux de BASIC-OS). Uso: tools/mplay/build.sh   (desde pc-basic/)
# Necesita: musl-gcc (Arch: pacman -S musl; Debian/Ubuntu: apt install musl-tools), nasm, git, make.
# La primera vez baja FFmpeg 7.1 a tools/mplay/ffmpeg y lo compila (unos minutos). Sale store/MPLAY.BIN.
set -e
cd "$(dirname "$0")"
TOP=$(cd ../.. && pwd)
FF=$PWD/ffmpeg
OUT=$PWD/ffout
command -v musl-gcc >/dev/null || { echo "falta musl-gcc (Arch: sudo pacman -S musl)"; exit 1; }
if [ ! -f "$OUT/lib/libavcodec.a" ]; then
    [ -d "$FF" ] || git clone --depth 1 -b n7.1 https://github.com/FFmpeg/FFmpeg.git "$FF"
    cd "$FF"
    ./configure --cc=musl-gcc --prefix="$OUT" --enable-static --disable-shared --disable-pic \
      --extra-cflags="-fno-pic -fno-pie -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables" \
      --extra-ldflags="-static -no-pie" \
      --disable-autodetect --disable-programs --disable-doc --disable-network --disable-pthreads --disable-iconv --disable-debug \
      --disable-avdevice --disable-avfilter --disable-postproc --disable-hwaccels --disable-everything \
      --enable-swscale --enable-swresample \
      --enable-decoder=h264,hevc,mpeg4,mpeg1video,mpeg2video,msmpeg4v3,vp8,vp9,theora,mjpeg,aac,mp3,mp2,vorbis,opus,flac,ac3,pcm_s16le,pcm_u8,pcm_f32le \
      --enable-demuxer=mov,matroska,avi,ogg,mp3,wav,flac,mpegts,mpegps,flv,aac,ac3,h264,hevc,m4v \
      --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,aac,mpegaudio,vorbis,opus,flac,ac3,mjpeg
    make -j"$(nproc)"
    make install
    cd ..
fi
MUSL=$(dirname "$(musl-gcc -dumpspecs 2>/dev/null | true; musl-gcc -v 2>&1 | grep -o '[^ "]*musl-gcc.specs' | head -1)")
[ -f "$MUSL/libc.a" ] || MUSL=/usr/lib/musl/lib                     # Arch
[ -f "$MUSL/libc.a" ] || MUSL=/usr/lib/x86_64-linux-musl            # Debian/Ubuntu
[ -f "$MUSL/libc.a" ] || { echo "no encuentro la libc.a de musl"; exit 1; }
LIBGCC=$(gcc -print-libgcc-file-name)
CF="-O2 -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables -I$OUT/include -I$TOP"
musl-gcc $CF -c mplay.c -o mplay.o
musl-gcc $CF -mgeneral-regs-only -c linux.c -o linux.o
musl-gcc -c syscall.S -o syscall.o
ld -m elf_x86_64 -T mplay.ld --defsym=MODBASE=0x9000000 -nostdlib -z noexecstack --no-warn-rwx-segments -o mplay.elf \
   mplay.o linux.o syscall.o -L"$OUT/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil "$MUSL/libc.a" "$LIBGCC"
if readelf -S mplay.elf | grep -q '\.tdata\|\.tbss'; then
    if [ "$(readelf -S mplay.elf | awk '/\.tdata|\.tbss/ {print $6}' | grep -v '^0*$' | head -1)" != "" ]; then
        echo "AVISO: el programa usa variables por hilo (TLS); la capa Linux no las prepara"; exit 1
    fi
fi
objcopy -O binary mplay.elf "$TOP/store/MPLAY.BIN"
END=$(nm mplay.elf | awk '/ __mod_end$/ {print $1}')
echo "store/MPLAY.BIN: $(stat -c %s "$TOP/store/MPLAY.BIN") bytes, termina en 0x$END (limite 0xb000000)"
