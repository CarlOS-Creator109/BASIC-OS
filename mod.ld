/* mod.ld - enlaza un modulo de /SYS a MODBASE (se pasa con --defsym=MODBASE=0x...) */
ENTRY(mod_main)
SECTIONS
{
    . = MODBASE;
    .header : { KEEP(*(.header)) }
    .text   : { *(.text) *(.text.*) }
    .rodata : ALIGN(4) { *(.rodata) *(.rodata.*) }
    .data   : ALIGN(4) { *(.data) *(.data.*) }
    __mod_filled = .;
    .bss    : ALIGN(16) { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(16);
    __mod_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
