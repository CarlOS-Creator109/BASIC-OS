/* task.c - tareas (las bases de los procesos de BASIC-OS)
 *
 * Multitarea COOPERATIVA: cada tarea tiene su pila y sus registros; el cambio de tarea ocurre cuando alguien
 * espera: kbd_poll() (que llaman todos los bucles que esperan una tecla, el escritorio, iPXE...) llama a
 * task_yield() y la CPU pasa a la siguiente tarea lista. Una tarea que nunca espera ni llama a K->task(TASK_YIELD)
 * se queda con la CPU; el paso siguiente es que el reloj (PIT) lo haga solo cada pocos milisegundos (preemptiva).
 *
 * La tarea 0 es la de siempre (el kernel, BASIC, el escritorio). Las demas se crean con
 * K->task(TASK_NEW, funcion, argumento, "NOMBRE"): la funcion recibe el argumento (convencion de C normal) y al
 * volver la tarea termina. Las pilas (1 MB cada una) salen de un banco de RAM ("TAREAS") que se pide al crear la
 * primera. Como el cambio solo pasa en una llamada a funcion, no hace falta guardar SSE/x87.
 */
#include "kernel.h"
#include "kapi.h"

#define MAXT       32
#define STACK_SZ   0x100000u                /* 1 MB por tarea: 32 tareas = un banco de 32 MB */
#define T_FREE     0
#define T_READY    1
#define T_SLEEP    2
#define T_DEAD     3                         /* termino: su pila se libera desde otra tarea */

struct task {
    unsigned long sp;                        /* pila guardada mientras no corre */
    int state;
    unsigned wake;                           /* T_SLEEP: hasta este ms */
    void (*fn)(void *);
    void *arg;
    unsigned long stack;                     /* base de su pila (0 = la tarea 0) */
    unsigned runs;                           /* veces que recibio la CPU */
    char name[12];
};

static struct task T[MAXT];
static int cur, inited;
static unsigned long stack_bank;
static int in_yield;

/* cambio de pila: guarda los registros que C espera conservados en la pila actual, deja el puntero en *save,
 * carga new_sp y saca de ahi los de la otra tarea */
void task_swap(unsigned long *save, unsigned long new_sp);
#ifdef __x86_64__
__asm__(".text\n.globl task_swap\ntask_swap:\n"
        "push %rbp\npush %rbx\npush %r12\npush %r13\npush %r14\npush %r15\n"
        "mov %rsp, (%rdi)\nmov %rsi, %rsp\n"
        "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbx\npop %rbp\nret\n");
