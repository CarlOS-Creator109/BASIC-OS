# BASIC-OS Hub

Servidor de programas para BASIC-OS. Lee los `.BIN` de una carpeta de tu repo de GitHub (y guarda ahi lo que subas) y los sirve por URL
(HTTPS en Render). En el SO, `PKG` lista lo que hay, descarga el programa que elijas por Ethernet, comprueba su
SHA-256 y lo ejecuta.

```
 GitHub (store/*.BIN)  --->  Hub en Render (hub/server.py)  --HTTPS-->  BASIC-OS: NET.APP (iPXE) -> PKG.APP
```

## 1. Programas de la tienda

Un programa es un modulo PCBM (como `DOOM.BIN`) enlazado a **0x9000000** (hasta 32 MB) que recibe la tabla de
servicios `K` de `kapi.h`. Ejemplo completo en `apps/hello.c`:

```
make store          # apps/NOMBRE.c  ->  store/NOMBRE.BIN   (agrega tu app a STOREAPPS en el Makefile)
```

- Nombres **8.3** en mayusculas: `NOMBRE.BIN` (el SO solo lee nombres cortos).
- `store/NOMBRE.TXT` (opcional): su primera linea es la descripcion que se ve en `PKG`.
- Sube `store/` a GitHub (`git add store && git commit && git push`). El hub se entera solo (cache de 2 min).

## 2. Desplegar el hub en Render

1. Sube este repo a GitHub (ver abajo).
2. En Render: **New > Blueprint**, elige el repo (usa `render.yaml`). O **New > Web Service** con
   *Runtime* Python, *Build command* `echo ok`, *Start command* `python3 hub/server.py`.
3. En **Environment** del servicio pon `GH_REPO` = `tu-usuario/tu-repo` (y `GH_BRANCH`/`GH_PATH` si cambiaste algo).
   Si el repo es privado o llegas al limite de la API de GitHub, agrega `GITHUB_TOKEN` (un token de solo lectura).
4. Abre `https://TU-SERVICIO.onrender.com/` : debe listar tus programas. `/list` es lo que lee el SO.

Notas de Render gratis: el servicio se duerme tras 15 min sin visitas y la primera peticion tarda ~1 min en despertar
(el SO espera). Para evitarlo hay que usar un plan de pago.

## 3. Apuntar BASIC-OS al hub

- En el SO escribe `PKG` y teclea la direccion del hub cuando la pida (se pierde al apagar), **o**
- crea el archivo `HUB.CFG` (una linea: `https://TU-SERVICIO.onrender.com`) en la particion FAT. Al armar la imagen:
  `make pc-basic-doom.img HUB_URL=https://TU-SERVICIO.onrender.com`

Antes de armar la imagen compila la red con HTTPS: `make net` (baja iPXE de GitHub, ~2 min; deja `tools/net/NET.BIN`).

Requisitos en la maquina: tarjeta de red soportada por iPXE (e1000, rtl8139/8168, virtio...), cable al router con DHCP,
~180 MB de RAM, y el RTC con la fecha mas o menos bien (HTTPS rechaza certificados si el reloj esta en el 2000).
En QEMU: `-m 256 -nic user,model=e1000`.

## 4. Subir paquetes desde BASIC-OS

En `PKG`, la tecla **S** sube un programa guardado con `SAVE` (como `NOMBRE.BAS`) o un archivo de la particion
(`HD0.1:RUTA/X.BIN`, modulo PCBM enlazado a 0x9000000). Para que el hub las acepte:

1. En **Environment** del servicio pon `HUB_KEY` = una clave larga. Sin ella las subidas estan apagadas (403).
2. Donde se guardan:
   - con `GITHUB_TOKEN` (token *fine-grained* con permiso **Contents: Read and write** solo en tu repo) el hub hace commit
     de `store/NOMBRE.BIN|BAS` y `NOMBRE.TXT` (la descripcion) en `GH_BRANCH`. Es lo recomendado en Render: el disco de
     Render gratis se borra en cada reinicio.
   - con `LOCAL_DIR` se escriben en esa carpeta.
   - sin ninguno de los dos se quedan solo en memoria y se pierden al reiniciar.
3. En el SO, la clave va en la **segunda linea** de `HUB.CFG` (o PKG la pregunta):
   `make super HUB_URL=https://TU-SERVICIO.onrender.com HUB_KEY=tu-clave`

Protocolo: `POST /upload` con el cuerpo `BOSUP 1\nNOMBRE\nCLAVE\nDESCRIPCION\n` + los bytes. Respuesta
`OK NOMBRE bytes sha256` (PKG la compara con su propia suma) o `ERROR ...` con 400/403/413. Maximo 2 MB
(`bospost` copia el cuerpo al heap de iPXE, de 4 MB).

Probar sin BASIC-OS:
```
printf 'BOSUP 1\nHOLA.BAS\ntu-clave\nprueba\n10 PRINT "HOLA"\n' | curl --data-binary @- https://TU-SERVICIO.onrender.com/upload
```

## 5. Como se mantiene al dia con GitHub

GitHub es la unica fuente de verdad; el hub no guarda programas en su disco (Render gratis lo borra al reiniciar) ni en RAM:

- **Al arrancar o despertar** relee la carpeta del repo (si GitHub no contesta, insiste cada pocos segundos y mientras tanto responde 503, que PKG
  muestra como "no pude bajar la lista"). La lista solo guarda nombre, tamano, SHA-256 y descripcion; los bytes se piden a GitHub cuando
  BASIC-OS descarga (API de blobs: siempre el contenido exacto de ese commit).
