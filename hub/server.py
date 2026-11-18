#!/usr/bin/env python3
"""BASIC-OS Hub: sirve a BASIC-OS los paquetes (.BIN y .BAS) de una carpeta de un repo de GitHub, y los recibe.

GitHub es la unica fuente de verdad: el hub NO guarda nada propio (Render gratis tiene poco disco y lo borra al reiniciar).
Cada vez que el servicio arranca (o se despierta) relee el repo; despues lo vuelve a mirar cuando la lista envejece
(CACHE_TTL) o cuando GitHub avisa de un push (webhook, ver abajo). Los programas no viven en RAM: la lista solo guarda
nombre, tamano, SHA-256 y descripcion, y los bytes se piden a GitHub cuando BASIC-OS los descarga.

Solo usa la biblioteca estandar de Python (nada que instalar). Pensado para Render (render.yaml), pero corre en
cualquier sitio:  python3 hub/server.py

Variables de entorno
  GH_REPO       "usuario/repositorio" de GitHub con los programas           (obligatoria salvo LOCAL_DIR)
  GH_BRANCH     rama                                                          (main)
  GH_PATH       carpeta del repo donde estan los .BIN                         (store)
  GITHUB_TOKEN  token (muy recomendado: sin el la API de GitHub solo deja 60 peticiones por hora, y hace falta para
                repos privados y para SUBIR, con permiso "Contents: Read and write")
  CACHE_TTL     segundos antes de volver a mirar GitHub si alguien pide la lista      (120)
  WEBHOOK_SECRET  si lo pones, POST /refresh (webhook "push" de GitHub, JSON, mismo secreto) relee el repo al instante
  LOCAL_DIR     sirve una carpeta local en vez de GitHub (pruebas sin red)
  GH_API        direccion de la API de GitHub (solo para pruebas)             (https://api.github.com)
  PORT          puerto (Render lo pone solo)                                   (8080)
  TLS_CERT / TLS_KEY   solo para pruebas locales con HTTPS (Render ya pone el HTTPS por fuera)
  HUB_KEY       clave para SUBIR paquetes desde BASIC-OS (PKG, tecla S). Sin ella las subidas estan apagadas.
                Lo subido se guarda con un commit en el repo (LOCAL_DIR si existe; sin GITHUB_TOKEN ni LOCAL_DIR solo queda
                en memoria y se pierde al reiniciar).

Rutas
  /list         texto plano, una linea por programa:  NOMBRE.BIN <bytes> <sha256> <descripcion>
  /get/NOMBRE.BIN   el programa (application/octet-stream, con Content-Length)
  POST /upload  cuerpo "BOSUP 1\nNOMBRE\nCLAVE\nDESCRIPCION\n" + los bytes del paquete (lo manda PKG con bospost).
                Responde "OK NOMBRE bytes sha256" o un error (403 clave, 400 paquete no valido, 413 muy grande).
  POST /refresh webhook de GitHub (solo con WEBHOOK_SECRET): relee el repo ya
  /             pagina de estado      /health   "ok"

Un paquete es  <GH_PATH>/NOMBRE.BIN  o  NOMBRE.BAS  (nombre 8.3, el SO solo lee nombres cortos) y, opcionalmente,
<GH_PATH>/NOMBRE.TXT cuya primera linea es la descripcion.
"""
import base64, collections, hashlib, hmac, json, os, re, ssl, sys, threading, time, urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAME_RE = re.compile(r"^[A-Za-z0-9_~-]{1,8}\.(BIN|BAS)$", re.I)
POST_MAX = 2 * 1024 * 1024             # lo que puede mandar BASIC-OS (bospost copia al heap de iPXE de 4 MB)
MAX_BYTES = 16 * 1024 * 1024           # el SO reserva 32 MB para un programa de la tienda
CFG = {
    "repo": os.environ.get("GH_REPO", ""),
    "branch": os.environ.get("GH_BRANCH", "main"),
    "path": os.environ.get("GH_PATH", "store").strip("/"),
    "token": os.environ.get("GITHUB_TOKEN", ""),
    "ttl": float(os.environ.get("CACHE_TTL", "120")),
    "local": os.environ.get("LOCAL_DIR", ""),
    "key": os.environ.get("HUB_KEY", ""),
    "hook": os.environ.get("WEBHOOK_SECRET", ""),
    "api": os.environ.get("GH_API", "https://api.github.com").rstrip("/"),
}
UPLOADS = {}                            # subidas sin donde guardarlas (ni LOCAL_DIR ni GITHUB_TOKEN): viven en memoria, con sus bytes

