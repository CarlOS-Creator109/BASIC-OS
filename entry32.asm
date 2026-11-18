; entry32.asm - Entrada del kernel en la variante de 32 bits (ARCH=32)
;
; boot.asm / stage2.asm nos dejan en modo protegido ejecutando en 0x10000, igual que para la
; version de 64 bits. Aqui NO se activa paginacion ni modo largo: se carga una GDT plana,
; se pone a cero el bss y se salta a kmain() en modo protegido de 32 bits.
; Corre en cualquier CPU i686 y en emuladores x86 de 32 bits (v86 en el navegador).

extern kmain
extern __bss_start
extern __bss_end

section .text
global _start

[bits 32]
_start:
    cli
    cld
    lgdt [gdt32_ptr]
    jmp 0x08:.reload
.reload:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top

    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .rodata
global thunk_blob, thunk_end
thunk_blob:
    incbin "thunk32.bin"
thunk_end:

align 8
gdt32:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF           ; 0x08 codigo 32 bits plano
    dq 0x00CF92000000FFFF           ; 0x10 datos plano
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

section .stack nobits alloc write
align 16
    resb 32768
stack_top:
