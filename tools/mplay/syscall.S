/* syscall.S - entrada de la instruccion SYSCALL para la capa Linux de MPLAY.
 * musl (la libc con la que se compila FFmpeg) hace "syscall" como en Linux. BASIC-OS corre todo en ring 0, asi que SYSCALL
 * salta aqui sin cambiar de privilegio ni de pila: guardamos todo, llamamos a bos_syscall() en C y volvemos con un salto
 * normal (sysret bajaria a ring 3). Los 128 bytes de "zona roja" debajo de la pila del llamador se respetan.
 * Entrada: rax = numero, rdi rsi rdx r10 r8 r9 = argumentos, rcx = vuelta, r11 = banderas. Salida: rax. */
    .text
    .globl bos_syscall_entry
bos_syscall_entry:
    sub $128, %rsp
    push %rcx
    push %r11
    push %rbx
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    mov %rsp, %rbx
    and $-16, %rsp
    sub $8, %rsp
    push %r9                /* a6 en la pila */
    mov %r8, %r9            /* a5 */
    mov %r10, %r8           /* a4 */
    mov %rdx, %rcx          /* a3 */
    mov %rsi, %rdx          /* a2 */
    mov %rdi, %rsi          /* a1 */
    mov %rax, %rdi          /* numero */
    call bos_syscall
    mov %rbx, %rsp
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbx
    pop %r11
    pop %rcx
    add $128, %rsp
    push %r11
    popfq
    jmp *%rcx
    .section .note.GNU-stack,"",@progbits

/* bos_on_stack(fn, top): llama fn() con la pila en top (una pila grande propia: dav1d y FFmpeg usan bastante y la del
 * kernel es chica). Vuelve con la pila de antes. */
    .text
    .globl bos_on_stack
bos_on_stack:
    push %rbp
    push %rbx
    mov %rsp, %rbx
    mov %rsi, %rsp
    and $-16, %rsp
    call *%rdi
    mov %rbx, %rsp
    pop %rbx
    pop %rbp
    ret
    .section .note.GNU-stack,"",@progbits
