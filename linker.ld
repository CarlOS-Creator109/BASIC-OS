/* linker.ld - el kernel se carga y ejecuta en 0x10000 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(4) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4) {
        *(.data)
        *(.data.*)
    }

    . = 0x100000;                       /* bss y pila sobre 1 MB: abajo queda sitio para el codigo */

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    .stack (NOLOAD) : ALIGN(16) {
        *(.stack)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
