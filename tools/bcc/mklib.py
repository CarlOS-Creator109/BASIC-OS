#!/usr/bin/env python3
"""mklib.py - mete lib/bcc.h y lib/bcclib.c dentro de bcclib.h (cadenas de C) para que BCC no necesite archivos aparte.
uso: mklib.py lib/bcc.h lib/bcclib.c > bcclib.h"""
import sys

HEADERS = ["bcc.h", "bos.h", "stdio.h", "stdlib.h", "string.h", "strings.h", "ctype.h", "stdarg.h", "stddef.h",
           "stdint.h", "stdbool.h", "limits.h", "assert.h", "time.h", "errno.h", "inttypes.h", "math.h", "unistd.h",
           "sys/types.h", "stdnoreturn.h", "iso646.h"]


def cstr(name, text):
    out = ["static const char %s[] =" % name]
    for line in text.split("\n"):
        s = line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
        out.append('    "%s\\n"' % s)
    out[-1] += ";"
    return "\n".join(out)


def main():
    h = open(sys.argv[1], encoding="utf-8").read()
    lib = open(sys.argv[2], encoding="utf-8").read()
    print("/* bcclib.h - generado por mklib.py desde lib/bcc.h y lib/bcclib.c: no lo edites a mano */")
    print(cstr("bcc_h_src", h))
    print(cstr("bcc_lib_src", lib))
    print("static const char *const bcc_hnames[] = {%s, 0};" % ", ".join('"%s"' % n for n in HEADERS))
    print("""static const char *bcc_header(const char *name)
{
    int i, k;
    for (i = 0; bcc_hnames[i]; i++) {
        for (k = 0; bcc_hnames[i][k] && bcc_hnames[i][k] == name[k]; k++) ;
        if (!bcc_hnames[i][k] && !name[k])
            return bcc_h_src;
    }
    return 0;
}""")


main()
