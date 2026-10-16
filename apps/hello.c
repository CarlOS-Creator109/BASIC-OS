/* hello.c - programa de ejemplo para la tienda (hub). Se compila con:  make store
 *
 * Un programa de la tienda es un modulo PCBM normal (ver mod_head.h) enlazado a 0x9000000 (hasta 32 MB) que recibe la
 * tabla de servicios K (kapi.h). PKG.APP lo baja por HTTPS, comprueba su SHA-256 y llama a mod_main(K).
 */
#include "kapi.h"
#include "mod_head.h"

int mod_main(const struct kapi *K)
{
    int i;
    K->print("\n  HOLA DESDE EL HUB DE BASIC-OS!\n");
    K->print("  Este programa llego por HTTPS desde Internet y se ejecuta en RAM.\n\n");
    K->sound(SND_SINE, 660, 0, 150);
    K->sleep(200);
    K->sound(SND_SINE, 880, 0, 250);
    K->print("  (cualquier tecla para volver)\n");
    for (i = 0; i < 6000; i++) {
        if (K->poll())
            break;
        K->sleep(5);
    }
    return 0;
}
