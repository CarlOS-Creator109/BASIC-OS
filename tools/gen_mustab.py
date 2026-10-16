#!/usr/bin/env python3
"""Genera sys/mustab.h: tablas del sintetizador de musica entero (puerto de chip.c de DoomProject).
El kernel no tiene SSE ni libm, asi que seno, hexagonal, frecuencias y decaimientos se precalculan aqui."""
import math
SR = 22050
out = ["/* mustab.h - GENERADO por tools/gen_mustab.py (no editar a mano). SR = %d Hz */" % SR,
       "#define MUS_SR %d" % SR]
inc = [round(440.0 * 2 ** ((n - 69) / 12.0) / SR * 2 ** 32) for n in range(140)]
out.append("static const unsigned mus_inc[140] = {" + ",".join(str(min(i, 2**32 - 1)) for i in inc) + "};")
out.append("static const short mus_sin[512] = {" + ",".join(str(round(32767 * math.sin(2 * math.pi * i / 512))) for i in range(512)) + "};")
def hexw(th):
    seg = math.pi / 3
    rel = math.fmod(th, seg)
    half = seg / 2
    return math.cos(half) / math.cos(rel - half) * math.cos(th)
out.append("static const short mus_hex[256] = {" + ",".join(str(round(32767 * hexw(2 * math.pi * i / 256))) for i in range(256)) + "};")
for ms in (4, 8, 20, 30, 45, 60, 70, 90, 100, 120, 150, 180, 300, 400, 450, 500, 520, 600, 700, 1200):
    k = math.exp(-1.0 / (ms / 1000.0 * SR))
    out.append("#define KMS_%d %du" % (ms, min(round(k * 2 ** 32), 2**32 - 1)))
def res(f, bw):
    rr = math.exp(-math.pi * bw / SR)
    th = 2 * math.pi * f / SR
    b = 2 * rr * math.cos(th)
    c = -rr * rr
    a = 1 - b - c
    return [round(x * 2 ** 20) for x in (a, b, c)]
def arr(name, lst):
    out.append("static const int %s[][3] = {" % name + ",".join("{%d,%d,%d}" % tuple(r) for r in lst) + "};")
arr("mus_res_clap", [res(1200, 800)])
arr("mus_res_crash", [res(300 * r, 400) for r in (1, 1.44, 1.86, 2.29, 2.66, 3.32)])
arr("mus_res_ride", [res(800, 40)] + [res(400 * r, 300) for r in (1.4, 2.6, 3.3, 4.8)])
open("sys/mustab.h", "w").write("\n".join(out) + "\n")
