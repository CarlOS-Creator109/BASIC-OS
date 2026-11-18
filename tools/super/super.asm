; super.asm - arranque de super-basicos.img: un menu que elige entre PC BASIC de 64 y de 32 bits.
;
; Disco real:   LBA 0         este MBR (tabla de particiones -> la FAT16 compartida)
;               LBA 1..15     etapa 2: menu + gancho de INT 13h
;               B64  (4096)   primeros 4096 sectores de PC BASIC 64 (arranque, kernel, /SYS, SAVE)
;               B32  (8192)   primeros 4096 sectores de PC BASIC 32
;               PART (12288)  particion FAT16 "BASICOS" (DOOM, WAD, video, Linux, NET.BIN...)
;
; Cada sistema ve un disco "virtual" igual al de pc-basic-doom.img: el sector v < 4096 sale de su
; propia zona (B64 o B32) y v >= 4096 de la FAT compartida. Lo hace un gancho de INT 13h que vive en
; el ultimo KB de la memoria convencional (se resta de 0x413, como memdisk). Los dos sistemas leen
; el disco por la BIOS, asi que los dos ven la misma FAT; Linux (TinyCore/BashTerm) usa sus propios
; drivers y la encuentra por la tabla de particiones de este MBR.
; nasm -f bin hook.asm -o hook.bin && nasm -f bin super.asm -o super.bin

[bits 16]
[org 0x7C00]
STAGE   equ 0x1000
NSTAGE  equ 15

mbr:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [drive0], dl
    mov ah, 0x41                    ; hace falta INT 13h extendida (LBA)
    mov bx, 0x55AA
    int 0x13
    jc .noext
    cmp bx, 0xAA55
    jne .noext
    mov si, dap0
    mov ah, 0x42
    mov dl, [drive0]
    int 0x13
    jc .err
    mov dl, [drive0]
    jmp 0:STAGE
.noext:
    mov si, msg_noext
    jmp .die
.err:
    mov si, msg_err
.die:
    lodsb
    test al, al
    jz $
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .die

drive0:    db 0
dap0:      db 16, 0
           dw NSTAGE, STAGE, 0
           dq 1
msg_noext: db "SUPER: la BIOS no tiene INT 13h extendida", 0
msg_err:   db "SUPER: error leyendo el disco", 0

    times 446 - ($ - $$) db 0
    times 64 db 0                   ; tabla de particiones (la llena mksuper.py)
    dw 0xAA55

; =====================================================================
; Etapa 2 (se carga en 0x1000)
; =====================================================================
section stage vstart=STAGE
stage:
    mov [s_drive], dl
    mov ax, 0x0003
    int 0x10
    mov si, msg_title
    call puts
    mov byte [choice], 1
    ; esperar 5 segundos (91 ticks) una tecla
    mov ax, [0x46C]
    mov [t0], ax
.wait:
    mov ah, 0x01
    int 0x16
    jnz .key
    mov ax, [0x46C]
    sub ax, [t0]
    cmp ax, 91
    jb .wait
    jmp .go
.key:
    xor ah, ah
    int 0x16
    cmp al, '1'
    je .k1
    cmp al, '2'
    je .k2
    cmp al, 13
    je .go
    jmp .wait
.k1: mov byte [choice], 1
    jmp .go
.k2: mov byte [choice], 2
.go:
    mov eax, [base64]
    cmp byte [choice], 2
    jne .b
    mov eax, [base32]
.b:
    mov [hookblk + HK_BASE], eax
    mov eax, [partlba]
    mov [hookblk + HK_PART], eax
    mov al, [s_drive]
    mov [hookblk + HK_DRIVE], al
    ; geometria real (para traducir lecturas CHS)
    push es
    mov ah, 0x08
    mov dl, [s_drive]
    xor di, di
    int 0x13
    pop es
    mov al, cl
    and al, 0x3F
    jnz .spt_ok
    mov al, 63
.spt_ok:
    xor ah, ah
    mov [hookblk + HK_SPT], ax
    mov al, dh
    xor ah, ah
    inc ax
    mov [hookblk + HK_HEADS], ax
    ; robar 1 KB de lo alto de la memoria convencional
    mov ax, [0x413]
    dec ax
    mov [0x413], ax
    shl ax, 6                       ; KB -> segmento
    mov es, ax
    xor di, di
    mov si, hookblk
    mov cx, hookend - hookblk
    rep movsb
    mov ebx, [0x13 * 4]
    mov [es:HK_OLD], ebx
    cli
    mov word [0x13 * 4], 0
    mov [0x13 * 4 + 2], es
    sti
    ; leer el sector 0 virtual del sistema elegido y arrancarlo como si fuera el disco
    xor ax, ax
    mov es, ax
    mov si, dapv
    mov ah, 0x42
    mov dl, [s_drive]
    int 0x13
    jc .rerr
    cmp word [0x7DFE], 0xAA55
    jne .rerr
    mov si, msg_boot
    call puts
    mov dl, [s_drive]
    xor ax, ax
    mov ds, ax
    jmp 0:0x7C00
.rerr:
    mov si, msg_err2
    call puts
    jmp $

puts:
    lodsb
    test al, al
    jz .r
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp puts
.r: ret

s_drive: db 0
choice:  db 1
t0:      dw 0
dapv:    db 16, 0
         dw 1, 0x7C00, 0
         dq 0
msg_title:
    db 13, 10, "  SUPER BASIC-OS", 13, 10
    db "  ===============", 13, 10, 13, 10
    db "  1) PC BASIC 64 bits  (DOOM, video, Linux/BashTerm, red)", 13, 10
    db "  2) PC BASIC 32 bits  (para CPUs viejos y v86)", 13, 10, 13, 10
    db "  Los dos ven la misma particion BASICOS (HD0.1:).", 13, 10, 13, 10
    db "  Elige 1 o 2 (en 5 segundos arranca el 1)... ", 0
msg_boot: db 13, 10, "  Arrancando...", 13, 10, 0
msg_err2: db 13, 10, "  SUPER: no pude leer el sistema elegido", 0

    align 4
base64:  dd 4096                    ; mismos numeros que tools/super/mksuper.py
base32:  dd 8192
partlba: dd 12288

; =====================================================================
; Gancho de INT 13h. Se copia a SEG:0, por eso todo es relativo a "hook".
; =====================================================================
    align 16
hookblk:
    incbin "hook.bin"
hookend:
HK_OLD   equ 4
HK_BASE  equ 8
HK_PART  equ 12
HK_SPT   equ 16
HK_HEADS equ 18
HK_DRIVE equ 20
