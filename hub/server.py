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
  STORES        varias tiendas en un mismo hub, separadas por comas          (store)
                La primera es la principal: vive en GH_PATH y responde tambien en la raiz (/list, /get/X, /upload).
                Las demas viven en la carpeta del repo con su nombre (liberty -> liberty/) y en /NOMBRE/list ...
                En BASIC-OS basta poner la direccion  https://mi-hub.onrender.com/liberty  (tecla U de PKG).
  NOMBRE_KEY    clave de subida de esa tienda (LIBERTY_KEY, PLUGINS_KEY...); si falta se usa HUB_KEY
  OPEN_STORES   tiendas que aceptan subidas SIN clave ni revision, separadas por comas (liberty)
  VIDEOS        "0" apaga los videos (/videos). Son NOMBRE.BVC (PLAY) y/o NOMBRE.MP4 (MPLAY) (+ NOMBRE.TXT) en VIDEO_PATH (1)
  VIDEO_PATH    carpeta del repo con los videos                                                                  (videos)
  FORO          "0" apaga el foro de texto (/foro/...). Cada hilo es un archivo <FORO_PATH>/NNNNNN.txt del repo.   (1)
  FORO_PATH     carpeta del repo con los hilos                                                                   (foro)
  FORO_KEY      clave para escribir en el foro; si falta se usa HUB_KEY. FORO_OPEN=1 deja escribir sin clave.
  FORO_MIN_GAP  segundos minimos entre mensajes de la misma IP (15) y FORO_PER_HOUR mensajes por IP y hora (20)

Rutas (con varias tiendas, delante va /NOMBRE: /liberty/list, /liberty/get/X, POST /liberty/upload)
  /list         texto plano, una linea por programa:  NOMBRE.BIN <bytes> <sha256> <descripcion>
  /get/NOMBRE.BIN   el programa (application/octet-stream, con Content-Length)
  POST /upload  cuerpo "BOSUP 1\nNOMBRE\nCLAVE\nDESCRIPCION\n" + los bytes del paquete (lo manda PKG con bospost).
                Responde "OK NOMBRE bytes sha256" o un error (403 clave, 400 paquete no valido, 413 muy grande).
  POST /refresh webhook de GitHub (solo con WEBHOOK_SECRET): relee el repo ya
  /foro/list    "#FORO 1 n" y una linea por hilo:  ID mensajes fecha ultimo_autor titulo   (los mas recientes primero)
  /foro/hilo/ID el hilo en texto plano (solo ASCII: sin acentos, para la consola de BASIC-OS; con ?u=1 en UTF-8 tal cual,
                para clientes de fuera como tools/forosdl). /foro/list?u=1 igual.
  POST /foro/post  cuerpo "BOSFORO 1\nCLAVE\nAUTOR\nHILO\nTITULO\n" + el mensaje. HILO = el ID de un hilo o NEW (y entonces
                lleva TITULO). Responde "OK ID" o un error (403 clave, 400 no valido, 409 hilo lleno, 429 muy seguido).
  Moderar: borra o edita el archivo del hilo en GitHub (el webhook o el CACHE_TTL lo recogen).
  /videos       pagina HTML (en COSMOS: clic en un .BVC lo baja y lo reproduce PLAY; clic en un .MP4 lo guarda en
                HDn.1:/VIDEOS/ para abrirlo con MPLAY)
  /videos/list  "#VIDEOS 1 n" y una linea por archivo:  NOMBRE.BVC (o .MP4) bytes descripcion
  /videos/get/NOMBRE.BVC   el video (o NOMBRE.MP4). make videos (tools/videos.py) los prepara desde una carpeta de .mp4. Los videos se suben al repo con git (VIDEO_PATH/NOMBRE.BVC, hasta 32 MB: lo que cabe en
                la memoria de BASIC-OS) o por la web de GitHub (hasta 25 MB).
  /             pagina de estado de todas las tiendas      /health   "ok"

