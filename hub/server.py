#!/usr/bin/env python3
"""BASIC-OS Hub: sirve a BASIC-OS los programas (.BIN) de una carpeta de un repo de GitHub.

Solo usa la biblioteca estandar de Python (nada que instalar). Pensado para Render (render.yaml), pero corre en
cualquier sitio:  python3 hub/server.py

Variables de entorno
  GH_REPO       "usuario/repositorio" de GitHub con los programas           (obligatoria salvo LOCAL_DIR)
  GH_BRANCH     rama                                                          (main)
  GH_PATH       carpeta del repo donde estan los .BIN                         (store)
  GITHUB_TOKEN  token (opcional: sube el limite de la API o permite repos privados)
  CACHE_TTL     segundos que se confia en la lista antes de volver a mirar GitHub   (120)
  LOCAL_DIR     sirve una carpeta local en vez de GitHub (pruebas sin red)
  PORT          puerto (Render lo pone solo)                                   (8080)
  TLS_CERT / TLS_KEY   solo para pruebas locales con HTTPS (Render ya pone el HTTPS por fuera)

Rutas
  /list         texto plano, una linea por programa:  NOMBRE.BIN <bytes> <sha256> <descripcion>
  /get/NOMBRE.BIN   el programa (application/octet-stream, con Content-Length)
  /             pagina de estado      /health   "ok"

Un programa es  <GH_PATH>/NOMBRE.BIN  (nombre 8.3, el SO solo lee nombres cortos) y, opcionalmente,
<GH_PATH>/NOMBRE.TXT cuya primera linea es la descripcion.
"""
import hashlib, json, os, re, ssl, sys, threading, time, urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAME_RE = re.compile(r"^[A-Za-z0-9_~-]{1,8}\.BIN$", re.I)
MAX_BYTES = 16 * 1024 * 1024           # el SO reserva 32 MB para un programa de la tienda
CFG = {
    "repo": os.environ.get("GH_REPO", ""),
    "branch": os.environ.get("GH_BRANCH", "main"),
    "path": os.environ.get("GH_PATH", "store").strip("/"),
    "token": os.environ.get("GITHUB_TOKEN", ""),
    "ttl": float(os.environ.get("CACHE_TTL", "120")),
    "local": os.environ.get("LOCAL_DIR", ""),
}

LOCK = threading.Lock()
CACHE = {"at": 0.0, "apps": {}, "error": "", "source": ""}     # apps: NAME -> dict(size, sha256, desc, data, gitsha)


def http_get(url, accept=None):
    req = urllib.request.Request(url, headers={"User-Agent": "basicos-hub/1"})
    if accept:
        req.add_header("Accept", accept)
    if CFG["token"] and ("api.github.com" in url or "raw.githubusercontent.com" in url):
        req.add_header("Authorization", "Bearer " + CFG["token"])
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def scan_local(old):
    d = CFG["local"]
    apps = {}
    for fn in sorted(os.listdir(d)):
        if not NAME_RE.match(fn):
            continue
        data = open(os.path.join(d, fn), "rb").read()
        if len(data) > MAX_BYTES:
            continue
        desc = ""
        tp = os.path.join(d, fn[:-4] + ".TXT")
        if not os.path.exists(tp):
            tp = os.path.join(d, fn[:-4] + ".txt")
        if os.path.exists(tp):
            desc = (open(tp, encoding="utf-8", errors="replace").read().splitlines() or [""])[0]
        apps[fn.upper()] = dict(size=len(data), sha256=hashlib.sha256(data).hexdigest(), desc=desc, data=data, gitsha="")
    return apps


def scan_github(old):
    api = "https://api.github.com/repos/%s/contents%s?ref=%s" % (CFG["repo"], ("/" + CFG["path"]) if CFG["path"] else "", CFG["branch"])
    items = json.loads(http_get(api, "application/vnd.github+json"))
    by = {i["name"]: i for i in items if i.get("type") == "file"}
    byu = {k.upper(): v for k, v in by.items()}
    apps = {}
    for fn, it in sorted(by.items()):
        if not NAME_RE.match(fn) or it.get("size", 0) > MAX_BYTES:
            continue
        key = fn.upper()
        prev = old.get(key)
        if prev and prev["gitsha"] == it["sha"]:                # no cambio: no se vuelve a bajar
            data, sha = prev["data"], prev["sha256"]
        else:
            data = http_get(it["download_url"])
            sha = hashlib.sha256(data).hexdigest()
        desc = prev["desc"] if prev and prev["gitsha"] == it["sha"] else ""
        if not desc:
            tn = byu.get(fn[:-4].upper() + ".TXT")
            if tn:
                try:
                    desc = (http_get(tn["download_url"]).decode("utf-8", "replace").splitlines() or [""])[0]
                except Exception:
                    pass
        apps[key] = dict(size=len(data), sha256=sha, desc=desc, data=data, gitsha=it["sha"])
    return apps