LOCK = threading.Lock()                 # protege CACHE, LRU y DESCS (se toma un instante: nunca durante la red)
SCAN = threading.Lock()                 # una sola relectura a la vez
# apps: NOMBRE -> dict(size, sha256, desc, gitsha, path, data)   (data solo en UPLOADS)
CACHE = {"at": 0.0, "apps": {}, "error": "", "source": "", "etag": "", "scans": 0, "changed": 0.0}
READY = threading.Event()               # se enciende con la primera relectura BUENA (hasta entonces no hay lista que dar)
LRU = collections.OrderedDict()         # ultimos programas bajados, por sha de git (inmutable: nunca se queda viejo)
LRU_MAX = 6 * 1024 * 1024
DESCS = {}                              # sha de git de un NOMBRE.TXT -> su descripcion


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------- GitHub ----------------
def gh_hdr(accept="application/vnd.github+json"):
    h = {"User-Agent": "basicos-hub/2", "Accept": accept}
    if CFG["token"]:
        h["Authorization"] = "Bearer " + CFG["token"]
    return h


def gh_blob(gitsha):
    """Los bytes de un archivo por su sha de git (API de blobs: siempre exacto, sin cache viejo de CDN)."""
    with LOCK:
        if gitsha in LRU:
            LRU.move_to_end(gitsha)
            return LRU[gitsha]
    url = "%s/repos/%s/git/blobs/%s" % (CFG["api"], CFG["repo"], gitsha)
    req = urllib.request.Request(url, headers=gh_hdr("application/vnd.github.raw+json"))
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
    lru_put(gitsha, data)
    return data


def lru_put(gitsha, data):
    with LOCK:
        LRU[gitsha] = data
        LRU.move_to_end(gitsha)
        while sum(len(v) for v in LRU.values()) > LRU_MAX and len(LRU) > 1:
            LRU.popitem(last=False)


def gh_put(name, data, message):
    """Crea o reemplaza GH_PATH/name en el repo con la API de contenidos de GitHub. Devuelve el sha de git."""
    path = ("%s/%s" % (CFG["path"], name)) if CFG["path"] else name
    api = "%s/repos/%s/contents/%s" % (CFG["api"], CFG["repo"], path)
    hdr = gh_hdr()
    sha = None
    try:
        with urllib.request.urlopen(urllib.request.Request(api + "?ref=" + CFG["branch"], headers=hdr), timeout=30) as r:
            sha = json.loads(r.read()).get("sha")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    body = {"message": message, "content": base64.b64encode(data).decode(), "branch": CFG["branch"]}
    if sha:
        body["sha"] = sha
    req = urllib.request.Request(api, data=json.dumps(body).encode(), headers=hdr, method="PUT")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())["content"]["sha"]


def scan_github(old, etag):
    """Lee la carpeta del repo. Devuelve (apps, etag) o None si GitHub dice que no cambio (304: no cuenta para el limite)."""
    api = "%s/repos/%s/contents%s?ref=%s" % (CFG["api"], CFG["repo"], ("/" + CFG["path"]) if CFG["path"] else "", CFG["branch"])
    hdr = gh_hdr()
    if etag:
        hdr["If-None-Match"] = etag
    try:
        with urllib.request.urlopen(urllib.request.Request(api, headers=hdr), timeout=30) as r:
            items = json.loads(r.read())
            new_etag = r.headers.get("ETag", "")
    except urllib.error.HTTPError as e:
        if e.code == 304:
            return None
        raise
    by = {i["name"]: i for i in items if i.get("type") == "file"}
    byu = {k.upper(): v for k, v in by.items()}
    apps = {}
    for fn, it in sorted(by.items()):
        if not NAME_RE.match(fn) or it.get("size", 0) > MAX_BYTES:
            continue
        key = fn.upper()
        prev = old.get(key)
        if prev and prev["gitsha"] == it["sha"] and prev.get("data") is None:        # no cambio: no se vuelve a bajar
            size, sha256 = prev["size"], prev["sha256"]
        else:
            data = gh_blob(it["sha"])                                             # cambio o es nuevo: se baja una vez para su SHA-256
            size, sha256 = len(data), hashlib.sha256(data).hexdigest()
        desc = ""
        tn = byu.get(fn[:-4].upper() + ".TXT")
        if tn:
            if tn["sha"] not in DESCS:
                try:
                    DESCS[tn["sha"]] = (gh_blob(tn["sha"]).decode("utf-8", "replace").splitlines() or [""])[0]
                except Exception as e:                                             # noqa: BLE001
                    log("no pude leer", tn["name"], e)
                    DESCS[tn["sha"]] = ""
            desc = DESCS[tn["sha"]]
        apps[key] = dict(size=size, sha256=sha256, desc=desc, gitsha=it["sha"], path=None, data=None)
    return apps, new_etag