Un paquete es  <GH_PATH>/NOMBRE.BIN  o  NOMBRE.BAS  (nombre 8.3, el SO solo lee nombres cortos) y, opcionalmente,
<GH_PATH>/NOMBRE.TXT cuya primera linea es la descripcion.
"""
import base64, collections, textwrap, hashlib, hmac, json, os, re, ssl, sys, threading, time, unicodedata, urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAME_RE = re.compile(r"^[A-Za-z0-9_~-]{1,8}\.(BIN|BAS)$", re.I)
VIDEO_RE = re.compile(r"^[A-Za-z0-9_~-]{1,8}\.(BVC|MP4)$", re.I)
VIDEO_MAX = 32 * 1024 * 1024           # BASIC-OS baja el video a 0x9000000 y tiene hasta 0xB000000
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
LOCK = threading.Lock()                 # protege las cachés, LRU y DESCS (se toma un instante: nunca durante la red)
LRU = collections.OrderedDict()         # ultimos programas bajados, por sha de git (inmutable: nunca se queda viejo)
LRU_MAX = 6 * 1024 * 1024
DESCS = {}                              # sha de git de un NOMBRE.TXT -> su descripcion
STORE_RE = re.compile(r"^[a-z0-9_-]{1,16}$")


class Store:
    """Una tienda = una carpeta del repo con su lista, su caché y sus reglas de subida."""

    def __init__(self, name, path, key, is_open, default, name_re=NAME_RE, max_bytes=MAX_BYTES, hashed=True):
        self.name, self.path, self.key, self.open, self.default = name, path, key, is_open, default
        self.name_re, self.max_bytes, self.hashed = name_re, max_bytes, hashed   # hashed=False: sin SHA-256 (videos grandes)
        self.scan = threading.Lock()                       # una sola relectura a la vez
        # apps: NOMBRE -> dict(size, sha256, desc, gitsha, path, data)   (data solo en uploads)
        self.cache = {"at": 0.0, "apps": {}, "error": "", "source": "", "etag": "", "scans": 0, "changed": 0.0}
        self.ready = threading.Event()                     # se enciende con la primera relectura BUENA
        self.uploads = {}                                  # subidas sin donde guardarlas (ni LOCAL_DIR ni GITHUB_TOKEN): en memoria

    @property
    def prefix(self):
        return "" if self.default else "/" + self.name

    @property
    def local_dir(self):
        d = CFG["local"]
        return d if self.default else os.path.join(d, self.path)


def make_stores():
    names = [n.strip().lower() for n in os.environ.get("STORES", "store").split(",") if n.strip()] or ["store"]
    opened = {n.strip().lower() for n in os.environ.get("OPEN_STORES", "").split(",") if n.strip()}
    out = {}
    for n in names:
        if not STORE_RE.match(n) or n in ("health", "refresh", "list", "get", "upload", "foro"):
            raise SystemExit("nombre de tienda no valido: %r (usa a-z 0-9 _ -)" % n)
        if n in ("videos",):
            raise SystemExit("la tienda no se puede llamar %r (es la ruta de los videos)" % n)
        if n in out:
            continue
        default = not out
        key = os.environ.get(n.upper().replace("-", "_") + "_KEY", "") or CFG["key"]
        out[n] = Store(n, CFG["path"] if default else n, key, n in opened, default)
    return out


STORES = make_stores()
DEFAULT = next(iter(STORES.values()))
VIDEOS = None
if os.environ.get("VIDEOS", "1") != "0":
    VIDEOS = Store("videos", os.environ.get("VIDEO_PATH", "videos").strip("/") or "videos", "", False, False,
                   name_re=VIDEO_RE, max_bytes=VIDEO_MAX, hashed=False)


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------- GitHub ----------------
def gh_hdr(accept="application/vnd.github+json"):
    h = {"User-Agent": "basicos-hub/2", "Accept": accept}
    if CFG["token"]:
        h["Authorization"] = "Bearer " + CFG["token"]
    return h


def gh_blob(gitsha, cache=True):
    """Los bytes de un archivo por su sha de git (API de blobs: siempre exacto, sin cache viejo de CDN)."""
    with LOCK:
        if gitsha in LRU:
            LRU.move_to_end(gitsha)
            return LRU[gitsha]
    url = "%s/repos/%s/git/blobs/%s" % (CFG["api"], CFG["repo"], gitsha)
    req = urllib.request.Request(url, headers=gh_hdr("application/vnd.github.raw+json"))
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    if cache:
        lru_put(gitsha, data)
    return data


def lru_put(gitsha, data):
    with LOCK:
        LRU[gitsha] = data
        LRU.move_to_end(gitsha)
        while sum(len(v) for v in LRU.values()) > LRU_MAX and len(LRU) > 1:
            LRU.popitem(last=False)


def gh_put(st, name, data, message):
    """Crea o reemplaza CARPETA/name en el repo con la API de contenidos de GitHub. Devuelve el sha de git."""
    path = ("%s/%s" % (st.path, name)) if st.path else name
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


def scan_github(st, old, etag):
    """Lee la carpeta del repo. Devuelve (apps, etag) o None si GitHub dice que no cambio (304: no cuenta para el limite).
    Una carpeta que todavia no existe en una tienda secundaria es una tienda vacia (se crea con la primera subida)."""
    api = "%s/repos/%s/contents%s?ref=%s" % (CFG["api"], CFG["repo"], ("/" + st.path) if st.path else "", CFG["branch"])
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
        if e.code == 404 and not st.default:
            return {}, ""
        raise
    by = {i["name"]: i for i in items if i.get("type") == "file"}
    byu = {k.upper(): v for k, v in by.items()}
    apps = {}
    for fn, it in sorted(by.items()):
        if not st.name_re.match(fn) or it.get("size", 0) > st.max_bytes:
            continue
        key = fn.upper()
        prev = old.get(key)
        if not st.hashed:                                                         # videos: ni se bajan ni se suman
            size, sha256 = it.get("size", 0), ""
        elif prev and prev["gitsha"] == it["sha"] and prev.get("data") is None:  # no cambio: no se vuelve a bajar
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
def scan_local(st, old):
    d = st.local_dir
    apps = {}
    if not os.path.isdir(d):
        return apps, ""
    for fn in sorted(os.listdir(d)):
        if not st.name_re.match(fn):
            continue
        p = os.path.join(d, fn)
        stt = os.stat(p)
        if stt.st_size > st.max_bytes:
            continue
        stamp = "%d:%d" % (stt.st_size, stt.st_mtime_ns)
        prev = old.get(fn.upper())
        if not st.hashed:
            sha256 = ""
        else:
            sha256 = prev["sha256"] if prev and prev["gitsha"] == stamp else hashlib.sha256(open(p, "rb").read()).hexdigest()
        desc = ""
        tp = os.path.join(d, fn[:-4] + ".TXT")
        if not os.path.exists(tp):
            tp = os.path.join(d, fn[:-4] + ".txt")
        if os.path.exists(tp):
            desc = (open(tp, encoding="utf-8", errors="replace").read().splitlines() or [""])[0]
        apps[fn.upper()] = dict(size=stt.st_size, sha256=sha256, desc=desc, gitsha=stamp, path=p, data=None)
    return apps, ""


# ---------------- la lista ----------------
def refresh(st, force=False):
    """Relee el repo. Los pedidos nunca esperan a esto (salvo el primero): leen la caché tal como esta."""
    c = st.cache
    with st.scan:
        with LOCK:
            fresh = time.time() - c["at"] < CFG["ttl"] and c["source"]
            old, etag = dict(c["apps"]), ("" if force else c["etag"])
        if fresh and not force:
            return
        try:
            if CFG["local"]:
                res, src = scan_local(st, old), "carpeta " + st.local_dir
            elif CFG["repo"]:
                res, src = scan_github(st, old, etag), "github.com/%s/%s@%s" % (CFG["repo"], st.path, CFG["branch"])
            else:
                raise RuntimeError("falta GH_REPO (o LOCAL_DIR)")
            with LOCK:
                if res is not None:
                    apps, new_etag = res
                    apps.update(st.uploads)
                    if {k: v["sha256"] for k, v in apps.items()} != {k: v["sha256"] for k, v in c["apps"].items()}:
                        c["changed"] = time.time()
                    c.update(apps=apps, etag=new_etag)
                c.update(at=time.time(), error="", source=src, scans=c["scans"] + 1)
            st.ready.set()
        except Exception as e:                                     # noqa: BLE001
            with LOCK:                                             # se sigue sirviendo lo ultimo bueno
                c["error"] = "%s: %s" % (type(e).__name__, e)
                c["at"] = time.time() - CFG["ttl"] + 15           # reintentar en 15 s
                if not c["source"]:
                    c["source"] = "(sin datos)"
            log("relectura fallo (%s):" % st.name, c["error"])


def refresh_async(st, force=False):
    threading.Thread(target=refresh, args=(st, force), daemon=True).start()


def startup_scan(st):
    """Al arrancar: relee el repo y, si GitHub no contesta (Render recien despierto, red...), insiste hasta lograrlo."""
    wait = 3
    while True:
        refresh(st, True)
        if not st.cache["error"]:
            log("repo leido: tienda %s: %d programas de %s" % (st.name, len(st.cache["apps"]), st.cache["source"]))
            return
        time.sleep(wait)
        wait = min(wait * 2, 60)


def catalog(st):
    if not st.ready.is_set():
        st.ready.wait(25)                                          # el primer pedido espera a la primera lectura buena
    with LOCK:
        stale = time.time() - st.cache["at"] >= CFG["ttl"]
    if stale and not st.scan.locked():
        refresh_async(st)                                          # se sirve lo que hay y se actualiza por detras
    with LOCK:
        return dict(st.cache, apps=dict(st.cache["apps"]))


def get_data(name, a):
    if a.get("data") is not None:
        return a["data"]
    if a["path"]:
        return open(a["path"], "rb").read()
    data = gh_blob(a["gitsha"], cache=bool(a["sha256"]))
    if a["sha256"] and hashlib.sha256(data).hexdigest() != a["sha256"]:
        raise RuntimeError("el SHA-256 de %s ya no coincide con la lista" % name)
    return data


# ---------------- subidas ----------------
def store_upload(st, name, desc, data):
    """Guarda un paquete subido. Devuelve (sha256, donde)."""
    sha = hashlib.sha256(data).hexdigest()
    entry = dict(size=len(data), sha256=sha, desc=desc, gitsha="", path=None, data=None)
    txtname = name[:-4] + ".TXT"
    if CFG["local"]:
        d = st.local_dir
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
        if desc:
            with open(os.path.join(d, txtname), "w", encoding="utf-8") as f:
                f.write(desc + "\n")
        entry["path"] = os.path.join(d, name)
        where = "carpeta " + d
    elif CFG["repo"] and CFG["token"]:
        entry["gitsha"] = gh_put(st, name, data, "BASIC-OS (%s): sube %s" % (st.name, name))
        lru_put(entry["gitsha"], data)
        if desc:
            tsha = gh_put(st, txtname, (desc + "\n").encode("utf-8"), "BASIC-OS (%s): descripcion de %s" % (st.name, name))
            DESCS[tsha] = desc.splitlines()[0] if desc else ""
        where = "github.com/%s/%s" % (CFG["repo"], st.path)
    else:
        entry["data"] = data
        st.uploads[name] = entry
        where = "memoria (se pierde al reiniciar: pon GITHUB_TOKEN o LOCAL_DIR)"
    with LOCK:
        st.cache["apps"][name] = entry
        st.cache["changed"] = time.time()
        st.cache["etag"] = ""                                      # la carpeta cambio: la proxima lectura no usa el ETag viejo
    return sha, where


# ---------------- foro de texto ----------------
# Cada hilo es un archivo <FORO_PATH>/NNNNNN.txt:   "# Titulo" y despues los mensajes, cada uno con su linea "== autor | fecha UTC".
# GitHub guarda todo (como la tienda); el hub solo ordena, limita y anade mensajes (un commit por mensaje).
FORO = {
    "on": os.environ.get("FORO", "1") != "0",
    "path": os.environ.get("FORO_PATH", "foro").strip("/") or "foro",
    "key": os.environ.get("FORO_KEY", "") or CFG["key"],
    "open": os.environ.get("FORO_OPEN", "") == "1",
    "gap": float(os.environ.get("FORO_MIN_GAP", "15")),
    "per_hour": int(os.environ.get("FORO_PER_HOUR", "20")),
}
F_ID = re.compile(r"^\d{6}\.txt$")
F_TITLE_MAX, F_NICK_MAX, F_MSG_MAX = 50, 16, 1500       # caracteres
F_THREAD_MAX, F_POSTS_MAX, F_THREADS_MAX = 40 * 1024, 60, 500
F_LIST_MAX = 80                                         # hilos que baja el sistema (los mas recientes)
F_TABLE = str.maketrans({"¿": "?", "¡": "!", "“": '"', "”": '"', "‘": "'", "’": "'", "–": "-", "—": "-", "…": "...",
                         "ñ": "n", "Ñ": "N", "€": "EUR"})


def ascii_fold(s):
    """La consola de BASIC-OS solo pinta ASCII: quita acentos y lo que no se pueda."""
    s = s.translate(F_TABLE)
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode("ascii")


def clean_line(s, limit):
    s = "".join(ch if (ch >= " " and ch != "\x7f") else " " for ch in s.replace("\t", " "))
    return " ".join(s.split())[:limit]


def parse_thread(text):
    lines = text.split("\n")
    title = lines[0][2:].strip() if lines and lines[0].startswith("# ") else "(sin titulo)"
    posts = [l for l in lines if l.startswith("== ")]
    last_nick, last_when = "-", "0000-00-00 00:00"
    if posts:
        m = re.match(r"^== (\S+) \| (\d{4}-\d\d-\d\d \d\d:\d\d)", posts[-1])
        if m:
            last_nick, last_when = m.group(1), m.group(2)
    return title, len(posts), last_nick, last_when


class Forum:
    def __init__(self):
        self.lock = threading.Lock()                       # protege threads y los contadores (nunca durante la red)
        self.scan = threading.Lock()                       # una relectura a la vez
        self.write = threading.Lock()                      # un mensaje a la vez (asi nunca se pisan dos en el mismo hilo)
        self.threads = {}                                  # ID(6 digitos) -> dict(text, gitsha, title, n, nick, when)
        self.at, self.etag, self.error, self.source = 0.0, "", "", ""
        self.ready = threading.Event()
        self.hits = collections.defaultdict(list)          # IP -> momentos de sus mensajes

    @property
    def local_dir(self):
        return os.path.join(CFG["local"], FORO["path"])

    def _entry(self, text, gitsha):
        title, n, nick, when = parse_thread(text)
        return dict(text=text, gitsha=gitsha, title=title, n=n, nick=nick, when=when)

    def refresh(self, force=False):
        with self.scan:
            with self.lock:
                fresh = time.time() - self.at < CFG["ttl"] and self.source
                old, etag = dict(self.threads), ("" if force else self.etag)
            if fresh and not force:
                return
            try:
                if CFG["local"]:
                    new, src, new_etag = self._scan_local(), "carpeta " + self.local_dir, ""
                elif CFG["repo"]:
                    res = self._scan_github(old, etag)
                    new, new_etag = res if res is not None else (None, etag)
                    src = "github.com/%s/%s@%s" % (CFG["repo"], FORO["path"], CFG["branch"])
                else:
                    new, new_etag, src = None, "", "memoria (pon GITHUB_TOKEN o LOCAL_DIR para que se guarde)"
                with self.lock:
                    if new is not None:
                        self.threads, self.etag = new, new_etag
                    self.at, self.error, self.source = time.time(), "", src
                self.ready.set()
            except Exception as e:                         # noqa: BLE001
                with self.lock:
                    self.error = "%s: %s" % (type(e).__name__, e)
                    self.at = time.time() - CFG["ttl"] + 15
                    self.source = self.source or "(sin datos)"
                log("relectura del foro fallo:", self.error)

    def _scan_local(self):
        out, d = {}, self.local_dir
        if os.path.isdir(d):
            for fn in sorted(os.listdir(d)):
                if F_ID.match(fn) and os.path.getsize(os.path.join(d, fn)) <= 2 * F_THREAD_MAX:
                    out[fn[:6]] = self._entry(open(os.path.join(d, fn), encoding="utf-8", errors="replace").read(), "")
        return out

    def _scan_github(self, old, etag):
        api = "%s/repos/%s/contents/%s?ref=%s" % (CFG["api"], CFG["repo"], FORO["path"], CFG["branch"])
        hdr = gh_hdr()
        if etag:
            hdr["If-None-Match"] = etag
        try:
            with urllib.request.urlopen(urllib.request.Request(api, headers=hdr), timeout=30) as r:
                items, new_etag = json.loads(r.read()), r.headers.get("ETag", "")
        except urllib.error.HTTPError as e:
            if e.code == 304:
                return None
            if e.code == 404:                              # todavia no hay carpeta: foro vacio
                return {}, ""
            raise
        out = {}
        for it in sorted((i for i in items if i.get("type") == "file" and F_ID.match(i["name"])), key=lambda i: i["name"])[-F_THREADS_MAX:]:
            if it.get("size", 0) > 2 * F_THREAD_MAX:
                continue
            tid = it["name"][:6]
            prev = old.get(tid)
            if prev and prev["gitsha"] == it["sha"]:
                out[tid] = prev
            else:
                out[tid] = self._entry(gh_blob(it["sha"]).decode("utf-8", "replace"), it["sha"])
        return out, new_etag

    def snapshot(self):
        if not self.ready.is_set():
            self.ready.wait(25)
        with self.lock:
            stale = time.time() - self.at >= CFG["ttl"]
        if stale and not self.scan.locked():
            threading.Thread(target=self.refresh, daemon=True).start()
        with self.lock:
            return dict(self.threads)

    # ---- escribir ----
    def _rate(self, ip):
        now = time.time()
        with self.lock:
            h = [t for t in self.hits[ip] if now - t < 3600]
            self.hits[ip] = h
            if h and now - h[-1] < FORO["gap"]:
                return "ERROR espera %d s antes de escribir otra vez\n" % int(FORO["gap"] - (now - h[-1]) + 1)
            if len(h) >= FORO["per_hour"]:
                return "ERROR demasiados mensajes de esta conexion; intenta mas tarde\n"
            if len(self.hits) > 5000:                      # no crecer sin limite
                for k in [k for k, v in self.hits.items() if not v or now - v[-1] > 3600]:
                    del self.hits[k]
            h.append(now)
        return None

    def post(self, ip, key, nick, hid, title, msg):
        """Anade un mensaje (o crea un hilo si hid == NEW). Devuelve (codigo_http, texto)."""
        if not FORO["open"] and (not FORO["key"] or not hmac.compare_digest(key.encode(), FORO["key"].encode())):
            return 403, "ERROR clave equivocada (o el foro esta cerrado)\n"
        nick = re.sub(r"[^A-Za-z0-9_.-]", "_", clean_line(ascii_fold(nick), F_NICK_MAX).replace(" ", "_"))
        out = []
        for l in msg.replace("\r", "").split("\n"):        # renglones de hasta 78 letras (la consola de BASIC-OS): se parten, no se cortan
            l = clean_line(l, F_MSG_MAX)
            out.extend(textwrap.wrap(l, 78, break_long_words=True, break_on_hyphens=False) or [""])
        msg = "\n".join(out).strip("\n")
        msg = re.sub(r"\n{3,}", "\n\n", msg)[:F_MSG_MAX]
        msg = "\n".join((" " + l) if l.startswith(("==", "# ")) else l for l in msg.split("\n"))
        if not nick:
            return 400, "ERROR falta tu nombre\n"
        if not msg.strip():
            return 400, "ERROR mensaje vacio\n"
        new = hid.upper() == "NEW"
        if new:
            title = clean_line(title, F_TITLE_MAX)
            if len(title) < 3:
                return 400, "ERROR el titulo es muy corto\n"
        elif not re.match(r"^\d{6}$", hid):
            return 400, "ERROR hilo no valido\n"
        bad = self._rate(ip)
        if bad:
            return 429, bad
        stamp = time.strftime("%Y-%m-%d %H:%M", time.gmtime())
        block = "== %s | %s UTC\n%s\n" % (nick, stamp, msg)
        with self.write:
            try:
                return self._store(new, hid, title if new else "", block, nick)
            except Exception as e:                         # noqa: BLE001
                log("foro: no pude guardar:", e)
                return 502, "ERROR no pude guardarlo: %s\n" % e

    def _store(self, new, hid, title, block, nick):
        local, remote = bool(CFG["local"]), bool(CFG["repo"] and CFG["token"])
        if new:
            with self.lock:
                if len(self.threads) >= F_THREADS_MAX:
                    return 409, "ERROR el foro esta lleno (maximo %d hilos)\n" % F_THREADS_MAX
                nxt = max([int(k) for k in self.threads] or [0]) + 1
            text = "# %s\n%s" % (title, block)
            for _ in range(5):                             # si ese numero ya existe en GitHub, prueba el siguiente
                tid = "%06d" % nxt
                try:
                    sha = self._write(tid, text, None, "Foro: hilo nuevo %s (%s)" % (tid, title), local, remote, create=True)
                    break
                except FileExistsError:
                    nxt += 1
            else:
                return 502, "ERROR no pude crear el hilo (numero ocupado)\n"
        else:
            tid = hid
            cur, sha0 = self._current(tid, local, remote)
            if cur is None:
                return 404, "ERROR ese hilo no existe\n"
            title_, n, _, _ = parse_thread(cur)
            if n >= F_POSTS_MAX or len(cur) + len(block) > F_THREAD_MAX:
                return 409, "ERROR el hilo esta lleno: abre otro\n"
            text = cur if cur.endswith("\n") else cur + "\n"
            text += block
            sha = self._write(tid, text, sha0, "Foro: mensaje en %s (%s)" % (tid, title_), local, remote, create=False)
        with self.lock:
            self.threads[tid] = self._entry(text, sha)
            self.etag = ""
            self.ready.set()
        print("foro: %s en el hilo %s" % (nick, tid), flush=True)
        return 200, "OK %s\n" % tid

    def _current(self, tid, local, remote):
        """Texto actual del hilo y su sha (de GitHub, no de la cache: un moderador pudo haberlo cambiado)."""
        if local:
            p = os.path.join(self.local_dir, tid + ".txt")
            return (open(p, encoding="utf-8", errors="replace").read(), None) if os.path.exists(p) else (None, None)
        if remote:
            api = "%s/repos/%s/contents/%s/%s.txt?ref=%s" % (CFG["api"], CFG["repo"], FORO["path"], tid, CFG["branch"])
            try:
                with urllib.request.urlopen(urllib.request.Request(api, headers=gh_hdr()), timeout=30) as r:
                    j = json.loads(r.read())
                return base64.b64decode(j["content"]).decode("utf-8", "replace"), j["sha"]
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    return None, None
                raise
        with self.lock:
            t = self.threads.get(tid)
        return (t["text"], None) if t else (None, None)

    def _write(self, tid, text, sha0, message, local, remote, create):
        data = text.encode("utf-8")
        if local:
            os.makedirs(self.local_dir, exist_ok=True)
            p = os.path.join(self.local_dir, tid + ".txt")
            if create and os.path.exists(p):
                raise FileExistsError(p)
            with open(p, "wb") as f:
                f.write(data)
            return ""
        if remote:
            api = "%s/repos/%s/contents/%s/%s.txt" % (CFG["api"], CFG["repo"], FORO["path"], tid)
            body = {"message": message, "content": base64.b64encode(data).decode(), "branch": CFG["branch"]}
            if sha0:
                body["sha"] = sha0
            req = urllib.request.Request(api, data=json.dumps(body).encode(), headers=gh_hdr(), method="PUT")
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    return json.loads(r.read())["content"]["sha"]
            except urllib.error.HTTPError as e:
                if create and e.code in (409, 422):       # ya existia ese numero
                    raise FileExistsError(tid)
                raise
        return ""                                          # solo memoria


FORUM = Forum()


def foro_start():
    if not FORO["on"]:
        return
    wait = 3
    while True:
        FORUM.refresh(True)
        if not FORUM.error:
            log("foro leido: %d hilos de %s" % (len(FORUM.threads), FORUM.source))
            return
        time.sleep(wait)
        wait = min(wait * 2, 60)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "basicos-hub/3"

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

    def pick(self, path):
        """(tienda, ruta dentro de la tienda). /liberty/list -> (liberty, /list); /list -> (principal, /list)."""
        seg = path.split("/", 2)
        if len(seg) >= 2 and seg[1] in STORES:
            return STORES[seg[1]], "/" + (seg[2] if len(seg) > 2 else "")
        return DEFAULT, path

    def status_page(self, head):
        out = []
        for st in STORES.values():
            c = catalog(st)
            apps = c["apps"]
            rows = "".join("<tr><td>%s</td><td align=right>%d</td><td>%s</td></tr>" % (esc(n), a["size"], esc(a["desc"]))
                           for n, a in sorted(apps.items()))
            err = "<p><b>Ultimo error:</b> %s</p>" % esc(c["error"]) if c["error"] else ""
            ago = int(time.time() - c["at"]) if c["at"] else -1
            addr = "https://TU-HUB" + ("/" + st.name if not st.default else "")
            out.append(("<h2>%s%s</h2><p>Fuente: %s &middot; %d programas &middot; leido hace %d s (%d lecturas) &middot; %s</p>%s"
                        "<table border=1 cellpadding=4><tr><th>Programa<th>Bytes<th>Descripcion</tr>%s</table>"
                        "<p>Direccion en PKG (tecla U): <code>%s</code></p>")
                       % (esc(st.name), " (principal)" if st.default else "", esc(c["source"]), len(apps), ago, c["scans"],
                          "subidas SIN clave" if st.open else ("subidas con clave" if st.key else "subidas apagadas"), err, rows, addr))
        if VIDEOS:
            cv = catalog(VIDEOS)
            out.append("<h2>videos</h2><p>%d videos &middot; fuente: %s &middot; <a href=\"/videos\">ver</a>%s</p>" % (
                len(cv["apps"]), esc(cv["source"] or "(arrancando)"),
                (" &middot; <b>Ultimo error:</b> " + esc(cv["error"])) if cv["error"] else ""))
        if FORO["on"]:
            out.append("<h2>foro</h2><p>%d hilos &middot; fuente: %s &middot; %s%s</p>" % (
                len(FORUM.threads), esc(FORUM.source or "(arrancando)"),
                "escribir sin clave" if FORO["open"] else ("escribir con clave" if FORO["key"] else "escribir apagado"),
                (" &middot; <b>Ultimo error:</b> " + esc(FORUM.error)) if FORUM.error else ""))
        html = ("<!doctype html><meta charset=utf-8><title>BASIC-OS Hub</title><h1>BASIC-OS Hub</h1>"
                "%s<p>En BASIC-OS: <code>EXEC \"PKG.APP\"</code> (tienda) y <code>EXEC \"FORO.APP\"</code> (foro)</p>") % "".join(out)
        return self.send(200, html, "text/html; charset=utf-8", head=head)

    def route(self, head):
        full = self.path.split("?", 1)[0]
        if full == "/health":
            return self.send(200, "ok\n", head=head)
        if full in ("/", "/index.html"):
            return self.status_page(head)
        if full == "/foro" or full.startswith("/foro/"):
            return self.foro_get(full, head)
        if full == "/videos" or full.startswith("/videos/"):
            return self.videos_get(full, head)
        st, path = self.pick(full)
        c = catalog(st)
        apps = c["apps"]
        if not st.ready.is_set() and (path == "/list" or path.startswith("/get/")):
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
                refresh_async(st, True)
                return self.send(502, "ERROR no pude leerlo de GitHub: %s\n" % e)
            return self.send(200, data, "application/octet-stream")
        if path in ("/", "/index.html"):                            # /liberty/  -> la pagina de estado
            return self.status_page(head)
        return self.send(404, "no existe\n", head=head)

    def foro_get(self, full, head):
        if not FORO["on"]:
            return self.send(404, "el foro esta apagado\n", head=head)
        path = full[5:]
        if path in ("", "/"):
            return self.send(200, "Foro de BASIC-OS: /foro/list, /foro/hilo/ID y POST /foro/post\n", head=head)
        query = self.path.split("?", 1)[1] if "?" in self.path else ""
        utf = "u=1" in query.split("&")                 # ?u=1: texto original (UTF-8, con acentos) para clientes de fuera
        fold = (lambda x: x) if utf else ascii_fold
        th = FORUM.snapshot()
        if not FORUM.ready.is_set():
            return self.send(503, "ERROR el foro todavia no pudo leer GitHub (%s). Reintenta en unos segundos.\n" % (FORUM.error or "arrancando"), head=head)
        if path == "/list":
            rows = sorted(th.items(), key=lambda kv: (kv[1]["when"], kv[0]), reverse=True)[:F_LIST_MAX]
            lines = ["#FORO 1 %d" % len(rows)]
            for tid, t in rows:
                lines.append("%s %d %s %s %s" % (tid, t["n"], t["when"][:10], fold(t["nick"]) or "-", fold(t["title"])))
            return self.send(200, "\n".join(lines) + "\n", head=head)
        if path.startswith("/hilo/"):
            t = th.get(path[6:])
            if not t:
                return self.send(404, "no existe\n", head=head)
            return self.send(200, fold(t["text"]), head=head)
        return self.send(404, "no existe\n", head=head)

    def videos_get(self, full, head):
        st = VIDEOS
        if st is None:
            return self.send(404, "los videos estan apagados\n", head=head)
        path = full[7:] or "/"
        c = catalog(st)
        vids = c["apps"]
        if not st.ready.is_set():
            return self.send(503, "ERROR el hub todavia no pudo leer GitHub (%s). Reintenta en unos segundos.\n" % (c["error"] or "arrancando"), head=head)
        if path == "/list":
            lines = ["#VIDEOS 1 %d" % len(vids)]
            for n, a in sorted(vids.items()):
                lines.append("%s %d %s" % (n, a["size"], a["desc"].replace("\n", " ")))
            return self.send(200, "\n".join(lines) + "\n", head=head)
        if path.startswith("/get/"):
            name = path[5:].upper()
            a = vids.get(name)
            if not a:
                return self.send(404, "no existe\n", head=head)
            if head:
                return self.send(200, b"", "application/octet-stream", head=True, length=a["size"])
            try:
                data = get_data(name, a)
            except Exception as e:                                 # noqa: BLE001
                log("video fallo:", e)
                refresh_async(st, True)
                return self.send(502, "ERROR no pude leerlo de GitHub: %s\n" % e)
            return self.send(200, data, "application/octet-stream")
        if path in ("/", "/index.html"):
            def size(a):
                mb = a["size"] / (1024 * 1024)
                return ("%.1f MB" % mb) if mb >= 1 else ("%d KB" % max(1, a["size"] // 1024))
            groups = {}
            for n, a in vids.items():                              # NOMBRE.BVC y NOMBRE.MP4 van en el mismo renglon
                groups.setdefault(n[:-4], {})[n[-3:].upper()] = (n, a)
            rows = []
            for base in sorted(groups):
                g = groups[base]
                desc = next((a["desc"] for _, a in g.values() if a["desc"]), "")
                links = []
                if "BVC" in g:
                    links.append("<a href=\"/videos/get/%s\" style=\"color:#b8c76f\">ver con PLAY</a> (%s)" % (esc(g["BVC"][0]), size(g["BVC"][1])))
                if "MP4" in g:
                    links.append("<a href=\"/videos/get/%s\" style=\"color:#9ad284\">bajar MP4 para MPLAY</a> (%s)" % (esc(g["MP4"][0]), size(g["MP4"][1])))
                rows.append("<li><b>%s</b>%s<br>%s</li>" % (esc(base), (" &middot; " + esc(desc)) if desc else "", " &middot; ".join(links)))
            body = ("<!doctype html><html><head><meta charset=utf-8><title>Videos de BASIC-OS</title></head>"
                    "<body style=\"background:#352879;color:#ffffff;margin:16px\">"
                    "<h1 style=\"color:#b8c76f\">Videos de BASIC-OS</h1>"
                    "<p>Cada video viene en dos formas. <b>.BVC</b> es el de PLAY (cada cuadro se dibuja del centro hacia afuera): "
                    "en COSMOS un clic lo baja y lo reproduce a pantalla completa (<b>Espacio</b> pausa, <b>ESC</b> sale). "
                    "<b>.MP4</b> es el video original: en COSMOS un clic lo guarda en <code>HD0.1:/VIDEOS/</code> y se abre con "
                    "<b>MPLAY</b> (paquete de la tienda). En otro navegador el clic lo descarga.</p>"
                    "<h2 style=\"color:#9ad284\">%d %s</h2><ul>%s</ul>"
                    "<p style=\"color:#959595\">Subir videos: pon los .mp4 en una carpeta y corre <code>make videos VIDEOS=carpeta</code>; "
                    "copia lo que sale en <code>videos/</code> a la carpeta <code>%s/</code> del repo (nombre 8.3, hasta 32 MB cada uno; "
                    "<code>NOMBRE.TXT</code> = descripcion).</p>"
                    "<p><a href=\"/\" style=\"color:#70a4b2\">Estado del hub</a></p></body></html>") % (
                len(groups), "video" if len(groups) == 1 else "videos",
                "".join(rows) or "<li>Todavia no hay videos.</li>", esc(st.path))
            return self.send(200, body, "text/html; charset=utf-8", head=head)
        return self.send(404, "no existe\n", head=head)

    def foro_post(self):
        body = self.body(8192)
        if body is None:
            return
        if not FORO["on"]:
            return self.send(404, "el foro esta apagado\n")
        parts = body.split(b"\n", 5)
        if len(parts) != 6 or parts[0].strip() != b"BOSFORO 1":
            return self.send(400, "ERROR formato desconocido\n")
        d = [x.decode("utf-8", "replace") for x in parts[1:]]            # clave, autor, hilo, titulo, mensaje
        ip = (self.headers.get("X-Forwarded-For", "").split(",")[0].strip()) or self.address_string()
        code, msg = FORUM.post(ip, d[0].strip(), d[1], d[2].strip(), d[3], d[4])
        if code != 200:
            print("foro: rechazado (%d) de %s" % (code, ip), flush=True)
        return self.send(code, msg)

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
        full = self.path.split("?", 1)[0]
        if full == "/refresh":
            return self.webhook()
        if full == "/foro/post":
            return self.foro_post()
        st, p = self.pick(full)
        if p != "/upload":
            return self.send(404, "no existe\n")
        body = self.body(POST_MAX + 512)
        if body is None:
            return
        if not st.open and not st.key:
            return self.send(403, "ERROR las subidas estan apagadas en esta tienda (falta la clave)\n")
        parts = body.split(b"\n", 4)
        if len(parts) != 5 or parts[0].strip() != b"BOSUP 1":
            return self.send(400, "ERROR formato de subida desconocido\n")
        name = parts[1].decode("ascii", "replace").strip().upper()
        key = parts[2].strip()
        desc = parts[3].decode("utf-8", "replace").strip()[:120]
        data = parts[4]
        if not st.open and not hmac.compare_digest(key, st.key.encode()):
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
            sha, where = store_upload(st, name, desc, data)
        except Exception as e:                                     # noqa: BLE001
            print("subida fallo:", e, file=sys.stderr, flush=True)
            return self.send(502, "ERROR no pude guardarlo: %s\n" % e)
        print("subido %s (%d bytes) a la tienda %s -> %s" % (name, len(data), st.name, where), flush=True)
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
        for st in STORES.values():
            refresh_async(st, True)
        if FORO["on"]:
            threading.Thread(target=FORUM.refresh, args=(True,), daemon=True).start()
        if VIDEOS:
            refresh_async(VIDEOS, True)
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
    print("BASIC-OS Hub en el puerto %d, tiendas: %s" % (port, ", ".join(
        "%s%s" % (n, " (abierta)" if st.open else "") for n, st in STORES.items())), flush=True)
    for st in STORES.values():
        threading.Thread(target=startup_scan, args=(st,), daemon=True).start()
    threading.Thread(target=foro_start, daemon=True).start()
    if VIDEOS:
        threading.Thread(target=startup_scan, args=(VIDEOS,), daemon=True).start()
    srv.serve_forever()


if __name__ == "__main__":
    main()
