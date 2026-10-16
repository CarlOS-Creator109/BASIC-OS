/* mod_head.h - cabecera comun de los modulos de /SYS: se incluye una vez por modulo */
#ifndef MOD_HEAD_H
#define MOD_HEAD_H
__asm__(".section .header,\"a\"\n"
        ".long 0x4D424350\n"        /* 'PCBM' */
        ".long __mod_end\n"
        ".long mod_main\n"
        ".long MODBASE\n"           /* direccion de carga: el kernel la lee antes de cargar */
        ".text\n");
#endif
