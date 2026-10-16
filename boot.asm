; boot.asm - Sector de arranque de PC BASIC (512 bytes)
;
; Solo carga el disco: stage2 (4 sectores) en 0x7E00 y el kernel en 0x10000,
; y salta a stage2, que prepara video, memoria y modo protegido.
;
; Disco: LBA 0 = este sector, LBA 1-4 = stage2, LBA 5.. = kernel

[bits 16]
[org 0x7C00]

STAGE2_ADDR     equ 0x7E00
STAGE2_SECTORS  equ 4
KERNEL_SEG      equ 0x1000              ; 0x1000:0000 = 0x10000

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; ---- geometria del disco (sectores por pista, cabezas) ----
    mov ah, 0x08
    mov dl, [boot_drive]
    xor di, di
    int 0x13
    jc .geo_done
    mov al, cl
    and al, 0x3F
    jz .geo_done
    xor ah, ah
    mov [spt], ax
    mov al, dh
    xor ah, ah
    inc ax
    mov [heads], ax
.geo_done:

    ; ---- stage2 -> 0000:7E00 ----
    mov word [lba], 1
    mov word [remaining], STAGE2_SECTORS
    xor ax, ax
    mov es, ax
    mov bx, STAGE2_ADDR
    call read_sectors

    ; ---- kernel -> 1000:0000 ----
    mov word [remaining], KERNEL_SECTORS
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx
    call read_sectors

    mov [0x5FF0], dl                    ; para el kernel: unidad, cabezas y sectores/pista
    mov ax, [heads]                     ; (lo usa SAVE/LOAD para llamar a la BIOS)
    mov [0x5FF2], ax
    mov ax, [spt]
    mov [0x5FF4], ax
    xor ax, ax
    mov es, ax
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDR

; ------------------------------------------------------------
; Lee [remaining] sectores desde [lba] hacia ES:BX
read_sectors:
.next:
    mov ax, [lba]
    xor dx, dx
    div word [spt]                  ; ax = lba / spt, dx = lba % spt
    inc dx
    mov cl, dl                      ; sector (base 1)
    xor dx, dx
    div word [heads]                ; ax = cilindro, dx = cabeza
    mov dh, dl                      ; cabeza
    mov ch, al                      ; cilindro (bits bajos)
    shl ah, 6
    or cl, ah                       ; cilindro (bits 8-9)
    mov dl, [boot_drive]

    mov si, 3                       ; reintentos
.try:
    push cx
    push dx
    mov ax, 0x0201                  ; leer 1 sector
    int 0x13
    pop dx
    pop cx
    jnc .ok
    xor ah, ah                      ; reset del controlador
    int 0x13
    dec si
    jnz .try
    jmp disk_error

.ok:
    add bx, 512
    jnz .nowrap
    mov ax, es
    add ax, 0x1000
    mov es, ax
.nowrap:
    inc word [lba]
    dec word [remaining]
    jnz .next
    ret

print:                                  ; imprime cadena en DS:SI
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

disk_error:
    mov si, msg_err
    call print
.hang:
    hlt
    jmp .hang

boot_drive  db 0
spt         dw 18
heads       dw 2
lba         dw 1
remaining   dw 0

msg_boot    db "PC BASIC: cargando...", 13, 10, 0
msg_err     db "Error de disco", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