# ---------------- carpeta local (pruebas) ----------------
def scan_local(old):
    d = CFG["local"]
    apps = {}
    for fn in sorted(os.listdir(d)):
        if not NAME_RE.match(fn):
            continue
        p = os.path.join(d, fn)
        st = os.stat(p)
        if st.st_size > MAX_BYTES:
            continue
        stamp = "%d:%d" % (st.st_size, st.st_mtime_ns)
        prev = old.get(fn.upper())
        sha256 = prev["sha256"] if prev and prev["gitsha"] == stamp else hashlib.sha256(open(p, "rb").read()).hexdigest()
        desc = ""
        tp = os.path.join(d, fn[:-4] + ".TXT")
        if not os.path.exists(tp):
            tp = os.path.join(d, fn[:-4] + ".txt")
        if os.path.exists(tp):
            desc = (open(tp, encoding="utf-8", errors="replace").read().splitlines() or [""])[0]
        apps[fn.upper()] = dict(size=st.st_size, sha256=sha256, desc=desc, gitsha=stamp, path=p, data=None)
    return apps, ""


# ---------------- la lista ----------------
def refresh(force=False):
    """Relee el repo. Los pedidos nunca esperan a esto (salvo el primero): leen CACHE tal como esta."""
    with SCAN:
        with LOCK:
            fresh = time.time() - CACHE["at"] < CFG["ttl"] and CACHE["source"]
            old, etag = dict(CACHE["apps"]), ("" if force else CACHE["etag"])
        if fresh and not force:
            return
        try:
            if CFG["local"]:
                res, src = scan_local(old), "carpeta " + CFG["local"]
            elif CFG["repo"]:
                res, src = scan_github(old, etag), "github.com/%s/%s@%s" % (CFG["repo"], CFG["path"], CFG["branch"])
            else:
                raise RuntimeError("falta GH_REPO (o LOCAL_DIR)")
            with LOCK:
                if res is not None:
                    apps, new_etag = res
                    apps.update(UPLOADS)
                    if {k: v["sha256"] for k, v in apps.items()} != {k: v["sha256"] for k, v in CACHE["apps"].items()}:
                        CACHE["changed"] = time.time()
                    CACHE.update(apps=apps, etag=new_etag)
                CACHE.update(at=time.time(), error="", source=src, scans=CACHE["scans"] + 1)
            READY.set()
        except Exception as e:                                     # noqa: BLE001
            with LOCK:                                             # se sigue sirviendo lo ultimo bueno
                CACHE["error"] = "%s: %s" % (type(e).__name__, e)
                CACHE["at"] = time.time() - CFG["ttl"] + 15       # reintentar en 15 s
                if not CACHE["source"]:
                    CACHE["source"] = "(sin datos)"
            log("relectura fallo:", CACHE["error"])


def refresh_async(force=False):
    threading.Thread(target=refresh, args=(force,), daemon=True).start()


def startup_scan():
    """Al arrancar: relee el repo y, si GitHub no contesta (Render recien despierto, red...), insiste hasta lograrlo."""
    wait = 3
    while True:
        refresh(True)
        if not CACHE["error"]:
            log("repo leido: %d programas de %s" % (len(CACHE["apps"]), CACHE["source"]))
            return
        time.sleep(wait)
        wait = min(wait * 2, 60)


