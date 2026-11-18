; hook.asm - gancho de INT 13h de super-basicos.img (ver super.asm). Se copia a SEG:0.
; Las variables que llena la etapa 2 van en offsets fijos al principio (HK_* en super.asm).
[bits 16]
[org 0]
hook:
    jmp near entry                  ; +0
    db 0
h_old:   dd 0                       ; +4  INT 13h original
h_base:  dd 0                       ; +8  zona del sistema elegido (B64 o B32)
h_part:  dd 0                       ; +12 inicio de la FAT compartida
h_spt:   dw 0                       ; +16
h_heads: dw 0                       ; +18
h_drive: db 0                       ; +20
    align 4
entry:
    cmp dl, [cs:h_drive]
    jne .chain
    cmp ah, 0x42
    je ext
    cmp ah, 0x43
    je ext
    cmp ah, 0x02
    je chs
    cmp ah, 0x03
    je chs
.chain:
    jmp far [cs:h_old]

; ---- AH=42h/43h: DS:SI = paquete del que llama ----
ext:
    pushad
    push ds
    push es
    mov [cs:h_op], ah
    mov cx, [si + 2]                ; sectores
    mov eax, [si + 8]               ; LBA virtual (los 32 bits altos se ignoran)
    movzx ebx, word [si + 4]        ; offset
    mov dx, [si + 6]                ; segmento
    push ds
    push si
    call xfer                       ; -> CF y AH, CX = sectores hechos
    pop si
    pop ds
    mov [si + 2], cx
    jmp done

; ---- AH=02h/03h: CHS ----
chs:
    pushad
    push ds
    push es
    mov byte [cs:h_op], 0x42
    cmp ah, 0x03
    jne .r
    mov byte [cs:h_op], 0x43
.r:
    push ax
    ; lba = (cil * cabezas + cabeza) * spt + sector - 1
    movzx eax, ch
    mov bl, cl
    shr bl, 6
    mov ah, bl
    movzx edi, dh
    movzx ebx, word [cs:h_heads]
    mul ebx
    add eax, edi
    movzx ebx, word [cs:h_spt]
    mul ebx
    movzx ebx, cl
    and bl, 0x3F
    dec ebx
    add eax, ebx
    pop cx
    xor ch, ch                      ; CX = sectores (AL)
    mov dx, es
    mov bp, sp
    movzx ebx, word [bp + 2 + 2 + 4*4]      ; BX original guardado por pushad
    call xfer
    jmp done

; ---- salida comun: AH = estado, CF = error. AL de CHS = sectores hechos ----
done:
    mov [cs:h_ret], ah
    mov [cs:h_cnt], cl
    pop es
    pop ds
    popad
    mov ah, [cs:h_ret]
    push bp
    mov bp, sp
    and word [bp + 6], 0xFFFE       ; CF en las banderas guardadas por INT
    test ah, ah
    jz .ok
    or word [bp + 6], 1
.ok:
    pop bp
    iret

; xfer: EAX = LBA virtual, CX = sectores, DX:BX = buffer. Devuelve AH = estado, CF, CX = hechos.
xfer:
    mov [cs:h_v], eax
    mov [cs:h_left], cx
    mov word [cs:h_done], 0
    mov [cs:h_dap + 4], bx
    mov [cs:h_dap + 6], dx
.loop:
    cmp word [cs:h_left], 0
    je .fin
    mov eax, [cs:h_v]
    mov ecx, 127
    cmp eax, 4096
    jae .hi
    mov edx, 4096
    sub edx, eax                    ; no cruzar la frontera 4096 en una sola llamada
    cmp edx, ecx
    jae .lo
    mov ecx, edx
.lo:
    add eax, [cs:h_base]
    jmp .n
.hi:
    sub eax, 4096
    add eax, [cs:h_part]
.n:
    movzx edx, word [cs:h_left]
    cmp edx, ecx
    jae .m
    mov ecx, edx
.m:
    mov [cs:h_dap + 8], eax
    mov dword [cs:h_dap + 12], 0
    mov [cs:h_dap + 2], cx
    mov byte [cs:h_dap], 16
    mov byte [cs:h_dap + 1], 0
    push cx
    push cs
    pop ds
    mov si, h_dap
    mov ah, [cs:h_op]
    mov al, 0
    mov dl, [cs:h_drive]
    pushf
    call far [cs:h_old]
    pop cx
    jc .err
    add [cs:h_done], cx
    sub [cs:h_left], cx
    movzx ecx, cx
    add [cs:h_v], ecx
    shl cx, 5                       ; sectores * 512 / 16 = parrafos
    add [cs:h_dap + 6], cx
    jmp .loop
.err:
    mov cx, [cs:h_done]
    stc
    ret
.fin:
    mov cx, [cs:h_done]
    xor ah, ah
    clc
    ret

    align 4
h_v:     dd 0
h_left:  dw 0
h_done:  dw 0
h_op:    db 0
h_ret:   db 0
h_cnt:   db 0
    align 4
h_dap:   times 16 db 0