#else
__asm__(".text\n.globl task_swap\ntask_swap:\n"
        "mov 4(%esp), %eax\nmov 8(%esp), %edx\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov %esp, (%eax)\nmov %edx, %esp\n"
        "pop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
#endif

static void set_name(struct task *t, const char *n)
{
    int k = 0;
    if (n) for (; n[k] && k < 11; k++) t->name[k] = n[k];
    t->name[k] = 0;
}

static void task_init(void)
{
    int i;
    for (i = 0; i < MAXT; i++) T[i].state = T_FREE;
    T[0].state = T_READY;
    set_name(&T[0], "KERNEL");
    cur = 0;
    inited = 1;
}

static void task_entry(void);

/* las tareas que terminaron (su pila se puede reusar en cuanto no estemos parados encima de ella) */
static void reap(void)
{
    int i;
    for (i = 1; i < MAXT; i++)
        if (T[i].state == T_DEAD && i != cur)
            T[i].state = T_FREE;
}

void task_yield(void)
{
    int i, n, prev;
    unsigned now;
    if (!inited || in_yield)
        return;
    in_yield = 1;
    now = (unsigned)plat_ms_now();
    for (n = 1; n <= MAXT; n++) {
        i = (cur + n) % MAXT;
        if (T[i].state == T_SLEEP && (int)(now - T[i].wake) >= 0)
            T[i].state = T_READY;
        if (T[i].state == T_READY)
            break;
    }
    if (n > MAXT || i == cur) {
        in_yield = 0;
        reap();
        return;
    }
    prev = cur;
    cur = i;
    T[i].runs++;
    task_swap(&T[prev].sp, T[i].sp);
    /* aqui vuelve la tarea prev cuando le toca otra vez */
    in_yield = 0;
    reap();
}

static void task_entry(void)
{
    struct task *t = &T[cur];
    in_yield = 0;
    t->fn(t->arg);
    t->state = T_DEAD;                       /* ya no vuelve a recibir la CPU */
    for (;;)
        task_yield();
}

static int task_new(void (*fn)(void *), void *arg, const char *name)
{
    int i;
    unsigned long *sp;
    if (!inited) task_init();
    if (!fn) return -1;
    if (!stack_bank) {
        stack_bank = (unsigned long)ram_api(RAM_ALLOC, 0, 1, "TAREAS");
        if (!stack_bank) return -1;
    }
    reap();
    for (i = 1; i < MAXT; i++)
        if (T[i].state == T_FREE) break;
    if (i >= MAXT) return -1;
    T[i].stack = stack_bank + (unsigned long)(i - 1) * STACK_SZ;
    T[i].fn = fn;
    T[i].arg = arg;
    T[i].runs = 0;
    set_name(&T[i], name ? name : "TAREA");
    /* pila inicial: lo que task_swap va a sacar (registros en cero) y luego "ret" a task_entry */
    sp = (unsigned long *)(T[i].stack + STACK_SZ - 64);
    *--sp = 0;                               /* alineacion: al entrar a task_entry la pila queda como tras un call */
    *--sp = (unsigned long)task_entry;
#ifdef __x86_64__
    { int k; for (k = 0; k < 6; k++) *--sp = 0; }
#else
    { int k; for (k = 0; k < 4; k++) *--sp = 0; }
#endif
    T[i].sp = (unsigned long)sp;
    T[i].state = T_READY;
    return i;
}

/* K->task():
 *   TASK_NEW     fn, arg, nombre -> numero de tarea (o -1)
 *   TASK_YIELD   cede la CPU
 *   TASK_SLEEP   arg = milisegundos (desde una tarea que no sea la 0; en la 0 equivale a esperar)
 *   TASK_EXIT    termina la tarea actual (no vuelve; en la tarea 0 no hace nada)
 *   TASK_KILL    arg = numero: la termina (no puede ser la 0 ni la actual)
 *   TASK_KILLRANGE fn = desde, arg = hasta: termina las tareas cuyo codigo esta en ese rango (programa que se cerro)
 *   TASK_SELF    -> numero de la tarea actual
 *   TASK_COUNT   -> tareas vivas */
long task_api(int op, void (*fn)(void *), void *arg, const char *name)
{
    int i, n;
    if (!inited) task_init();
    switch (op) {
    case TASK_NEW:
        return task_new(fn, arg, name);
    case TASK_YIELD:
        task_yield();
        return 0;
    case TASK_SLEEP: {
        unsigned ms = (unsigned)(unsigned long)arg;
        if (cur == 0) {
            unsigned t0 = (unsigned)plat_ms_now();
            while ((unsigned)plat_ms_now() - t0 < ms) task_yield();
            return 0;
        }
        T[cur].wake = (unsigned)plat_ms_now() + ms;
        T[cur].state = T_SLEEP;
        task_yield();
        return 0;
    }
    case TASK_EXIT:
        if (cur == 0) return -1;
        T[cur].state = T_DEAD;
        for (;;) task_yield();
    case TASK_KILL:
        i = (int)(long)arg;
        if (i <= 0 || i >= MAXT || i == cur || T[i].state == T_FREE) return -1;
        T[i].state = T_FREE;
        return 0;
    case TASK_KILLRANGE:
        for (n = 0, i = 1; i < MAXT; i++)
            if (i != cur && T[i].state != T_FREE && (unsigned long)T[i].fn >= (unsigned long)fn &&
                (unsigned long)T[i].fn < (unsigned long)arg) {
                T[i].state = T_FREE;
                n++;
            }
        return n;
    case TASK_SELF:
        return cur;
    case TASK_COUNT:
        for (n = 0, i = 0; i < MAXT; i++) n += T[i].state == T_READY || T[i].state == T_SLEEP;
        return n;
    }
    return -1;
}

/* linea i del comando TAREAS (0 = no hay mas) */
int task_line(int i, char *out)
{
    static const char *const st[] = { "", "LISTA", "DORMIDA", "TERMINO" };
    int k, line = 0;
    char *o = out;
    if (!inited) task_init();
    for (k = 0; k < MAXT; k++) {
        unsigned v;
        char t[12];
        int n = 0;
        const char *p;
        if (T[k].state == T_FREE) continue;
        if (line++ != i) continue;
        v = (unsigned)k;
        do t[n++] = (char)('0' + v % 10); while (v /= 10);
        while (n) *o++ = t[--n];
        *o++ = ' '; *o++ = ' ';
        for (p = T[k].name; *p; p++) *o++ = *p;
        for (n = (int)(p - T[k].name); n < 12; n++) *o++ = ' ';
        for (p = k == cur ? "CORRIENDO" : st[T[k].state]; *p; p++) *o++ = *p;
        *o++ = ' '; *o++ = ' ';
        v = T[k].runs;
        n = 0;
        do t[n++] = (char)('0' + v % 10); while (v /= 10);
        while (n) *o++ = t[--n];
        for (p = " TURNOS"; *p; p++) *o++ = *p;
        *o = 0;
        return 1;
    }
    return 0;
}
