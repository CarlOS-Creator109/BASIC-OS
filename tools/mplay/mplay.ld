/* mplay.ld - MPLAY.BIN: programa de la tienda en 0x9000000, con lo que necesita musl (init_array, TLS) */
ENTRY(mod_main)
SECTIONS
{
    . = MODBASE;
    .header : { KEEP(*(.header)) }
    .text   : { *(.text) *(.text.*) }
    .rodata : ALIGN(64) { *(.rodata) *(.rodata.*) }
    .preinit_array : ALIGN(8) { __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .; }
    .init_array : ALIGN(8) { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .; }
    .fini_array : ALIGN(8) { __fini_array_start = .; KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) __fini_array_end = .; }
    .data   : ALIGN(64) { *(.data) *(.data.*) }
    .tdata  : ALIGN(64) { __tdata_start = .; *(.tdata) *(.tdata.*) __tdata_end = .; }
    .tbss   : ALIGN(64) { __tbss_start = .; *(.tbss) *(.tbss.*) __tbss_end = .; }
    __mod_filled = .;
    .bss    : ALIGN(64) { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(64);
    __mod_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.hash) *(.hash) *(.dynsym) *(.dynstr) }
}
