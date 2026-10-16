; entry.asm - Entrada del kernel: de modo protegido (32 bits) a modo largo (64 bits)
;
; boot.asm nos deja en modo protegido ejecutando en 0x10000. Aqui se comprueba que
; la CPU soporta x86-64, se arman tablas de paginacion con identidad para el primer
; GB, se activa PAE + LME + paginacion y se salta a codigo de 64 bits.

%define PML4 0x70000
%define PDPT 0x71000
%define PD   0x72000
%define PT   0x73000
%define PD1  0x74000                    ; PD extra para el framebuffer (si esta sobre 1 GB)

extern kmain
extern __bss_start
extern __bss_end

section .text
global _start

[bits 32]
_start:
    cli
    cld
    mov esp, 0x90000

    mov eax, 0x80000000             ; la CPU soporta CPUID extendido?
    cpuid
    cmp eax, 0x80000001
    jb .no64
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; bit LM (long mode)
    jz .no64

    mov edi, PML4                   ; limpiar 5 paginas de tablas
    mov ecx, 5 * 4096 / 4
    xor eax, eax
    rep stosd

    mov dword [PML4], PDPT | 3
    mov dword [PDPT], PD | 3
    mov dword [PD], PT | 3

    ; PT: los primeros 2 MB en paginas de 4 KB (0xA0000-0xBFFFF sin cache: VGA)
    mov edi, PT
    xor ebx, ebx
.pt:
    mov eax, ebx
    shl eax, 12
    or  eax, 3
    cmp ebx, 0xA0
    jb  .ptw
    cmp ebx, 0xC0
    jae .ptw
    or  eax, 0x18                   ; PCD | PWT
.ptw:
    mov [edi], eax
    mov dword [edi + 4], 0
    add edi, 8
    inc ebx
    cmp ebx, 512
    jb  .pt

    ; PD[1..511]: paginas de 2 MB (hasta 1 GB)
    mov edi, PD + 8
    mov eax, 0x200000 | 0x83
    mov ecx, 511
.pd:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .pd

    ; Framebuffer VBE: las paginas necesarias (hasta 32) de 2 MB con PWT. Con el PAT reprogramado abajo,
    ; PWT selecciona la entrada 1 = write-combining (escrituras rapidas, sin cache).
    cmp byte [0x600B], 0
    je .nofb
    movzx eax, word [0x6008]        ; paginas de 2 MB = (offset + pitch*alto) / 2 MB, max 32
    movzx edx, word [0x6006]
    mul edx
    mov esi, [0x600C]
    and esi, 0x1FFFFF
    add eax, esi
    add eax, 0x1FFFFF
    shr eax, 21
    mov ecx, eax
    cmp ecx, 32
    jbe .fbn
    mov ecx, 32
.fbn:
    mov eax, [0x600C]               ; direccion fisica del framebuffer
    mov edx, eax
    shr edx, 30                     ; indice en el PDPT (cada entrada = 1 GB)
    mov edi, PD
    test edx, edx
    jz .fbpd
    mov edi, PD1
    mov dword [PDPT + edx * 8], PD1 | 3
    mov dword [PDPT + edx * 8 + 4], 0
.fbpd:
    mov ebx, eax
    shr ebx, 21
    and ebx, 511                    ; indice dentro del PD
    and eax, 0xFFE00000
    or  eax, 0x8B                   ; P | RW | PS | PWT
.fbloop:
    cmp ebx, 512
    jae .nofb
    mov [edi + ebx * 8], eax
    mov dword [edi + ebx * 8 + 4], 0
    add eax, 0x200000
    inc ebx
    loop .fbloop
.nofb:

    mov ecx, 0x277                  ; IA32_PAT: entrada 1 = write-combining (01h)
    mov eax, 0x00070106
    mov edx, 0x00070406
    wrmsr

    mov eax, PML4
    mov cr3, eax
    mov eax, cr4
    or  eax, 1 << 5                 ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or  eax, 1 << 8                 ; LME
    wrmsr
    mov eax, cr0
    or  eax, 1 << 31                ; PG
    mov cr0, eax

    lgdt [gdt64_ptr]
    push dword 0x08
    push dword long_mode
    retf

.no64:                              ; sin x86-64: mensaje en rojo y detenerse
    mov esi, msg_no64
    mov edi, 0xB8000
.msg:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x4F
    stosw
    jmp .msg
.halt:
    hlt
    jmp .halt

[bits 64]
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, stack_top

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
    incbin "thunk.bin"
thunk_end:

align 8
gdt64:
    dq 0x0000000000000000
    dq 0x00AF9A000000FFFF           ; 0x08 codigo 64 bits
    dq 0x00CF92000000FFFF           ; 0x10 datos
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64
msg_no64: db "SE NECESITA CPU DE 64 BITS", 0

section .stack nobits alloc write
align 16
    resb 32768
stack_top:
