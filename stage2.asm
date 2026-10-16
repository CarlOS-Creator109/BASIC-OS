; stage2.asm - Preparacion en modo real de PC BASIC (cargado en 0x7E00, max 2048 bytes)
;
; 1. Mapa de memoria (INT 15h E820 y AH=88h)
; 2. Copia la fuente VGA 8x16 de la BIOS
; 3. Elige el modo VBE (framebuffer lineal, 16/24/32 bits) mas grande que soporte el
;    monitor segun su EDID. Sin VBE usable cae a texto 80x25.
; 4. A20, GDT plana, modo protegido y salto al kernel en 0x10000
;
; Todo se deja en la estructura "bootinfo" en 0x6000 (ver BI_* abajo).

[bits 16]
[org 0x7E00]

BI          equ 0x6000                  ; bootinfo
FONT        equ 0x6800                  ; fuente 8x16 (4096 bytes)
VBEINFO     equ 0x9000                  ; buffers temporales de VBE
MODEINFO    equ 0x9200
BESTINFO    equ 0x9300
EDID        equ 0x9400

; bootinfo: +00 'PCBI' | +04 ancho | +06 alto | +08 pitch | +0A bpp | +0B grafico(1/0)
;           +0C fb (dd) | +10 rs rp gs gp bs bp | +16 n e820 | +18 KB ext (AH=88h)
;           +20 entradas E820 (24 bytes c/u, hasta 32)

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov di, BI                          ; limpiar 1 KB de bootinfo
    mov cx, 0x200
    rep stosw
    mov dword [BI], 0x49424350          ; 'PCBI'

    ; ---- 1. memoria ----
    mov di, BI + 0x20
    xor ebx, ebx
.e820:
    mov eax, 0xE820
    mov edx, 0x534D4150                 ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip
    add di, 24
    inc word [BI + 0x16]
.e820_skip:
    test ebx, ebx
    jz .e820_done
    cmp word [BI + 0x16], 32
    jb .e820
.e820_done:
    mov ah, 0x88
    int 0x15
    jc .no88
    mov [BI + 0x18], ax
.no88:

    ; ---- 2. fuente 8x16 de la BIOS ----
    mov ax, 0x1130
    mov bh, 0x06
    int 0x10                            ; ES:BP -> fuente
    push ds
    push es
    pop ds
    mov si, bp
    xor ax, ax
    mov es, ax
    mov di, FONT
    mov cx, 2048
    rep movsw
    pop ds

    ; ---- 3. modo de video VBE ----
    ; Se recorren TODOS los modos de la BIOS y se elige el de mayor area (y mas bits)
    ; que quepa en el monitor: la resolucion nativa viene del EDID (INT 10h AX=4F15h).
    ; Sin EDID se limita a 1024x768, que cualquier monitor acepta.
    mov dword [VBEINFO], 0x32454256     ; 'VBE2'
    mov di, VBEINFO
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .text_mode
    mov ax, [VBEINFO + 0x10]            ; lista de modos (segmento:offset)
    mov [lseg], ax
    mov ax, [VBEINFO + 0x0E]
    mov [lptr0], ax

    mov ax, 0x4F15                      ; leer EDID (bloque 0)
    mov bx, 1
    xor cx, cx
    xor dx, dx
    mov di, EDID
    int 0x10
    cmp ax, 0x004F
    jne .noedid
    cmp dword [EDID], 0xFFFFFF00        ; cabecera 00 FF FF FF FF FF FF 00
    jne .noedid
    cmp dword [EDID + 4], 0x00FFFFFF
    jne .noedid
    cmp word [EDID + 54], 0             ; descriptor de temporizacion preferido
    je .noedid
    movzx ax, byte [EDID + 58]          ; ancho = byte56 | (byte58 & F0) << 4
    and ax, 0xF0
    shl ax, 4
    movzx cx, byte [EDID + 56]
    or ax, cx
    mov [nat_w], ax
    movzx ax, byte [EDID + 61]          ; alto = byte59 | (byte61 & F0) << 4
    and ax, 0xF0
    shl ax, 4
    movzx cx, byte [EDID + 59]
    or ax, cx
    mov [nat_h], ax
    cmp word [nat_w], 640
    jb .bad_edid
    cmp word [nat_h], 400
    jae .noedid
