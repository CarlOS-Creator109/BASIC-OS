# BASIC-OS Hub

Servidor de programas para BASIC-OS. Baja los `.BIN` de una carpeta de tu repo de GitHub y los sirve por URL
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

Requisitos en la maquina: tarjeta de red soportada por iPXE (e1000, rtl8139/8168, virtio...), cable al router con DHCP,
~180 MB de RAM, y el RTC con la fecha mas o menos bien (HTTPS rechaza certificados si el reloj esta en el 2000).
En QEMU: `-m 256 -nic user,model=e1000`.

## Seguridad (leer)

- **Lo que baja PKG se ejecuta con todos los privilegios del SO.** Usa solo tu propio repo. Por eso PKG no trae ninguna
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