def refresh(force=False):
    """Devuelve el catalogo; si GitHub falla sirve el ultimo bueno."""
    with LOCK:
        if not force and time.time() - CACHE["at"] < CFG["ttl"] and CACHE["source"]:
            return CACHE
        try:
            if CFG["local"]:
                apps, src = scan_local(CACHE["apps"]), "carpeta " + CFG["local"]
            elif CFG["repo"]:
                apps, src = scan_github(CACHE["apps"]), "github.com/%s/%s@%s" % (CFG["repo"], CFG["path"], CFG["branch"])
            else:
                raise RuntimeError("falta GH_REPO (o LOCAL_DIR)")
            CACHE.update(at=time.time(), apps=apps, error="", source=src)
        except Exception as e:                                     # noqa: BLE001
            CACHE["error"] = "%s: %s" % (type(e).__name__, e)
            CACHE["at"] = time.time() - CFG["ttl"] + 15            # reintentar en 15 s
            if not CACHE["source"]:
                CACHE["source"] = "(sin datos)"
            print("refresh fallo:", CACHE["error"], file=sys.stderr, flush=True)
        return CACHE


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "basicos-hub/1"

    def log_message(self, fmt, *a):
        print("%s %s" % (self.address_string(), fmt % a), flush=True)

    def send(self, code, body, ctype="text/plain; charset=utf-8", head=False):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def route(self, head):
        path = self.path.split("?", 1)[0]
        if path == "/health":
            return self.send(200, "ok\n", head=head)
        c = refresh()
        apps = c["apps"]
        if path == "/list":
            lines = ["#HUB 1 %d" % len(apps)]
            for n, a in sorted(apps.items()):
                lines.append("%s %d %s %s" % (n, a["size"], a["sha256"], a["desc"].replace("\n", " ")))
            return self.send(200, "\n".join(lines) + "\n", head=head)
        if path.startswith("/get/"):
            a = apps.get(path[5:].upper())
            if not a:
                return self.send(404, "no existe\n", head=head)
            return self.send(200, a["data"], "application/octet-stream", head=head)
        if path in ("/", "/index.html"):
            rows = "".join("<tr><td>%s</td><td align=right>%d</td><td>%s</td></tr>" % (esc(n), a["size"], esc(a["desc"]))
                           for n, a in sorted(apps.items()))
            err = "<p><b>Ultimo error:</b> %s</p>" % esc(c["error"]) if c["error"] else ""
            html = ("<!doctype html><meta charset=utf-8><title>BASIC-OS Hub</title><h1>BASIC-OS Hub</h1>"
                    "<p>Fuente: %s &middot; %d programas</p>%s<table border=1 cellpadding=4>"
                    "<tr><th>Programa<th>Bytes<th>Descripcion</tr>%s</table>"
                    "<p>En BASIC-OS: <code>EXEC \"PKG.APP\"</code></p>") % (esc(c["source"]), len(apps), err, rows)
            return self.send(200, html, "text/html; charset=utf-8", head=head)
        return self.send(404, "no existe\n", head=head)

    def do_GET(self):
        self.route(False)

    def do_HEAD(self):
        self.route(True)


def main():
    port = int(os.environ.get("PORT", "8080"))
    srv = ThreadingHTTPServer(("0.0.0.0", port), H)
    if os.environ.get("TLS_CERT"):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2      # iPXE habla TLS 1.2 (Render/Cloudflare tambien aceptan 1.2)
        ctx.load_cert_chain(os.environ["TLS_CERT"], os.environ["TLS_KEY"])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print("BASIC-OS Hub en el puerto", port, flush=True)
    threading.Thread(target=refresh, args=(True,), daemon=True).start()
    srv.serve_forever()


if __name__ == "__main__":
    main()