.bad_edid:
    mov word [nat_w], 0
    mov word [nat_h], 0
.noedid:
    mov dword [ceiling], 0xFFFFFFFF

.scan:
    mov ax, [lptr0]
    mov [lptr], ax
    mov dword [bestkey], 0
.mode_loop:
    mov fs, [lseg]
    mov bx, [lptr]
    mov cx, [fs:bx]
    add word [lptr], 2
    cmp cx, 0xFFFF
    je .pick
    mov [curmode], cx
    mov ax, 0x4F01
    mov di, MODEINFO
    int 0x10
    cmp ax, 0x004F
    jne .mode_loop
    mov ax, [MODEINFO]
    and ax, 0x0091                      ; soportado + grafico + LFB
    cmp ax, 0x0091
    jne .mode_loop
    cmp byte [MODEINFO + 0x1B], 6       ; color directo
    jne .mode_loop
    mov bl, [MODEINFO + 0x19]           ; bits por pixel
    mov al, 3
    cmp bl, 32
    je .bppok
    mov al, 2
    cmp bl, 24
    je .bppok
    mov al, 1
    cmp bl, 16
    jne .mode_loop
.bppok:
    mov [bppscore], al
    movzx ebx, word [MODEINFO + 0x12]   ; ancho
    movzx edx, word [MODEINFO + 0x14]   ; alto
    cmp bx, 640
    jb .mode_loop
    cmp dx, 400
    jb .mode_loop
    mov cx, [nat_w]
    mov si, [nat_h]
    test cx, cx
    jnz .limit
    mov cx, 1024
    mov si, 768
.limit:
    cmp bx, cx
    ja .mode_loop
    cmp dx, si
    ja .mode_loop
    mov eax, ebx
    mul edx                             ; edx:eax = ancho * alto
    shl eax, 2
    movzx edx, byte [bppscore]
    add eax, edx                        ; clave = area * 4 + bits
    cmp eax, [ceiling]
    jae .mode_loop
    cmp eax, [bestkey]
    jbe .mode_loop
    mov [bestkey], eax
    mov cx, [curmode]
    mov [bestmode], cx
    mov si, MODEINFO
    mov di, BESTINFO
    mov cx, 128
    rep movsw
    jmp .mode_loop

.pick:
    cmp dword [bestkey], 0
    je .text_mode
    mov bx, [bestmode]
    or bx, 0x4000                       ; framebuffer lineal
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    je .set_ok
    mov eax, [bestkey]                  ; la BIOS lo rechazo: probar el siguiente mejor
    mov [ceiling], eax
    jmp .scan
.set_ok:
    mov ax, [BESTINFO + 0x12]
    mov [BI + 0x04], ax
    mov ax, [BESTINFO + 0x14]
    mov [BI + 0x06], ax
    mov ax, [BESTINFO + 0x10]
    mov [BI + 0x08], ax
    mov al, [BESTINFO + 0x19]
    mov [BI + 0x0A], al
    mov byte [BI + 0x0B], 1
    mov eax, [BESTINFO + 0x28]
    mov [BI + 0x0C], eax
    mov al, [BESTINFO + 0x1F]           ; tamano/posicion de R, G, B
    mov [BI + 0x10], al
    mov al, [BESTINFO + 0x20]
    mov [BI + 0x11], al
    mov al, [BESTINFO + 0x21]
    mov [BI + 0x12], al
    mov al, [BESTINFO + 0x22]
    mov [BI + 0x13], al
    mov al, [BESTINFO + 0x23]
    mov [BI + 0x14], al
    mov al, [BESTINFO + 0x24]
    mov [BI + 0x15], al
    jmp .video_done

.text_mode:                             ; sin VBE: texto 80x25, fondos brillantes
    mov ax, 0x0003
    int 0x10
    mov ax, 0x1003
    xor bx, bx
    int 0x10
.video_done:

    ; ---- 4. A20, modo protegido ----
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry

[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, 0x10000
    jmp eax

align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF               ; 0x08 codigo 4GB
    dq 0x00CF92000000FFFF               ; 0x10 datos 4GB
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

lseg     dw 0
lptr     dw 0
lptr0    dw 0
curmode  dw 0
bestmode dw 0
nat_w    dw 0
nat_h    dw 0
bppscore db 0
ceiling  dd 0
bestkey  dd 0

times 2048 - ($ - $$) db 0
