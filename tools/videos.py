#!/usr/bin/env python3
"""videos.py - prepara una carpeta de videos para la pagina de videos del hub (make videos VIDEOS=carpeta)

uso: videos.py CARPETA SALIDA [opciones de mkbvc...]

Por cada video de CARPETA (.mp4 .mkv .webm .avi .mov .m4v) deja en SALIDA:
  NOMBRE.BVC  convertido con tools/mkbvc, para PLAY (solo se reconvierte si el original cambio)
  NOMBRE.MP4  el video tal cual, para MPLAY (si no es .mp4 o pasa de 32 MB, ffmpeg lo pasa a MP4 de 480p)
  NOMBRE.TXT  el titulo (la descripcion que muestra el hub), sin el [id] de YouTube y con el cirilico en letras latinas
NOMBRE es un nombre corto de FAT (8 letras, A-Z y 0-9) sacado del titulo: "Zemlyane - Grass near home..." -> ZEMLYANE.
Cada archivo tiene que pesar 32 MB o menos (es lo que BASIC-OS baja de una vez): si el .BVC pasa, se reconvierte
mas chico (menos cuadros por segundo, luego menos resolucion, luego gris).
Lo que sale va a la carpeta videos/ del repo (la que lee el hub).
"""
import os, re, shutil, subprocess, sys, unicodedata

EXTS = (".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v")
LIMIT = 32 * 1024 * 1024
# si el .BVC no cabe, se intenta con esto (en orden)
SMALLER = [[], ["-f", "15"], ["-w", "192", "-h", "144", "-f", "15"], ["-w", "160", "-h", "120", "-f", "12"],
           ["-w", "160", "-h", "120", "-f", "12", "-r", "8000"],                  # audio a 8 kHz
           ["-w", "160", "-h", "120", "-f", "12", "-r", "8000", "-t", "24"],      # menos pixeles actualizados por cuadro
           ["-w", "160", "-h", "120", "-f", "10", "-r", "8000", "-t", "24"],
           ["-w", "128", "-h", "96", "-f", "10", "-r", "8000", "-t", "24"]]


def latin(text):
    """titulo legible en BASIC-OS (la consola solo pinta ASCII): cirilico transliterado, comillas anchas normales"""
    out = []
    for ch in unicodedata.normalize("NFKC", text):
        lo = ch.lower()
        if lo in CYR:
            t = CYR[lo]
            out.append(t.capitalize() if ch != lo else t)
        elif ch in "\u29f8\u2215\u2044":
            out.append("/")
        elif ch in "\u2013\u2014":
            out.append("-")
        elif ch in "\u201c\u201d\u00ab\u00bb":
            out.append('"')
        elif ord(ch) < 128:
            out.append(ch)
        else:
            d = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode()
            out.append(d)
    return re.sub(r"\s+", " ", "".join(out)).strip()


def short_name(title, used):
    base = re.sub(r"[^A-Za-z0-9]", "", latin(os.path.splitext(title)[0])).upper()
    base = base[:8] or "VIDEO"
    name, n = base, 1
    while name in used:
        n += 1
        suf = str(n)
        name = base[: 8 - len(suf)] + suf
    used.add(name)
    return name


def newer(dst, src):
    return os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src)


def make_bvc(mkbvc, src, dst, opts):
    for extra in SMALLER:
        tmp = dst + ".tmp"
        r = subprocess.run([mkbvc, src, tmp] + opts + extra)
        if r.returncode != 0:
            if os.path.exists(tmp):
                os.unlink(tmp)
            return "no se pudo convertir (falta ffmpeg?)"
        if os.path.getsize(tmp) <= LIMIT:
            os.replace(tmp, dst)
            return None
        print("videos:   %.1f MB no cabe en 32 MB, otra vez mas chico..." % (os.path.getsize(tmp) / 1048576), flush=True)
        os.unlink(tmp)
    return "ni a 128x96 y 10 cuadros cabe en 32 MB (es muy largo)"


def make_mp4(src, dst):
    if src.lower().endswith(".mp4") and os.path.getsize(src) <= LIMIT:
        shutil.copyfile(src, dst)
        return None
    for crf, h in (("28", "480"), ("32", "360"), ("36", "240")):
        tmp = dst + ".tmp.mp4"
        r = subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", src, "-vf", "scale=-2:'min(%s,ih)'" % h,
                            "-c:v", "libx264", "-preset", "veryfast", "-crf", crf, "-c:a", "aac", "-b:a", "96k",
                            "-movflags", "+faststart", tmp])
        if r.returncode != 0:
            if os.path.exists(tmp):
                os.unlink(tmp)
            return "ffmpeg no pudo pasarlo a MP4"
        if os.path.getsize(tmp) <= LIMIT:
            os.replace(tmp, dst)
            return None
        os.unlink(tmp)
    return "ni a 240p cabe en 32 MB"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out, opts = sys.argv[1], sys.argv[2], sys.argv[3:]
    if not os.path.isdir(src):
        sys.exit("videos: no existe la carpeta %s" % src)
    mkbvc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkbvc")
    os.makedirs(out, exist_ok=True)
    vids = sorted(f for f in os.listdir(src) if f.lower().endswith(EXTS))
    used, report = set(), []
    for title in vids:
        path = os.path.join(src, title)
        name = short_name(title, used)
        with open(os.path.join(out, name + ".TXT"), "w", encoding="utf-8") as f:
            f.write(latin(re.sub(r"\s*\[[A-Za-z0-9_-]{11}\]$", "", os.path.splitext(title)[0])) + "\n")   # sin el [id] de YouTube
        bvc, mp4 = os.path.join(out, name + ".BVC"), os.path.join(out, name + ".MP4")
        notes = []
        if not newer(bvc, path):
            print("videos: %s -> %s.BVC ..." % (title, name), flush=True)
            e = make_bvc(mkbvc, path, bvc, opts)
            if e:
                notes.append("BVC: " + e)
        if not newer(mp4, path):
            print("videos: %s -> %s.MP4 ..." % (title, name), flush=True)
            e = make_mp4(path, mp4)
            if e:
                notes.append("MP4: " + e)
        sizes = "  ".join("%s %.1f MB" % (os.path.basename(p), os.path.getsize(p) / 1048576) for p in (bvc, mp4) if os.path.exists(p))
        report.append("%-8s %s   [%s]%s" % (name, title, sizes, ("  ERROR " + "; ".join(notes)) if notes else ""))
    print()
    for r in report:
        print("videos: " + r)
    print("\nvideos: listo en %s/ -> copialo a la carpeta videos/ del repo y haz push" % out)


main()
