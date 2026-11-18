; thunk32.asm - llamar a la BIOS (INT 13h) desde modo protegido de 32 bits (variante ARCH=32)
;
; Igual que thunk.asm pero sin modo largo: el kernel copia este bloque a 0x8000 y lo llama
; como una funcion C (cdecl). Hace 32 -> 16 protegido -> modo real, ejecuta INT 13h con los
; registros de la zona de parametros (0x8F00) y vuelve por el mismo camino. Preserva
; ebx/esi/edi/ebp, que exige la ABI de i386.

[org 0x8000]

P_AX    equ 0x8F00
P_BX    equ 0x8F02
P_CX    equ 0x8F04
P_DX    equ 0x8F06
P_ES    equ 0x8F08
R_AX    equ 0x8F0A
R_CF    equ 0x8F0C
P_SI    equ 0x8F0E
SAVE_SP equ 0x8F10
R_BX    equ 0x8F20
R_CX    equ 0x8F22
R_DX    equ 0x8F24
REAL_SP equ 0x9F00

[bits 32]
global thunk_start
thunk_start:
    push ebx
    push esi
    push edi
    push ebp
    mov [SAVE_SP], esp
    cli
    lgdt [gdt_ptr]
    mov esp, REAL_SP
    jmp 0x20:pm16                       ; CS = codigo de 16 bits

[bits 16]
pm16:
    mov ax, 0x28
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE                 ; sin modo protegido
    mov cr0, eax
    jmp 0x0000:real16

real16:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov sp, REAL_SP
    lidt [idt_real]
    mov ax, [P_ES]
    mov es, ax
    mov bx, [P_BX]
    mov cx, [P_CX]
    mov dx, [P_DX]
    mov si, [P_SI]
    mov ax, [P_AX]
    sti
    int 0x13
    cli
    mov [R_AX], ax
    mov [R_BX], bx
    mov [R_CX], cx
    mov [R_DX], dx
    mov byte [R_CF], 0
    jnc .nocarry
    mov byte [R_CF], 1
.nocarry:
    xor ax, ax
    mov ds, ax
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm32b

[bits 32]
pm32b:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, [SAVE_SP]
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF               ; 0x08 codigo 32
    dq 0x00CF92000000FFFF               ; 0x10 datos 32
    dq 0x00CF9A000000FFFF               ; 0x18 codigo 32 (copia)
    dq 0x00009A000000FFFF               ; 0x20 codigo 16
    dq 0x000092000000FFFF               ; 0x28 datos 16
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt
idt_real:
    dw 0x03FF
    dd 0