- **Si alguien pide la lista y esta vieja** (`CACHE_TTL`, 120 s) se sirve lo que hay y se relee por detras. La relectura usa ETag: si nada
  cambio GitHub contesta 304 y no cuenta para el limite de la API.
- **Webhook (opcional, instantaneo):** en tu repo de GitHub, Settings > Webhooks > Add: *Payload URL* `https://TU-SERVICIO.onrender.com/refresh`,
  *Content type* `application/json`, *Secret* = lo mismo que pongas en `WEBHOOK_SECRET`, evento *Just the push event*.
- **Si GitHub se cae** sigue sirviendo la ultima lista buena y lo dice en la pagina de estado (`/`).
- **Subidas:** `PKG` > **S** hace un commit en `GH_BRANCH` con `GH_PATH/NOMBRE.BIN|BAS` y `NOMBRE.TXT`. Necesita en el Environment de Render:
  `GH_REPO`, `GITHUB_TOKEN` (*fine-grained*, solo ese repo, permiso **Contents: Read and write**) y `HUB_KEY`. Solo con `GH_REPO` el hub
  puede leer pero no escribir. Sin `GITHUB_TOKEN` ni `LOCAL_DIR` las subidas quedan en memoria y se pierden al reiniciar.
- Con `GITHUB_TOKEN` el limite de la API es de 5000 peticiones/hora; sin el, 60 (se nota si bajas varios programas distintos).

## 6. Varias tiendas en un mismo hub (store, liberty, plugins...)
`STORES=store,liberty,plugins` (ya viene en `render.yaml`) hace que un solo servicio sirva varias carpetas del repo.
- La **primera** es la principal: vive en `GH_PATH` y responde en la raiz (`/list`, `/get/X`, `/upload`), asi que nada cambia para quien ya la usa.
- Las demas viven en la carpeta con su nombre (`liberty/`, `plugins/`) y en `/NOMBRE/...`: `/liberty/list`, `/liberty/get/X`, `POST /liberty/upload`.
  Si la carpeta no existe todavia es una tienda vacia: se crea sola con la primera subida.
- **En BASIC-OS** basta cambiar la direccion del hub con la tecla `U` de PKG: `https://tu-hub.onrender.com/liberty`
  (o ponerla en la 1a linea de `HUB.CFG`). Los programas bajados se guardan todos en `HDn.1:/APPS/`: ojo, dos tiendas con el mismo nombre de programa se pisan en el disco.
- **Claves:** cada tienda usa `NOMBRE_KEY` (`PLUGINS_KEY`...) y si no existe, `HUB_KEY`.
- **`OPEN_STORES=liberty`**: esa tienda acepta subidas **sin clave y sin revision**; cualquiera puede subir y reemplazar un programa que ya existe
  (en GitHub queda el historial para revertir). Es a proposito (tienda "anarquica"), pero lo que baja PKG se ejecuta con control total del sistema:
  trata esa tienda como no confiable.
- El webhook de GitHub (`/refresh`) relee todas las tiendas.

## 7. Foro de texto (/foro)

El mismo hub sirve un foro para `FORO.APP`. No hay que configurar nada nuevo: usa `GH_REPO`, `GITHUB_TOKEN` y `HUB_KEY`.
Cada hilo es `foro/NNNNNN.txt` en el repo (la carpeta se crea con el primer hilo); cada mensaje es un commit.

```
# Titulo del hilo
== autor | 2026-11-18 16:40 UTC
texto del mensaje
```

- `GET /foro/list`, `GET /foro/hilo/NNNNNN`, `POST /foro/post` (formato en la cabecera de `server.py`).
- Moderar = editar o borrar el archivo en GitHub. Con el webhook puesto el hub lo ve al momento; si no, en `CACHE_TTL`.
- Variables opcionales: `FORO=0` (apagar), `FORO_PATH`, `FORO_KEY`, `FORO_OPEN=1`, `FORO_MIN_GAP` (15 s), `FORO_PER_HOUR` (20).
- La clave de la comunidad sirve para escribir: quien la tenga puede publicar. El limite por IP frena el spam, no lo impide.

## Seguridad (leer)

- **Lo que baja PKG se ejecuta con todos los privilegios del SO.** Usa solo tu propio repo. Con `HUB_KEY`, quien tenga
  la clave puede subir programas que otros van a ejecutar: no la publiques ni la metas en imagenes que compartas. Por eso PKG no trae ninguna
  direccion de hub por defecto.
- HTTPS autentica el hub (NET.BIN lleva embebidos los certificados raiz de Mozilla) y PKG comprueba el SHA-256 de la
  lista, pero el hub confia en lo que haya en tu repo de GitHub: quien pueda hacer push alli puede cambiar programas.
- La fuente de aleatoriedad de TLS en maquinas sin RDRAND (Turion II, QEMU `qemu64`) es el jitter del RTC contra el TSC
  (`bosstash`/`bos_entropy` en `tools/net/overlay/interface/basicos/basicos.c`): suficiente para bajar programas, no la
  uses para secretos de verdad.

## Probarlo sin Render

```
LOCAL_DIR=store PORT=8080 python3 hub/server.py        # http://localhost:8080/list
# con HTTPS local (para QEMU): TLS_CERT=cadena.pem TLS_KEY=clave.pem ... ; el NET.BIN debe confiar en esa CA:
TRUST=mi-ca.pem tools/net/build.sh
```