def catalog():
    if not READY.is_set():
        READY.wait(25)                                             # el primer pedido espera a la primera lectura buena
    with LOCK:
        stale = time.time() - CACHE["at"] >= CFG["ttl"]
    if stale and not SCAN.locked():
        refresh_async()                                            # se sirve lo que hay y se actualiza por detras
    with LOCK:
        return dict(CACHE, apps=dict(CACHE["apps"]))


def get_data(name, a):
    if a.get("data") is not None:
        return a["data"]
    if a["path"]:
        return open(a["path"], "rb").read()
    data = gh_blob(a["gitsha"])
    if hashlib.sha256(data).hexdigest() != a["sha256"]:
        raise RuntimeError("el SHA-256 de %s ya no coincide con la lista" % name)
    return data


# ---------------- subidas ----------------
def store_upload(name, desc, data):
    """Guarda un paquete subido. Devuelve (sha256, donde)."""
    sha = hashlib.sha256(data).hexdigest()
    entry = dict(size=len(data), sha256=sha, desc=desc, gitsha="", path=None, data=None)
    txtname = name[:-4] + ".TXT"
    if CFG["local"]:
        with open(os.path.join(CFG["local"], name), "wb") as f:
            f.write(data)
        if desc:
            with open(os.path.join(CFG["local"], txtname), "w", encoding="utf-8") as f:
                f.write(desc + "\n")
        entry["path"] = os.path.join(CFG["local"], name)
        where = "carpeta " + CFG["local"]
    elif CFG["repo"] and CFG["token"]:
        entry["gitsha"] = gh_put(name, data, "BASIC-OS: sube %s" % name)
        lru_put(entry["gitsha"], data)
        if desc:
            tsha = gh_put(txtname, (desc + "\n").encode("utf-8"), "BASIC-OS: descripcion de %s" % name)
            DESCS[tsha] = desc.splitlines()[0] if desc else ""
        where = "github.com/%s" % CFG["repo"]
    else:
        entry["data"] = data
        UPLOADS[name] = entry
        where = "memoria (se pierde al reiniciar: pon GITHUB_TOKEN o LOCAL_DIR)"
    with LOCK:
        CACHE["apps"][name] = entry
        CACHE["changed"] = time.time()
        CACHE["etag"] = ""                                         # la carpeta cambio: la proxima lectura no usa el ETag viejo
    return sha, where


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "basicos-hub/2"

    def log_message(self, fmt, *a):
        print("%s %s" % (self.address_string(), fmt % a), flush=True)

    def send(self, code, body, ctype="text/plain; charset=utf-8", head=False, length=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def route(self, head):
        path = self.path.split("?", 1)[0]
        if path == "/health":
            return self.send(200, "ok\n", head=head)
        c = catalog()
        apps = c["apps"]
        if not READY.is_set() and (path == "/list" or path.startswith("/get/")):
            return self.send(503, "ERROR el hub todavia no pudo leer GitHub (%s). Reintenta en unos segundos.\n" % (c["error"] or "arrancando"), head=head)
        if path == "/list":
            lines = ["#HUB 1 %d" % len(apps)]
            for n, a in sorted(apps.items()):
                lines.append("%s %d %s %s" % (n, a["size"], a["sha256"], a["desc"].replace("\n", " ")))
            return self.send(200, "\n".join(lines) + "\n", head=head)
        if path.startswith("/get/"):
            name = path[5:].upper()
            a = apps.get(name)
            if not a:
                return self.send(404, "no existe\n", head=head)
            if head:
                return self.send(200, b"", "application/octet-stream", head=True, length=a["size"])
            try:
                data = get_data(name, a)
            except Exception as e:                                 # noqa: BLE001
                log("get fallo:", e)
                refresh_async(True)
                return self.send(502, "ERROR no pude leerlo de GitHub: %s\n" % e)
            return self.send(200, data, "application/octet-stream")
        if path in ("/", "/index.html"):
            rows = "".join("<tr><td>%s</td><td align=right>%d</td><td>%s</td></tr>" % (esc(n), a["size"], esc(a["desc"]))
                           for n, a in sorted(apps.items()))
            err = "<p><b>Ultimo error:</b> %s</p>" % esc(c["error"]) if c["error"] else ""
            ago = int(time.time() - c["at"]) if c["at"] else -1
            html = ("<!doctype html><meta charset=utf-8><title>BASIC-OS Hub</title><h1>BASIC-OS Hub</h1>"
                    "<p>Fuente: %s &middot; %d programas &middot; repo leido hace %d s (%d lecturas desde que arranco)</p>%s"
                    "<table border=1 cellpadding=4><tr><th>Programa<th>Bytes<th>Descripcion</tr>%s</table>"
                    "<p>En BASIC-OS: <code>EXEC \"PKG.APP\"</code></p>") % (esc(c["source"]), len(apps), ago, c["scans"], err, rows)
            return self.send(200, html, "text/html; charset=utf-8", head=head)
        return self.send(404, "no existe\n", head=head)

    def do_GET(self):
        self.route(False)

    def body(self, limit):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = -1
        if n <= 0:
            self.send(411, "falta Content-Length\n")
            return None
        if n > limit:
            self.send(413, "ERROR demasiado grande\n")
            return None
        return self.rfile.read(n)

    def do_POST(self):
        p = self.path.split("?", 1)[0]
        if p == "/refresh":
            return self.webhook()
        if p != "/upload":
            return self.send(404, "no existe\n")
        body = self.body(POST_MAX + 512)
        if body is None:
            return
        if not CFG["key"]:
            return self.send(403, "ERROR las subidas estan apagadas en este hub (falta HUB_KEY)\n")
        parts = body.split(b"\n", 4)
        if len(parts) != 5 or parts[0].strip() != b"BOSUP 1":
            return self.send(400, "ERROR formato de subida desconocido\n")
        name = parts[1].decode("ascii", "replace").strip().upper()
        key = parts[2].strip()
        desc = parts[3].decode("utf-8", "replace").strip()[:120]
        data = parts[4]
        if not hmac.compare_digest(key, CFG["key"].encode()):
            print("subida rechazada (clave) de", self.address_string(), flush=True)
            return self.send(403, "ERROR clave equivocada\n")
        if not NAME_RE.match(name):
            return self.send(400, "ERROR el nombre debe ser 8.3 terminado en .BIN o .BAS\n")
        if not data:
            return self.send(400, "ERROR paquete vacio\n")
        if name.endswith(".BIN"):
            magic, end, entry, base = (int.from_bytes(data[i:i + 4], "little") for i in (0, 4, 8, 12))
            if len(data) < 16 or magic != 0x4D424350 or base != 0x9000000:
                return self.send(400, "ERROR un .BIN debe ser un modulo PCBM enlazado a 0x9000000\n")
        elif b"\0" in data:
            return self.send(400, "ERROR un .BAS debe ser texto\n")
        try:
            sha, where = store_upload(name, desc, data)
        except Exception as e:                                     # noqa: BLE001
            print("subida fallo:", e, file=sys.stderr, flush=True)
            return self.send(502, "ERROR no pude guardarlo: %s\n" % e)
        print("subido %s (%d bytes) -> %s" % (name, len(data), where), flush=True)
        return self.send(200, "OK %s %d %s\n" % (name, len(data), sha))

    def webhook(self):
        """Webhook 'push' de GitHub: firma X-Hub-Signature-256 = HMAC-SHA256 del cuerpo con WEBHOOK_SECRET."""
        body = self.body(1024 * 1024)
        if body is None:
            return
        if not CFG["hook"]:
            return self.send(403, "ERROR el webhook esta apagado (falta WEBHOOK_SECRET)\n")
        want = "sha256=" + hmac.new(CFG["hook"].encode(), body, hashlib.sha256).hexdigest()
        if not hmac.compare_digest(self.headers.get("X-Hub-Signature-256", ""), want):
            return self.send(403, "ERROR firma equivocada\n")
        if self.headers.get("X-GitHub-Event") == "ping":
            return self.send(200, "pong\n")
        refresh_async(True)
        print("webhook: releyendo el repo", flush=True)
        return self.send(202, "releyendo\n")

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
    threading.Thread(target=startup_scan, daemon=True).start()
    srv.serve_forever()


if __name__ == "__main__":
    main()
