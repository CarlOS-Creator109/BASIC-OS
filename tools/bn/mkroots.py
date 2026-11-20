#!/usr/bin/env python3
# mkroots.py - arma ROOTS.DER (certs DER concatenados) desde un bundle PEM de CAs raiz.
# uso: mkroots.py bundle.pem salida.der
import sys, base64, re

def main():
    if len(sys.argv) != 3:
        sys.exit("uso: mkroots.py bundle.pem salida.der")
    pem = open(sys.argv[1], "r", errors="ignore").read()
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S)
    out = bytearray()
    n = 0
    for b in blocks:
        try:
            out += base64.b64decode("".join(b.split()))
            n += 1
        except Exception:
            pass
    open(sys.argv[2], "wb").write(out)
    print("ROOTS.DER: %d raices, %d bytes" % (n, len(out)))

if __name__ == "__main__":
    main()
