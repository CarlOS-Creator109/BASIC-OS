#!/usr/bin/env python3
"""videos.py - prepara una carpeta de videos para la imagen (make super VIDEOS=carpeta)

uso: videos.py CARPETA SALIDA [opciones de mkbvc...]

Por cada video de CARPETA (.mp4 .mkv .webm .avi .mov):
  - SALIDA/NOMBRE.MP4 (o la extension que tenga): enlace al original, para abrirlo con MPLAY
  - SALIDA/NOMBRE.BVC: el mismo video convertido con tools/mkbvc para PLAY (solo se reconvierte si el original cambio)
NOMBRE es un nombre corto de FAT (8 letras, A-Z y 0-9) sacado del titulo: "Zemlyane - Grass near home..." -> ZEMLYANE.
Escribe SALIDA/copy.args (los --copy para mkbashimg), SALIDA/mib (megas que ocupan) y SALIDA/VIDEOS.TXT
(que nombre corto es cada titulo; tambien va a la imagen).
"""
import os, re, subprocess, sys

EXTS = (".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v")


def short_name(title, used):
    base = re.sub(r"[^A-Za-z0-9]", "", os.path.splitext(title)[0]).upper()
    base = base[:8] or "VIDEO"
    name, n = base, 1
    while name in used:
        n += 1
        suf = str(n)
        name = base[: 8 - len(suf)] + suf
    used.add(name)
    return name


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out, opts = sys.argv[1], sys.argv[2], sys.argv[3:]
    here = os.path.dirname(os.path.abspath(__file__))
    mkbvc = os.path.join(here, "mkbvc")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                       # enlaces viejos (los .BVC se quedan: sirven de cache)
        p = os.path.join(out, f)
        if os.path.islink(p):
            os.unlink(p)
    vids = sorted(f for f in os.listdir(src) if f.lower().endswith(EXTS)) if os.path.isdir(src) else []
    used, args, lines, total = set(), [], [], 0
    for title in vids:
        path = os.path.abspath(os.path.join(src, title))
        name = short_name(title, used)
        ext = os.path.splitext(title)[1].upper()[:4]
        link = os.path.join(out, name + ext)
        os.symlink(path, link)
        bvc = os.path.join(out, name + ".BVC")
        if not os.path.exists(bvc) or os.path.getmtime(bvc) < os.path.getmtime(path):
            print("videos: %s -> %s.BVC ..." % (title, name), flush=True)
            r = subprocess.run([mkbvc, path, bvc + ".tmp"] + opts)
            if r.returncode != 0:
                print("videos: no pude convertir %s (falta ffmpeg?)" % title, file=sys.stderr)
                if os.path.exists(bvc + ".tmp"):
                    os.unlink(bvc + ".tmp")
            else:
                os.replace(bvc + ".tmp", bvc)
        args.append("--copy %s=%s%s" % (link, name, ext))
        total += os.path.getsize(path)
        line = "%-12s %s" % (name + ext, title)
        if os.path.exists(bvc):
            args.append("--copy %s=%s.BVC" % (bvc, name))
            total += os.path.getsize(bvc)
            line += "   (PLAY \"%s.BVC\")" % name
        lines.append(line)
    with open(os.path.join(out, "VIDEOS.TXT"), "wb") as f:      # la consola solo pinta ASCII
        f.write("".join(l + "\n" for l in lines).encode("ascii", "replace"))
    if lines:
        args.append("--copy %s=VIDEOS.TXT" % os.path.join(out, "VIDEOS.TXT"))
    with open(os.path.join(out, "copy.args"), "w") as f:
        f.write(" ".join(args) + "\n")
    with open(os.path.join(out, "mib"), "w") as f:
        f.write("%d\n" % (total // 1048576 + 1))
    for l in lines:
        print("videos: " + l)


main()
