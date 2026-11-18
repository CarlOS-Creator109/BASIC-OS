#!/usr/bin/env python3
"""bin2h.py - mete los runtimes de bas2bin (rt_pack.bin, rt_native.bin) como arreglos de C en b2b_blobs.h.
uso: bin2h.py salida.h rt_pack.bin [rt_native.bin]   (sin rt_native.bin: PC BASIC de 32 bits, solo empaquetador)"""
import sys
out = sys.argv[1]
names = [("b2b_rt_pack", sys.argv[2])] + ([("b2b_rt_native", sys.argv[3])] if len(sys.argv) > 3 else [])
with open(out, "w") as f:
    f.write("/* generado por tools/bas2bin/bin2h.py: no editar */\n#ifndef B2B_BLOBS_H\n#define B2B_BLOBS_H\n")
    if len(names) == 1:
        f.write("#define B2B_NO_NATIVE 1\n")
    for name, path in names:
        data = open(path, "rb").read()
        f.write("static const unsigned char %s[%d] = {\n" % (name, len(data)))
        for i in range(0, len(data), 20):
            f.write("    " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
        f.write("};\nstatic const unsigned int %s_len = %d;\n" % (name, len(data)))
    f.write("#endif\n")
