/* say.c - SAY.APP: sintetizador de voz por formantes (estilo Klatt) para BASIC-OS
 *
 * Es un port de FTTS (el TTS de formantes de Carlos, ftts.c) a un modulo de /SYS sin libc:
 *   texto --G2P (reglas del espanol, numeros a palabras)--> X-SAMPA --> items (acento, diptongos, nasalizacion)
 *   --> segmentos (duraciones tipo Klatt, locus, transiciones) --> fuente glotal + polo/cero nasal
 *   + cascada F1..F5 + ruido paralelo --> PCM --> K->stream() (HDA / AC'97).
 *
 * Diferencias con ftts.c:
 *   - El kernel se compila con -mgeneral-regs-only (sin SSE). Este modulo se compila con SSE2 y lo activa
 *     el mismo (CR0.EM=0, CR4.OSFXSR/OSXMMEXCPT) cada vez que va a sintetizar, porque una llamada a la BIOS
 *     (thunk) deja CR4 sin esos bits. Sin libm: sin/cos/exp son polinomios (math mas abajo).
 *   - Solo va el motor de formantes y los idiomas es, ja (romaji) y xs (X-SAMPA directo). Los formantes de
 *     vocales/semivocales/laterales/nasales son los que ftts.c lee de espeak-ng (es-419), ya incrustados
 *     (tabla OV[]), asi que no hace falta ningun archivo de datos.
 *   - No hay .ctt, .fts, .rbsong, banco UTAU ni gTTS: eso es del programa de escritorio.
 *   - Se sintetiza por frases y se acumula en RAM; luego se reproduce de corrido a 22050 Hz mono.
 *
 * Uso desde BASIC:   SAY "hola mundo"        SAY A$        SAY "-v f -r 1.2 buenos dias"
 *                    SAY "@TEXTO.TXT"        SAY            (sin argumento: modo conversacion, una frase por linea)
 * Opciones (antes del texto):  -v m|f|c|o|w|r  voz     -r N  velocidad (0.3..4)     -p HZ  tono base
 *                              -l es|ja|xs  idioma (xs = X-SAMPA)     -x  muestra los fonemas
 * GPL-3.0-or-later.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef short s16;

static const struct kapi *K;

/* ---- memoria alta (como PLAY): segmentos, texto, audio ---- */
#define SEGBASE   0xA000000ul      /* hasta 0xA180000 */
#define TXTBASE   0xA200000ul      /* bytes del archivo (64 KB) */
#define CPBASE    0xA220000ul      /* codepoints expandidos (64K enteros) */
#define FBASE     0xA500000ul      /* float de una frase (hasta 1 M muestras) */
#define AUDBASE   0xA900000ul      /* s16 mono acumulado */
#define AUD_MAX   (6 * 1024 * 1024) /* muestras: ~4.7 min a 22050 Hz */
#define F_MAX     (1024 * 1024)
#define TXT_MAX   65536
#define CP_MAX    65536

/* ================================================================== */
/* utilidades sin libc                                                 */
/* ================================================================== */
static int xlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int xcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
static int xncmp(const char *a, const char *b, int n) { while (n > 0 && *a && *a == *b) { a++; b++; n--; } return n ? (u8)*a - (u8)*b : 0; }
static void xcpy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static void xcat(char *d, const char *s) { while (*d) d++; xcpy(d, s); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

#define PI 3.14159265358979323846
#define FMIN(a, b) ((a) < (b) ? (a) : (b))

/* ---- matematicas ---- */
static double cos_(double x)
{
    long k;
    double y, y2;
    int neg = 0;
    if (x < 0) x = -x;
    if (x > 1e9) x = 0;
    k = (long)(x * (1.0 / (2 * PI)));
    x -= (double)k * (2 * PI);                       /* [0, 2pi) */
    if (x > PI) x = 2 * PI - x;                      /* [0, pi] */
    if (x > PI / 2) { x = PI - x; neg = 1; }         /* [0, pi/2] */
    y2 = x * x;
    y = 1 + y2 * (-1.0 / 2 + y2 * (1.0 / 24 + y2 * (-1.0 / 720 + y2 * (1.0 / 40320 + y2 * (-1.0 / 3628800 + y2 * (1.0 / 479001600))))));
    return neg ? -y : y;
}
static double sin_(double x) { return cos_(x - PI / 2); }
static double exp_(double x)
{
    long k;
    double r, p;
    union { u64 u; double d; } v;
    if (x < -700) return 0;
    if (x > 700) x = 700;
    k = (long)(x * 1.4426950408889634 + (x >= 0 ? 0.5 : -0.5));
    r = x - (double)k * 0.6931471805599453;
    p = 1 + r * (1 + r * (1.0 / 2 + r * (1.0 / 6 + r * (1.0 / 24 + r * (1.0 / 120 + r * (1.0 / 720 + r * (1.0 / 5040 + r * (1.0 / 40320 + r * (1.0 / 362880)))))))));
    v.u = (u64)(k + 1023) << 52;
    return p * v.d;
}
static double sqrt_(double x) { return __builtin_sqrt(x); }

/* ---- activar SSE2 en este nucleo (el kernel no lo hace y la BIOS-thunk lo deshace) ---- */
static inline __attribute__((always_inline)) void sse_on(void)
{
    unsigned long c;                                 /* del tamano del registro de control (32 o 64 bits) */
    unsigned mx = 0x9F80;                            /* mascaras de excepcion + flush-to-zero (sin denormales lentos) */
    __asm__ volatile("mov %%cr0, %0" : "=r"(c));
    c &= ~(1ul << 2);                               /* EM = 0 */
    c &= ~(1ul << 3);                               /* TS = 0 */
    c |= (1ul << 1);                                /* MP = 1 */
    __asm__ volatile("mov %0, %%cr0" : : "r"(c) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(c));
    c |= (1ul << 9) | (1ul << 10);                 /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(c) : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mx));
}

/* ================================================================== */
/* Tabla de fonemas (X-SAMPA)                                          */
/* ================================================================== */
enum { C_VOW, C_GLIDE, C_LIQ, C_NASAL, C_TAP, C_TRILL, C_FRIC, C_STOP, C_AFFR, C_ASP };

typedef struct {
    const char *sym; int cls, voiced;
    float f1, f2, f3;       /* formantes (o locus para consonantes) */
    float dur;              /* ms (cierre para oclusivas/africadas) */
    float ff, bf, af;       /* ruido: frecuencia, ancho de banda, amplitud (burst) */
    float asp;              /* ms de aspiracion (oclusivas) o de friccion (africadas) */
} Ph;

static const Ph PH[] = {
 {"a",C_VOW,1, 730,1090,2440, 90, 0,0,0, 0},
 {"e",C_VOW,1, 500,1850,2500, 90, 0,0,0, 0},
 {"i",C_VOW,1, 280,2250,3000, 90, 0,0,0, 0},
 {"o",C_VOW,1, 500, 930,2400, 90, 0,0,0, 0},
 {"u",C_VOW,1, 310, 870,2250, 90, 0,0,0, 0},
 {"E",C_VOW,1, 550,1770,2490, 90, 0,0,0, 0},
 {"O",C_VOW,1, 590, 880,2540, 90, 0,0,0, 0},
 {"@",C_VOW,1, 500,1500,2500, 70, 0,0,0, 0},
 {"I",C_VOW,1, 400,1990,2550, 80, 0,0,0, 0},
 {"U",C_VOW,1, 440,1020,2240, 80, 0,0,0, 0},
 {"A",C_VOW,1, 710,1100,2540, 90, 0,0,0, 0},
 {"y",C_VOW,1, 280,1750,2150, 90, 0,0,0, 0},
 {"2",C_VOW,1, 420,1600,2200, 90, 0,0,0, 0},
 {"9",C_VOW,1, 550,1450,2300, 90, 0,0,0, 0},
 {"6",C_VOW,1, 600,1300,2500, 80, 0,0,0, 0},
 {"{",C_VOW,1, 660,1720,2410, 90, 0,0,0, 0},
 {"V",C_VOW,1, 640,1190,2390, 85, 0,0,0, 0},
 {"j",C_GLIDE,1, 260,2250,3000, 60, 0,0,0, 0},
 {"w",C_GLIDE,1, 300, 750,2250, 60, 0,0,0, 0},
 {"l",C_LIQ,1,   380,1350,2750, 70, 0,0,0, 0},
 {"L",C_LIQ,1,   300,2100,2800, 80, 0,0,0, 0},
 {"4",C_TAP,1,   350,1400,2200, 30, 0,0,0, 0},
 {"r",C_TRILL,1, 380,1300,2000, 95, 0,0,0, 0},
 {"m",C_NASAL,1, 260,1100,2200, 75, 0,0,0, 0},
 {"n",C_NASAL,1, 260,1700,2600, 70, 0,0,0, 0},
 {"N",C_NASAL,1, 260,2000,2700, 70, 0,0,0, 0},
 {"J",C_NASAL,1, 270,2200,2900, 80, 0,0,0, 0},
 {"f",C_FRIC,0, 400,1200,2300, 90, 4500,3500,0.33f, 0},
 {"v",C_FRIC,1, 400,1200,2300, 70, 4500,3500,0.15f, 0},
 {"T",C_FRIC,0, 400,1400,2500, 90, 5500,3500,0.18f, 0},
 {"D",C_FRIC,1, 300,1600,2600, 55, 5000,3500,0.06f, 0},
 {"s",C_FRIC,0, 350,1800,2700,100, 6300,1800,0.30f, 0},
 {"z",C_FRIC,1, 350,1800,2700, 80, 6300,1800,0.22f, 0},
 {"S",C_FRIC,0, 350,1900,2500,100, 2900,1100,1.80f, 0},
 {"Z",C_FRIC,1, 350,1900,2500, 85, 2900,1100,1.00f, 0},
 {"x",C_FRIC,0, 450,1500,2300, 90, 2600,2200,2.20f, 0},
 {"B",C_FRIC,1, 250, 900,2200, 55, 1500,2500,0.06f, 0},
 {"G",C_FRIC,1, 300,1500,2200, 55, 2000,2000,0.06f, 0},
 {"h",C_ASP, 0, 500,1500,2500, 70, 0,0,0, 0},
 {"p",C_STOP,0, 250, 800,2200, 50, 1200,2000,3.00f, 15},
 {"t",C_STOP,0, 250,1700,2650, 50, 4200,2200,1.00f, 15},
 {"k",C_STOP,0, 300,1500,2300, 50, 2000,1200,4.00f, 25},
 {"b",C_STOP,1, 200, 800,2200, 55, 1200,2000,0.15f,  0},
 {"d",C_STOP,1, 200,1700,2600, 55, 4000,2200,0.20f,  0},
 {"g",C_STOP,1, 250,1400,2200, 55, 2000,1200,0.20f,  0},
 {"tS",C_AFFR,0, 350,1900,2500, 45, 2900,1100,1.80f, 85},
 {"dZ",C_AFFR,1, 350,1900,2500, 45, 2900,1100,1.20f, 70},
 {"ts",C_AFFR,0, 350,1800,2700, 45, 6300,1800,0.30f, 80},
 {"dz",C_AFFR,1, 350,1800,2700, 45, 6300,1800,0.22f, 70},
 {"r\\",C_LIQ,1,  400,1300,1750, 70, 0,0,0, 0},
 {"R",C_FRIC,1,   400,1300,2200, 60, 2000,1800,0.20f, 0},
 {"C",C_FRIC,0,   300,2100,2800, 80, 4000,2200,0.40f, 0},
 {"c",C_STOP,0,   280,2000,2700, 50, 3500,1800,0.45f, 18},
 {"#q",C_STOP,0,  400,1500,2500, 40, 0,0,0.0f, 0},
 {"1",C_VOW,1, 330,1700,2100, 90, 0,0,0, 0},
 {"}",C_VOW,1, 300,1500,2100, 90, 0,0,0, 0},
 {"M",C_VOW,1, 300,1300,2300, 90, 0,0,0, 0},
 {"7",C_VOW,1, 450,1300,2300, 90, 0,0,0, 0},
 {"Q",C_VOW,1, 600, 900,2500, 90, 0,0,0, 0},
 {"Y",C_VOW,1, 330,1650,2100, 80, 0,0,0, 0},
 {"3",C_VOW,1, 580,1380,2400, 90, 0,0,0, 0},
};
#define NPH ((int)(sizeof(PH) / sizeof(PH[0])))

/* formantes F1..F5 que espeak-ng (es-419) da a cada fonema: los mismos que ftts.c lee de phondata.
 * mask: bit0 = F1 ... bit4 = F5 (las nasales solo F2 y F3) */
static const struct { const char *sym; u8 mask; short f[5]; } OV[] = {
    {"@", 0x1f, {525, 1441, 2340, 3260, 3700}}, {"a", 0x1f, {841, 1409, 2550, 3414, 3795}},
    {"e", 0x1f, {504, 1900, 2540, 3360, 3720}}, {"i", 0x1f, {280, 2280, 2840, 3400, 3840}},
    {"o", 0x1f, {544,  900, 2520, 2980, 3725}}, {"u", 0x1f, {320,  820, 2360, 3340, 3900}},
    {"n", 0x06, {480, 1500, 2540, 3500, 4000}}, {"l", 0x1f, {360, 1620, 2900, 3460, 3975}},
    {"j", 0x1f, {285, 2233, 2760, 3360, 3700}}, {"w", 0x1f, {275,  462, 2360, 3100, 4060}},
    {"L", 0x1f, {360, 1620, 2900, 3460, 3975}}, {"m", 0x06, {472, 1100, 2130, 3500, 4000}},
    {"J", 0x06, {470, 2140, 2840, 3500, 4000}}, {"N", 0x06, {490, 2100, 2540, 3500, 4000}},
    {"E", 0x1f, {504, 1900, 2540, 3360, 3720}}, {"O", 0x1f, {584,  940, 2580, 2980, 3637}},
    {"U", 0x1f, {388,  940, 2420, 3040, 3720}}, {"y", 0x1f, {280, 1720, 2340, 3300, 3680}},
    {"Y", 0x1f, {448, 1520, 2420, 3300, 3830}},
};
static float OVF[96][5];
static u8 OVM[96];

static int sym_index(const char *x)
{
    int i;
    for (i = 0; i < NPH; i++) if (!xcmp(PH[i].sym, x)) return i;
    return -1;
}

static void load_overrides(void)
{
    unsigned i, k;
    for (i = 0; i < 96; i++) OVM[i] = 0;
    for (i = 0; i < sizeof(OV) / sizeof(OV[0]); i++) {
        int p = sym_index(OV[i].sym);
        if (p < 0) continue;
        for (k = 0; k < 5; k++) OVF[p][k] = OV[i].f[k];
        OVM[p] = OV[i].mask;
    }
}

/* ================================================================== */
/* Items (secuencia de fonemas con prosodia)                           */
/* ================================================================== */
typedef struct {
    int ph, ph2;       /* fonema; 2o elemento si es diptongo (-1 = no); ph<0 = pausa */
    int stress, nas, wend, q;
    float len, pdur;
} Item;
#define MAXITEMS 3000
static Item it[MAXITEMS];

static int parse_xs(const char *s, Item *t, int max)
{
    int n = 0, stress = 0;
    const char *prev_end = 0;
    while (*s && n < max - 1) {
        char c = *s;
        int i, best = -1, bl = 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-' || c == '_') {
            if (n > 0 && t[n - 1].ph >= 0) t[n - 1].wend = 1;
            s++; continue;
        }
        if (c == '\'') { stress = 1; s++; continue; }
        if (c == ':') { if (n > 0 && t[n - 1].ph >= 0) t[n - 1].len *= 1.7f; s++; continue; }
        if (c == '~') { if (n > 0 && t[n - 1].ph >= 0) t[n - 1].nas = 1; s++; continue; }
        if (c == ',' || c == ';' || c == '.' || c == '?' || c == '!') {
            Item *p = &t[n];
            p->ph = -1; p->ph2 = -1; p->stress = p->nas = p->wend = 0; p->len = 1;
            p->pdur = (float)((c == ',' || c == ';') ? 180 : 360);
            p->q = (c == '?'); n++; s++; continue;
        }
        for (i = 0; i < NPH; i++) {
            int l = xlen(PH[i].sym);
            if (l > bl && xncmp(s, PH[i].sym, l) == 0) { best = i; bl = l; }
        }
        if (best < 0) { s++; continue; }               /* simbolo desconocido: se ignora */
        if (n > 0 && t[n - 1].ph >= 0 && t[n - 1].ph2 < 0 && PH[t[n - 1].ph].cls == C_VOW &&
            PH[best].cls == C_VOW && prev_end == s && !stress &&
            PH[best].sym[1] == 0) {
            char q = PH[best].sym[0];
            if (q == 'I' || q == 'U' || q == '@' || q == 'i' || q == 'u' || q == '6') {
                t[n - 1].ph2 = best; s += bl; prev_end = s; continue;
            }
        }
        {
            Item *p = &t[n];
            p->ph = best; p->ph2 = -1; p->stress = stress; p->nas = p->wend = p->q = 0; p->len = 1; p->pdur = 0;
        }
        n++; s += bl; prev_end = s;
        if (PH[best].cls == C_VOW) stress = 0;
    }
    if (n > 0 && t[n - 1].ph >= 0) t[n - 1].wend = 1;
    return n;
}

/* ================================================================== */
/* G2P espanol (reglas) sobre codepoints Latin-1                       */
/* ================================================================== */
static int lowcp(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c == 0xC1 || c == 0xC9 || c == 0xCD || c == 0xD3 || c == 0xDA || c == 0xDC || c == 0xD1) return c + 0x20;
    return c;
}
static int is_acute(int c) { return c == 0xE1 || c == 0xE9 || c == 0xED || c == 0xF3 || c == 0xFA; }
static int is_vow(int c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || is_acute(c) || c == 0xFC; }
static int is_letter(int c) { return (c >= 'a' && c <= 'z') || is_acute(c) || c == 0xFC || c == 0xF1; }
static int base_of(int c)
{
    switch (c) {
    case 0xE1: return 'a'; case 0xE9: return 'e'; case 0xED: return 'i';
    case 0xF3: return 'o'; case 0xFA: case 0xFC: return 'u'; default: return c;
    }
}
static void app(char *o, int cap, const char *s)
{
    if (xlen(o) + xlen(s) + 1 < cap) xcat(o, s);
}

typedef struct { char ph[4]; int v, acc, weak; } U;

static const char *const FUNCW[] = {"el","la","los","las","un","una","de","del","en","a","y","e","o","u",
    "que","se","me","te","le","lo","con","por","para","su","mi","tu","al",0};

static void es_word(const int *w, int n, char *out, int cap, int *pv)
{
    U u[80];
    int nu = 0, k, i, mex;
    char ws[80];
    int idx[80], nn = 0, st = -1;
    if (n > 70) n = 70;
    if (n < 1) return;
    for (i = 0; i < n; i++) ws[i] = (base_of(w[i]) < 128) ? (char)base_of(w[i]) : '?';
    ws[n] = 0;
    mex = (n >= 5 && !xncmp(ws, "mexic", 5));

    for (k = 0; k < n; k++) {
        int c = w[k], nx = (k + 1 < n) ? w[k + 1] : 0, nx2 = (k + 2 < n) ? w[k + 2] : 0;
        int pr = k ? w[k - 1] : 0;
        U *x = &u[nu];
        x->ph[0] = x->ph[1] = x->ph[2] = x->ph[3] = 0; x->v = x->acc = x->weak = 0;
        if (is_vow(c)) {
            x->v = 1; x->acc = is_acute(c); x->ph[0] = (char)base_of(c);
            x->weak = (x->ph[0] == 'i' || x->ph[0] == 'u'); nu++; continue;
        }
        switch (c) {
        case 'h': continue;
        case 'c':
            if (nx == 'h') { xcpy(x->ph, "tS"); k++; }
            else if (nx == 'e' || nx == 'i' || nx == 0xE9 || nx == 0xED) xcpy(x->ph, "s");
            else xcpy(x->ph, "k");
            break;
        case 'q': xcpy(x->ph, "k"); if (nx == 'u') k++; break;
        case 'g':
            if (nx == 'u' && (nx2 == 'e' || nx2 == 'i' || nx2 == 0xE9 || nx2 == 0xED)) { xcpy(x->ph, "g"); k++; }
            else if (nx == 0xFC) xcpy(x->ph, "g");
            else if (nx == 'e' || nx == 'i' || nx == 0xE9 || nx == 0xED) xcpy(x->ph, "x");
            else xcpy(x->ph, "g");
            break;
        case 'j': xcpy(x->ph, "x"); break;
        case 'k': xcpy(x->ph, "k"); break;
        case 'l': if (nx == 'l') { xcpy(x->ph, "j"); k++; } else xcpy(x->ph, "l"); break;
        case 0xF1: xcpy(x->ph, "J"); break;
        case 'n': xcpy(x->ph, "n"); break;
        case 'm': xcpy(x->ph, "m"); break;
        case 'p': xcpy(x->ph, "p"); break;
        case 'b': case 'v': xcpy(x->ph, "b"); break;
        case 'd': xcpy(x->ph, "d"); break;
        case 'f': xcpy(x->ph, "f"); break;
        case 't': xcpy(x->ph, "t"); break;
        case 's': case 'z': xcpy(x->ph, "s"); break;
        case 'w': xcpy(x->ph, "w"); break;
        case 'r':
            if (nx == 'r') { xcpy(x->ph, "r"); k++; }
            else if (k == 0 || pr == 'n' || pr == 'l' || pr == 's') xcpy(x->ph, "r");
            else xcpy(x->ph, "4");
            break;
        case 'x':
            if (k == 0) xcpy(x->ph, "s");
            else if (mex) xcpy(x->ph, "x");
            else xcpy(x->ph, "ks");
            break;
        case 'y':
            if (n == 1) { x->v = 1; x->ph[0] = 'i'; }
            else if (is_vow(nx)) xcpy(x->ph, "j");
            else if (k > 0 && is_vow(pr)) xcpy(x->ph, "j");
            else { x->v = 1; x->ph[0] = 'i'; }
            break;
        default: continue;
        }
        nu++;
    }

    /* diptongos: i/u atonas junto a otra vocal pasan a semivocal */
    for (k = 0; k < nu; k++) {
        int nextv, prevv;
        if (!u[k].v || !u[k].weak || u[k].acc) continue;
        nextv = (k + 1 < nu) && u[k + 1].v;
        prevv = (k > 0) && u[k - 1].v;
        if ((nextv && !prevv) || (prevv && !nextv)) {
            u[k].ph[0] = (u[k].ph[0] == 'i') ? 'j' : 'w'; u[k].ph[1] = 0; u[k].v = 0;
        }
    }

    /* acento */
    for (k = 0; k < nu; k++) if (u[k].v) idx[nn++] = k;
    for (i = 0; i < nn; i++) if (u[idx[i]].acc) st = idx[i];
    if (st < 0 && nn > 0) {
        if (nn == 1) st = idx[0];
        else {
            char l = ws[n - 1];
            if (l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u' || l == 'n' || l == 's') st = idx[nn - 2]; else st = idx[nn - 1];
        }
    }
    for (i = 0; FUNCW[i]; i++) if (!xcmp(ws, FUNCW[i])) st = -1;

    /* emision con alofonos */
    for (k = 0; k < nu; k++) {
        U *x = &u[k];
        const char *ph = x->ph;
        if (x->v && k == st) app(out, cap, "'");
        if (!xcmp(ph, "b") && *pv) ph = "B";
        else if (!xcmp(ph, "d") && *pv) ph = "D";
        else if (!xcmp(ph, "g") && *pv) ph = "G";
        else if (!xcmp(ph, "n") && k + 1 < nu &&
                 (u[k + 1].ph[0] == 'k' || u[k + 1].ph[0] == 'g' || u[k + 1].ph[0] == 'x')) ph = "N";
        app(out, cap, ph);
        *pv = x->v || !xcmp(ph, "j") || !xcmp(ph, "w");
    }
    app(out, cap, " ");
}

static void g2p_es(const int *in, int len, char *out, int cap)
{
    int w[128], n = 0, pv = 0, p = 0;
    out[0] = 0;
    for (;;) {
        int c = (p < len) ? lowcp(in[p++]) : 0;
        if (c && is_letter(c)) { if (n < 120) w[n++] = c; continue; }
        if (n) { es_word(w, n, out, cap, &pv); n = 0; }
        if (!c) break;
        if (c == ',' || c == ';' || c == ':') { app(out, cap, ", "); pv = 0; }
        else if (c == '.') { app(out, cap, ". "); pv = 0; }
        else if (c == '?') { app(out, cap, "? "); pv = 0; }
        else if (c == '!') { app(out, cap, "! "); pv = 0; }
    }
}

static void g2p_id(const int *in, int len, char *out, int cap)
{
    int i, o = 0;
    for (i = 0; i < len && o < cap - 1; i++) out[o++] = (in[i] < 128) ? (char)in[i] : ' ';
    out[o] = 0;
}

/* Japones en romaji (Hepburn simple) -> X-SAMPA. ou/ei/aa/ii/uu/ee/oo = vocal larga. "wo" = o. */
static int has_ch(const char *s, char c) { while (*s) if (*s++ == c) return 1; return 0; }
static void g2p_ja(const int *in, int len, char *out, int cap)
{
    char t[1024];
    int L = 0, i;
    for (i = 0; i < len && L < (int)sizeof t - 2; i++) {
        int c = in[i];
        t[L++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (c < 128 ? (char)c : ' ');
    }
    t[L] = 0; out[0] = 0;
    for (i = 0; i < L; ) {
        char c = t[i], n1 = i + 1 < L ? t[i + 1] : 0;
        if (has_ch("aiueo", c)) {
            char o[3];
            o[0] = c == 'u' ? 'M' : c; o[1] = 0; o[2] = 0;
            app(out, cap, o); i++;
            if (n1 == c || (c == 'o' && n1 == 'u') || (c == 'e' && n1 == 'i')) { app(out, cap, ":"); i++; }
            continue;
        }
        if (c == 'w' && n1 == 'o') { app(out, cap, "o"); i += 2; if (t[i] == 'o' || t[i] == 'u') { app(out, cap, ":"); i++; } continue; }
        if (c == 's' && n1 == 'h') { app(out, cap, "S"); i += 2; continue; }
        if (c == 'c' && n1 == 'h') { app(out, cap, "tS"); i += 2; continue; }
        if (c == 't' && n1 == 's') { app(out, cap, "ts"); i += 2; continue; }
        if (c == 'j') { app(out, cap, "dZ"); i++; continue; }
        if (c == 'y') { app(out, cap, "j"); i++; continue; }
        if (c == 'r' || c == 'l') { app(out, cap, "4"); i++; continue; }
        if (c == 'v') { app(out, cap, "b"); i++; continue; }
        if (c == 'x') { app(out, cap, "ks"); i++; continue; }
        if (c == n1 && !has_ch("aiueon", c)) { i++; continue; }
        if (has_ch("kgsztdnhbpmfw", c)) { char o[2]; o[0] = c; o[1] = 0; app(out, cap, o); i++; continue; }
        if (c == ',' || c == '.' || c == '?' || c == '!') { char o[2]; o[0] = c; o[1] = 0; app(out, cap, o); i++; continue; }
        if (c == ' ' || c == '-') { app(out, cap, " "); i++; continue; }
        i++;
    }
}

/* ================================================================== */
/* Voz, parametros y segmentos                                         */
/* ================================================================== */
static int SR = 22050;

typedef struct {
    double f0, fs, fs45, range, oq, tilt, breath, jitter, shimmer;
    int whisper;
} Voice;
static Voice V;

static void voice_preset(char c)
{
    V.f0 = 115; V.fs = 1.0; V.fs45 = 1.0; V.range = 1.0; V.oq = 0.60; V.tilt = 0.10; V.breath = 0.02;
    V.jitter = 0.004; V.shimmer = 0.025; V.whisper = 0;
    switch (c) {
    case 'f': V.f0 = 200; V.fs = 1.17; V.fs45 = 1.10; V.oq = 0.65; V.tilt = 0.10; V.breath = 0.03; break;
    case 'c': V.f0 = 270; V.fs = 1.35; V.fs45 = 1.20; V.oq = 0.70; V.tilt = 0.05; V.breath = 0.02; break;
    case 'o': V.f0 = 100; V.fs = 0.98; V.oq = 0.55; V.breath = 0.06; V.jitter = 0.012; V.shimmer = 0.06; V.range = 0.8; break;
    case 'w': V.whisper = 1; V.breath = 0; V.jitter = 0; V.shimmer = 0; break;
    case 'r': V.range = 0; V.jitter = 0; V.shimmer = 0; V.breath = 0; V.oq = 0.5; V.tilt = 0.0; break;
    default: break;   /* 'm' */
    }
}

/* Parametros por instante. Grupos de transicion: 0 = formantes/filtros, 1 = amplitudes, 2 = tono. */
typedef struct {
    float f0m;
    float F1, F2, F3, F4, F5, B1, B2, B3, Ff, Bf, Ff2, nas, Fnz;
    float Av, Ah, Af, Af2, trill, am;
} Par;
#define NPAR 20
static const u8 GRP[NPAR] = { 2, 0,0,0,0,0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1 };

typedef struct { double t0, t1; Par p; float tg[3]; int phr; } Seg;
#define SEGMAX 8192
static Seg *sg;
static int nsg;
static Par lastp;
static double T;
static int curphr, nphr;
#define MAXPHR 256
static double ph_t0[MAXPHR], ph_t1[MAXPHR];
static int ph_q[MAXPHR];

static Par defpar(void)
{
    Par d;
    u8 *b = (u8 *)&d;
    unsigned i;
    for (i = 0; i < sizeof d; i++) b[i] = 0;
    d.f0m = 1; d.F1 = (float)(500 * V.fs); d.F2 = (float)(1500 * V.fs); d.F3 = (float)(2500 * V.fs);
    d.F4 = (float)(3400 * V.fs45); d.F5 = (float)(4500 * V.fs45);
    d.B1 = 70; d.B2 = 100; d.B3 = 160; d.Ff = 4000; d.Bf = 2000; d.Fnz = 1000;
    return d;
}

static double g_rate = 1.0;

static void addseg(const Par *p, double ms, double tf, double ta, double tp, double rate)
{
    double d = ms / 1000.0 / rate, k;
    Seg *s;
    if (d < 1e-4) d = 1e-4;
    if (nsg >= SEGMAX) return;
    s = &sg[nsg++];
    k = 1.0 / sqrt_(rate);
    s->t0 = T; s->t1 = T + d; s->p = *p;
    s->tg[0] = (float)(tf * k / 1000); s->tg[1] = (float)(ta * k / 1000); s->tg[2] = (float)(tp * k / 1000);
    s->phr = curphr < MAXPHR ? curphr : MAXPHR - 1;
    T += d; lastp = *p;
}

static void setform(Par *p, int ph)
{
    const Ph *P = &PH[ph];
    float f[5];
    int k;
    f[0] = P->f1; f[1] = P->f2; f[2] = P->f3; f[3] = 3400; f[4] = 4500;
    for (k = 0; k < 5; k++) if (OVM[ph] & (1 << k)) f[k] = OVF[ph][k];
    p->F1 = (float)(f[0] * V.fs); p->F2 = (float)(f[1] * V.fs); p->F3 = (float)(f[2] * V.fs);
    p->F4 = (float)(f[3] * V.fs45); p->F5 = (float)(f[4] * V.fs45);
}

static void apply_locus(Par *p, int ph, const Item *t, int i, int n)
{
    const Ph *C = &PH[ph];
    int k;
    for (k = i + 1; k < n && k <= i + 4; k++) {
        int cl;
        if (t[k].ph < 0) return;
        cl = PH[t[k].ph].cls;
        if (cl == C_VOW || cl == C_GLIDE || cl == C_LIQ) {
            Par v = *p;
            double L, kk;
            setform(&v, t[k].ph);
            L = C->f2 * V.fs;
            kk = (C->f2 < 1000) ? 0.75 : (C->f2 < 1650) ? 0.70 : 0.30;
            p->F2 = (float)(L + kk * (v.F2 - L));
            p->F3 = (float)(p->F3 + 0.25 * (v.F3 - p->F3));
            return;
        }
    }
}
static int next_vowel(const Item *t, int i, int n)
{
    int k;
    for (k = i + 1; k < n && k <= i + 4; k++) {
        if (t[k].ph < 0) return -1;
        if (PH[t[k].ph].cls == C_VOW) return t[k].ph;
    }
    return -1;
}
static int is_cons(int ph)
{
    int c = PH[ph].cls;
    return c == C_FRIC || c == C_STOP || c == C_AFFR || c == C_NASAL || c == C_ASP;
}
static int is_voiceless(int ph)
{
    int c = PH[ph].cls;
    return (c == C_FRIC || c == C_STOP || c == C_AFFR || c == C_ASP) && !PH[ph].voiced;
}
static double klatt(double inh, double minfrac, double pct)
{
    double mn = inh * minfrac;
    return mn + (inh - mn) * pct;
}
static void set_noise2(Par *p, double ff, double bf)
{
    p->Ff = (float)ff; p->Bf = (float)bf;
    if (ff >= 5000) { p->Ff2 = (float)(ff * 1.3); p->Af2 = 0.5f; }
    else if (ff >= 2500) { p->Ff2 = (float)(ff * 1.5); p->Af2 = 0.4f; }
    else { p->Ff2 = 0; p->Af2 = 0; }
}

static u8 lastv[MAXITEMS + 1];

static void build(Item *t, int n, double rate)
{
    Par d, e;
    int i, seen, open = 0;
    nsg = 0; T = 0; curphr = 0; nphr = 0;
    d = defpar(); lastp = d;
    {
        Par s = d;
        addseg(&s, 60, 0, 0, 0, 1.0);
    }
    for (i = 0; i <= n; i++) lastv[i] = 0;
    for (i = n - 1, seen = 0; i >= 0; i--) {
        if (t[i].ph < 0) seen = 0;
        else if (PH[t[i].ph].cls == C_VOW) { if (!seen) lastv[i] = 1; seen = 1; }
    }
    for (i = 0; i < n; i++) {
        const Ph *P;
        Par p;
        int nxt, st;
        if (t[i].ph < 0) {
            if (open) { ph_t1[curphr] = T; ph_q[curphr] = t[i].q; open = 0; if (curphr < MAXPHR - 1) curphr++; }
            p = lastp; p.Av = p.Ah = p.Af = p.Af2 = p.trill = p.am = 0; p.nas = 0; p.f0m = 1;
            addseg(&p, t[i].pdur, 20, 15, 80, rate);
            continue;
        }
        if (!open) { ph_t0[curphr] = T; ph_q[curphr] = 0; open = 1; }
        P = &PH[t[i].ph];
        p = d; setform(&p, t[i].ph);
        p.f0m = lastp.f0m;
        nxt = (i + 1 < n && t[i + 1].ph >= 0) ? t[i + 1].ph : -1;
        st = t[i].stress;
        switch (P->cls) {
        case C_VOW: {
            double pct = st ? 1.15 : 0.70, dur;
            if (nxt >= 0 && is_voiceless(nxt) && !t[i].wend) pct *= 0.88;
            if (lastv[i]) pct *= 1.5;
            if (t[i].ph2 >= 0) pct *= 1.45;
            dur = klatt(P->dur, 0.5, pct) * t[i].len;
            p.Av = st ? 1.0f : 0.90f; p.Ah = 0.01f;
            if (t[i].nas) { p.nas = 0.6f; p.Fnz = 1000; }
            if (t[i].ph2 >= 0) {
                Par a = p, b;
                a.f0m = (float)(st ? 1.17 : 1.0);
                addseg(&a, dur * 0.45, 50, 18, 60, rate);
                b = p; setform(&b, t[i].ph2);
                b.F1 = a.F1 + 0.85f * (b.F1 - a.F1); b.F2 = a.F2 + 0.85f * (b.F2 - a.F2);
                b.F3 = a.F3 + 0.85f * (b.F3 - a.F3); b.F4 = a.F4 + 0.85f * (b.F4 - a.F4);
                b.F5 = a.F5 + 0.85f * (b.F5 - a.F5);
                b.f0m = (float)(st ? 1.05 : 0.98); b.Av = p.Av * 0.85f;
                addseg(&b, dur * 0.55, dur * 0.50, 25, 60, rate);
            } else if (st) {
                Par a = p, b;
                a.f0m = 1.17f;
                addseg(&a, dur * 0.40, 50, 18, 60, rate);
                b = p; b.f0m = 1.05f;
                addseg(&b, dur * 0.60, 0, 18, 60, rate);
            } else {
                addseg(&p, dur, 50, 18, 60, rate);
            }
            break; }
        case C_GLIDE: p.Av = 0.55f; addseg(&p, klatt(P->dur, 0.6, 1.0) * t[i].len, 40, 15, 60, rate); break;
        case C_LIQ: p.Av = 0.65f; addseg(&p, klatt(P->dur, 0.6, nxt >= 0 && is_cons(nxt) ? 0.8 : 1.0) * t[i].len, 40, 15, 60, rate); break;
        case C_NASAL: {
            double pct = (nxt >= 0 && is_cons(nxt) && !t[i].wend) ? 0.8 : 1.0;
            p.F1 = (float)(260 * V.fs); p.Av = 0.85f; p.B1 = 330; p.B2 = 260; p.B3 = 300;
            p.nas = 1.0f;
            p.Fnz = (float)((PH[t[i].ph].f2 < 1300) ? 1000 : (PH[t[i].ph].f2 < 1900) ? 1600 : (PH[t[i].ph].f2 < 2150) ? 2500 : 2200);
            apply_locus(&p, t[i].ph, t, i, n);
            addseg(&p, klatt(P->dur, 0.6, pct) * t[i].len, 35, 12, 60, rate); break; }
        case C_TAP: p.Av = 0.4f; addseg(&p, P->dur * t[i].len, 25, 8, 60, rate); break;
        case C_TRILL: p.Av = 0.8f; p.trill = 1.0f; addseg(&p, P->dur * t[i].len, 40, 12, 60, rate); break;
        case C_FRIC: {
            double pct = (nxt >= 0 && is_cons(nxt) && !t[i].wend) ? 0.75 : 1.0;
            p.Av = P->voiced ? 0.35f : 0; p.Af = P->af; p.am = P->voiced ? 0.5f : 0;
            set_noise2(&p, P->ff, P->bf);
            apply_locus(&p, t[i].ph, t, i, n);
            addseg(&p, klatt(P->dur, 0.6, pct) * t[i].len, 25, 10, 70, rate); break; }
        case C_ASP: {
            int nv = next_vowel(t, i, n);
            if (nv >= 0) setform(&p, nv);
            p.Av = 0; p.Ah = 0.45f;
            addseg(&p, P->dur * t[i].len, 25, 12, 70, rate); break; }
        case C_STOP: {
            Par c, b1, b2;
            apply_locus(&p, t[i].ph, t, i, n);
            c = p; c.Av = P->voiced ? 0.22f : 0;
            addseg(&c, P->dur * t[i].len, 30, 6, 70, rate);
            b1 = p; b1.Af = P->af; b1.Ah = 0.12f; b1.Av = P->voiced ? 0.3f : 0; set_noise2(&b1, P->ff, P->bf);
            addseg(&b1, 3, 0, 0.5, 70, rate);
            b2 = b1; b2.Af = P->af * 0.4f;
            addseg(&b2, P->voiced ? 5 : 7, 0, 2, 70, rate);
            if (P->asp > 0) { Par a = p; a.Ah = 0.35f; a.Av = 0; addseg(&a, P->asp, 15, 5, 70, rate); }
            break; }
        case C_AFFR: {
            Par c, f;
            apply_locus(&p, t[i].ph, t, i, n);
            c = p; c.Av = P->voiced ? 0.22f : 0;
            addseg(&c, P->dur * t[i].len, 30, 6, 70, rate);
            f = p; f.Af = P->af; f.Av = P->voiced ? 0.35f : 0; f.am = P->voiced ? 0.5f : 0; set_noise2(&f, P->ff, P->bf);
            addseg(&f, P->asp, 0, 3, 70, rate);
            break; }
        }
    }
    if (open) { ph_t1[curphr] = T; if (curphr < MAXPHR - 1) curphr++; }
    nphr = curphr;
    e = lastp; e.Av = e.Ah = e.Af = e.Af2 = e.trill = e.am = 0; e.nas = 0;
    addseg(&e, 90, 0, 20, 0, 1.0);
}

static double phrase_curve(int phr, double t)
{
    double a, b, u, c;
    if (phr >= nphr) return 1.0;
    a = ph_t0[phr]; b = ph_t1[phr];
    if (b <= a) return 1.0;
    u = (t - a) / (b - a); if (u < 0) u = 0; if (u > 1) u = 1;
    c = 1.08 - 0.22 * u;                                   /* declinacion */
    if (ph_q[phr]) { if (u > 0.7) { double w = (u - 0.7) / 0.3; c *= 1 + 0.55 * w * w; } }
    else if (u > 0.88) c *= 1 - 0.12 * (u - 0.88) / 0.12; /* caida final */
    return c;
}

/* ================================================================== */
/* Sintesis                                                            */
/* ================================================================== */
typedef struct { double a, b, c, y1, y2; } Res;
typedef struct { double A, B, C, x1, x2; } Anti;
static void res_set(Res *r, double f, double bw)
{
    double Ts = 1.0 / SR;
    if (f > SR * 0.45) f = SR * 0.45;
    if (f < 20) f = 20;
    r->c = -exp_(-2 * PI * bw * Ts);
    r->b = 2 * exp_(-PI * bw * Ts) * cos_(2 * PI * f * Ts);
    r->a = 1 - r->b - r->c;
}
static inline double res_run(Res *r, double x)
{
    double y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1; r->y1 = y; return y;
}
static void anti_set(Anti *z, double f, double bw)
{
    Res r;
    res_set(&r, f, bw);
    z->A = 1.0 / r.a; z->B = -r.b / r.a; z->C = -r.c / r.a;
}
static inline double anti_run(Anti *z, double x)
{
    double y = z->A * x + z->B * z->x1 + z->C * z->x2;
    z->x2 = z->x1; z->x1 = x; return y;
}
static double glot(double t, double oq)                  /* pulso glotal tipo Rosenberg, OQ variable */
{
    double Tp = 0.62 * oq, Tn = 0.28 * oq;
    if (t < Tp) return 0.5 * (1 - cos_(PI * t / Tp));
    if (t < Tp + Tn) return cos_(PI / 2 * (t - Tp) / Tn);
    return 0;
}
static u32 rng = 12345;
static double noise(void)
{
    double a, b;
    rng = rng * 1664525u + 1013904223u; a = ((rng >> 8) / 8388608.0) - 1.0;
    rng = rng * 1664525u + 1013904223u; b = ((rng >> 8) / 8388608.0) - 1.0;
    return (a + b) * 0.5;
}
static double ease(double u) { return u * u * (3 - 2 * u); }

static void eval(double t, int *si, Par *o, int *phr)
{
    const Seg *s;
    const float *cv, *pv;
    float *q = (float *)o;
    double dseg;
    int i;
    while (*si + 1 < nsg && sg[*si + 1].t0 <= t) (*si)++;
    s = &sg[*si];
    cv = (const float *)&s->p;
    pv = (*si > 0) ? (const float *)&sg[*si - 1].p : cv;
    dseg = s->t1 - s->t0;
    for (i = 0; i < NPAR; i++) {
        double tg = s->tg[GRP[i]], u;
        if (tg > 0.9 * dseg) tg = 0.9 * dseg;
        u = (tg <= 1e-6) ? 1.0 : (t - s->t0) / tg;
        if (u > 1) u = 1;
        if (u < 0) u = 0;
        q[i] = (float)(pv[i] + (cv[i] - pv[i]) * ease(u));
    }
    *phr = s->phr;
}

#define FRIC_GAIN  1.0
#define CASC_GAIN  4.8
#define TRHZ       28.0

/* devuelve el numero de muestras escritas en o[] (float) */
static long synth(float *o, long maxn)
{
    long total = (long)(sg[nsg - 1].t1 * SR) + 1, n, m;
    Res r1 = {0, 0, 0, 0, 0}, r2 = {0, 0, 0, 0, 0}, r3 = {0, 0, 0, 0, 0}, r4 = {0, 0, 0, 0, 0}, r5 = {0, 0, 0, 0, 0},
        rf1 = {0, 0, 0, 0, 0}, rf2 = {0, 0, 0, 0, 0}, rnp = {0, 0, 0, 0, 0};
    Anti rz = {0, 0, 0, 0, 0};
    double phase = 0, gprev = 0, nzp = 0, dcx = 0, dcy = 0, tiltz = 0, jit = 1, shim = 1;
    int si = 0;
    if (total > maxn) total = maxn;
    for (n = 0; n < total; n += 64) {
        double t = n / (double)SR, f0;
        Par p;
        int phr;
        eval(t, &si, &p, &phr);
        f0 = V.f0 * p.f0m * (1 + V.range * (phrase_curve(phr, t) - 1)) * (1 + 0.004 * sin_(2 * PI * 5.0 * t));
        res_set(&r1, p.F1, p.B1); res_set(&r2, p.F2, p.B2); res_set(&r3, p.F3, p.B3);
        res_set(&r4, p.F4, 200); res_set(&r5, p.F5, 300);
        res_set(&rf1, p.Ff, p.Bf); res_set(&rf2, p.Ff2 > 0 ? p.Ff2 : p.Ff, p.Bf * 1.2);
        res_set(&rnp, 270, 100); anti_set(&rz, p.Fnz > 200 ? p.Fnz : 200, 120);
        for (m = 0; m < 64 && n + m < total; m++) {
            double ts = (n + m) / (double)SR;
            double inc = f0 * jit / SR, g, voice, nz, gate, mod, src, zp, xn, za, s0, s, nz2, nd, am, fr, y, dy;
            phase += inc;
            if (phase >= 1) { phase -= 1; jit = 1 + V.jitter * noise() * 1.7; shim = 1 + V.shimmer * noise() * 1.7; }
            g = glot(phase, V.oq);
            voice = (g - gprev) / inc * 0.1 * shim; gprev = g;
            tiltz = (1 - V.tilt) * voice + V.tilt * tiltz; voice = tiltz;
            nz = noise();
            gate = 0.3 + 0.7 * g;
            if (V.whisper) voice = nz * 0.6;
            mod = 1.0;
            if (p.trill > 0) mod = 1 - p.trill * (0.5 - 0.5 * cos_(2 * PI * TRHZ * ts));
            src = p.Av * voice * mod
                + p.Av * V.breath * nz * gate
                + p.Ah * 0.5 * nz * (p.Av > 0.1 ? gate : 1.0);
            /* nasal: polo ~270 Hz + antirresonancia */
            zp = res_run(&rnp, src);
            xn = src + 0.12 * p.nas * zp;
            za = anti_run(&rz, xn);
            s0 = (1 - 0.65 * p.nas) * xn + 0.65 * p.nas * za;
            s = res_run(&r1, s0);
            s = res_run(&r2, s); s = res_run(&r3, s); s = res_run(&r4, s); s = res_run(&r5, s);
            /* friccion / burst (rama paralela, AM al tono en las sonoras) */
            nz2 = noise(); nd = nz2 - nzp; nzp = nz2;
            am = 1 - p.am + p.am * g;
            fr = (res_run(&rf1, nd) + p.Af2 * res_run(&rf2, nd)) * p.Af * FRIC_GAIN * 2.0 * am;
            y = s * CASC_GAIN + fr;
            dy = y - dcx + 0.995 * dcy; dcx = y; dcy = dy;   /* bloqueo de DC */
            o[n + m] = (float)dy;
        }
    }
    return total;
}

/* ================================================================== */
/* Texto: bytes (UTF-8 o CP437 de la consola) -> codepoints Latin-1,   */
/* numeros a palabras, simbolos                                        */
/* ================================================================== */
static const u8 CP437[38] = {    /* 0x80..0xA5 */
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, ' ', ' ', ' ', ' ', ' ',
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1
};

/* letras acentuadas que el espanol no usa: se dejan en su vocal base */
static int fold(int c)
{
    if (c >= 0xC0 && c <= 0xC5) return 'a';
    if (c == 0xC7) return 's';
    if (c >= 0xC8 && c <= 0xCB) return c == 0xC9 ? c : 'e';
    if (c >= 0xCC && c <= 0xCF) return c == 0xCD ? c : 'i';
    if (c >= 0xD2 && c <= 0xD6) return c == 0xD3 ? c : 'o';
    if (c >= 0xD9 && c <= 0xDB) return c == 0xDA ? c : 'u';
    if (c == 0xE0 || c == 0xE2 || c == 0xE3 || c == 0xE4 || c == 0xE5) return 'a';
    if (c == 0xE7) return 's';
    if (c == 0xE8 || c == 0xEA || c == 0xEB) return 'e';
    if (c == 0xEC || c == 0xEE || c == 0xEF) return 'i';
    if (c == 0xF2 || c == 0xF4 || c == 0xF5 || c == 0xF6) return 'o';
    if (c == 0xF9 || c == 0xFB) return 'u';
    if (c == 0xFF) return 'i';
    if (c == 0xBF || c == 0xA1) return ' ';
    return c;
}

static int dec_next(const u8 **ps, const u8 *end)
{
    const u8 *p = *ps;
    int c = *p;
    if (c < 0x80) { *ps = p + 1; return c; }
    if ((c & 0xE0) == 0xC0 && p + 1 < end && (p[1] & 0xC0) == 0x80) {
        *ps = p + 2; return fold(((c & 31) << 6) | (p[1] & 63));
    }
    if ((c & 0xF0) == 0xE0 && p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *ps = p + 3; return ' ';
    }
    if ((c & 0xF8) == 0xF0 && p + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *ps = p + 4; return ' ';
    }
    *ps = p + 1;
    if (c >= 0x80 && c <= 0xA5) return fold(CP437[c - 0x80]);
    if (c == 0xA8) return ' ';
    return ' ';
}

static const char *const NU[30] = { "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve", "diez",
    "once", "doce", "trece", "catorce", "quince", "diecis" "\xE9" "is", "diecisiete", "dieciocho", "diecinueve", "veinte",
    "veintiuno", "veintid" "\xF3" "s", "veintitr" "\xE9" "s", "veinticuatro", "veinticinco", "veintis" "\xE9" "is",
    "veintisiete", "veintiocho", "veintinueve" };
static const char *const NT[10] = { "", "", "", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta", "ochenta", "noventa" };
static const char *const NH[10] = { "", "ciento", "doscientos", "trescientos", "cuatrocientos", "quinientos", "seiscientos",
    "setecientos", "ochocientos", "novecientos" };

static void n999(int n, int apo, char *o)
{
    if (n >= 100) {
        if (n == 100) xcat(o, "cien"); else xcat(o, NH[n / 100]);
        n %= 100;
        if (n) xcat(o, " ");
    }
    if (n == 0 && o[0]) return;
    if (n < 30) {
        if (n == 1 && apo) xcat(o, "un");
        else if (n == 21 && apo) xcat(o, "veinti" "\xFA" "n");
        else if (n || !o[0]) xcat(o, NU[n]);
    } else {
        xcat(o, NT[n / 10]);
        if (n % 10) {
            xcat(o, " y ");
            if (n % 10 == 1 && apo) xcat(o, "un"); else xcat(o, NU[n % 10]);
        }
    }
}
static void lt_million(unsigned n, int apo, char *o)
{
    unsigned th = n / 1000, r = n % 1000;
    if (th) {
        if (th == 1) xcat(o, "mil");
        else { n999((int)th, 1, o); xcat(o, " mil"); }
        if (r) xcat(o, " ");
    }
    if (r) n999((int)r, apo, o);
}
static void num_words(u64 n, char *o)
{
    unsigned m;
    o[0] = 0;
    if (n == 0) { xcpy(o, "cero"); return; }
    m = (unsigned)(n / 1000000ull);
    if (m) {
        if (m == 1) xcat(o, "un mill" "\xF3" "n");
        else { lt_million(m, 1, o); xcat(o, " millones"); }
        if (n % 1000000ull) xcat(o, " ");
    }
    if (n % 1000000ull) lt_million((unsigned)(n % 1000000ull), 0, o);
}

static int ncp;
static int *CP;
static void put_cp(int c) { if (ncp < CP_MAX - 2) CP[ncp++] = c; }
static void put_str(const char *s) { while (*s) put_cp((u8)*s++); }

/* convierte el texto a codepoints, leyendo los numeros en voz alta */
static void prepare(const u8 *s, int len)
{
    const u8 *end = s + len;
    int c, prev = ' ';
    ncp = 0;
    while (s < end) {
        c = dec_next(&s, end);
        if (c == '\r') c = '\n';
        if (is_digit(c) || (c == '-' && s < end && is_digit(*s) && (prev == ' ' || prev == '\n' || prev == '(' || prev == '='))) {
            char dg[24], buf[300];
            int nd = 0, neg = (c == '-'), i;
            u64 v = 0;
            if (neg) c = *s++;
            for (;;) {
                if (nd < 20) dg[nd++] = (char)c;
                if (s < end && is_digit(*s)) { c = *s++; continue; }
                if (s + 3 < end && *s == '.' && is_digit(s[1]) && is_digit(s[2]) && is_digit(s[3]) && !(s + 4 < end && is_digit(s[4]))) {
                    s++; c = *s++; continue;           /* separador de miles: 1.000 */
                }
                break;
            }
            if (neg) put_str("menos ");
            if (nd > 12) {                             /* demasiado largo: cifra por cifra */
                for (i = 0; i < nd; i++) { num_words((u64)(dg[i] - '0'), buf); put_str(buf); put_cp(' '); }
            } else {
                for (i = 0; i < nd; i++) v = v * 10 + (u64)(dg[i] - '0');
                num_words(v, buf); put_str(buf);
            }
            if (s + 1 < end && (*s == ',' || *s == '.') && is_digit(s[1])) {   /* decimales: 3,14 */
                put_str(" coma");
                s++;
                while (s < end && is_digit(*s)) { num_words((u64)(*s - '0'), buf); put_cp(' '); put_str(buf); s++; }
            }
            put_cp(' ');
            prev = ' ';
            continue;
        }
        switch (c) {
        case '%': put_str(" por ciento "); break;
        case '+': put_str(" m" "\xE1" "s "); break;
        case '=': put_str(" igual a "); break;
        case '@': put_str(" arroba "); break;
        case '&': put_str(" y "); break;
        case '#': put_str(" numeral "); break;
        case '/': case '\\': case '(': case ')': case '[': case ']': case '"': case '*': case '_': case '<': case '>': case '|':
            put_cp(' '); break;
        default: put_cp(c); break;
        }
        prev = c;
    }
}

/* ================================================================== */
/* Salida                                                              */
/* ================================================================== */
static s16 *AUD;
static long naud;
static float *FB;
static int aborted;


static int check_esc(void)
{
    int c;
    if (K->esc()) { aborted = 1; return 1; }
    while ((c = K->poll()) != 0)
        if (c == 27 || c == 3) { aborted = 1; return 1; }
    return 0;
}

/* reproduce AUD[0..naud) por el flujo de audio y espera a que acabe */
static void play_all(void)
{
    s16 fr[1024];
    long pos = 0;
    int i;
    if (naud <= 0) return;
    while (pos < naud && !aborted) {
        long m = naud - pos;
        int k;
        if (m > 512) m = 512;
        for (i = 0; i < m; i++) fr[2 * i] = fr[2 * i + 1] = AUD[pos + i];
        k = K->stream(1, fr, (unsigned)m);
        pos += k;
        if (k < m) {
            check_esc();
            K->sleep(4);
        }
    }
    while (!aborted && K->stream(3, 0, 0) > 0) {
        check_esc();
        K->sleep(5);
    }
    if (aborted)                             /* ESC: corta lo que quede en la cola */
        K->stream(2, 0, 0);
    naud = 0;
}

/* ================================================================== */
/* Orquestacion                                                        */
/* ================================================================== */
static char g_lang[4] = "es";
static char g_voice = 'm';
static double g_f0 = 0;
static int g_show;
static unsigned t_synth, n_synth;
static char xs[4096];

static void speak_chunk(const int *cp, int n)
{
    int ni, i;
    long ns;
    double pk = 1e-9, sc;
    int any = 0;
    unsigned t0;
    for (i = 0; i < n; i++) if (is_letter(lowcp(cp[i])) || is_digit(cp[i]) || cp[i] >= 'a') { any = 1; break; }
    if (!any && g_lang[0] == 'e') return;
    if (!xcmp(g_lang, "xs")) g2p_id(cp, n, xs, (int)sizeof xs);
    else if (!xcmp(g_lang, "ja")) g2p_ja(cp, n, xs, (int)sizeof xs);
    else g2p_es(cp, n, xs, (int)sizeof xs);
    if (g_show) { K->print(xs); K->print("\n"); }
    ni = parse_xs(xs, it, MAXITEMS);
    if (ni == 0) return;
    t0 = K->ms();
    sse_on();
    voice_preset(g_voice);
    if (g_f0 > 0) V.f0 = g_f0;
    build(it, ni, g_rate);
    if (nsg == 0) return;
    ns = synth(FB, F_MAX);
    t_synth += K->ms() - t0; n_synth += (unsigned)ns;
    for (i = 0; i < ns; i++) { double a = FB[i] < 0 ? -(double)FB[i] : (double)FB[i]; if (a > pk) pk = a; }
    sc = 0.9 / pk * 32000.0;
    if (naud + ns >= AUD_MAX) ns = AUD_MAX - naud;
    for (i = 0; i < ns; i++) {
        double v = FB[i] * sc;
        AUD[naud + i] = (s16)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
    naud += ns;
}

/* texto completo: lo divide en frases, sintetiza y reproduce por tandas */
static void speak(const u8 *txt, int len)
{
    int s0 = 0, i;
    if (len <= 0) return;
    prepare(txt, len);
    for (i = 0; i <= ncp; i++) {
        int cut = 0;
        if (i == ncp) cut = 1;
        else {
            int c = CP[i];
            if ((c == '.' || c == '?' || c == '!') && (i + 1 >= ncp || CP[i + 1] == ' ' || CP[i + 1] == '\n')) cut = 1;
            else if (c == '\n') cut = 1;
            else if (i - s0 >= 280 && (c == ' ' || c == ',')) cut = 1;
        }
        if (!cut) continue;
        if (i > s0) {
            sse_on();
            speak_chunk(CP + s0, (i < ncp && CP[i] != '\n' ? i + 1 : i) - s0);
            if (check_esc()) return;
            if (naud > AUD_MAX - 2 * 1024 * 1024 || naud > 22050L * 25) play_all();
            if (aborted) return;
        }
        s0 = i + 1;
    }
    play_all();
}

static int load_text(const char *name, u8 *dst, int max)
{
    static const char *const where[] = { "", "USB0.1:", "USB1.1:", "USB0:", "HD0.1:", "HD1.1:", "HD2.1:", "HD0:", "HD1:", "FD1:", "FD0:", "CD0:", 0 };
    char spec[100];
    unsigned total;
    int i, n, j, colon = 0;
    for (j = 0; name[j]; j++) if (name[j] == ':') colon = 1;
    for (i = 0; where[i]; i++) {
        int p = 0;
        if (colon && i > 0) break;
        if (!colon)
            for (j = 0; where[i][j]; j++) spec[p++] = where[i][j];
        for (j = 0; name[j] && p < 98; j++) spec[p++] = name[j];
        spec[p] = 0;
        total = 0;
        n = K->read(spec, (char *)dst, max, &total);
        if (n > 0) return n;
    }
    return -1;
}

static double parse_num(const char *s)
{
    double v = 0, f = 0.1;
    while (is_digit(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',') {
        s++;
        while (is_digit(*s)) { v += (*s++ - '0') * f; f /= 10; }
    }
    return v;
}

static void usage(void)
{
    K->print("SAY \"texto\"     habla el texto (formantes, port de FTTS)\n"
             "SAY \"@ARCHIVO.TXT\"  lee un archivo (CP437 o UTF-8)\n"
             "SAY              modo conversacion (linea vacia o ESC = salir)\n"
             "opciones antes del texto:  -v m|f|c|o|w|r  -r 1.2  -p 150  -l es|ja|xs  -x\n"
             "  -v voz (hombre/f/c nino/o ogro/w susurro/r robot)  -r velocidad  -p tono Hz  -x ver fonemas\n");
}

/* opciones al principio del texto (la consola entrega MAYUSCULAS, asi que no distinguen caso). Devuelve el texto que sigue;
 * (const char *)1 = se pidio la ayuda */
static const char *parse_opts(const char *a)
{
    while (a && *a) {
        while (*a == ' ') a++;
        if (a[0] == '-' && a[1] && (a[2] == ' ' || a[2] == 0)) {
            char o = a[1] >= 'A' && a[1] <= 'Z' ? (char)(a[1] + 32) : a[1];
            const char *v = a + 2;
            while (*v == ' ') v++;
            if (o == 'x') { g_show = 1; a += 2; continue; }
            if (o == 'h' || o == '?') { usage(); return (const char *)1; }
            if (o == 'v') { g_voice = *v ? (*v >= 'A' && *v <= 'Z' ? (char)(*v + 32) : *v) : 'm'; while (*v && *v != ' ') v++; a = v; continue; }
            if (o == 'r') { g_rate = parse_num(v); if (g_rate < 0.3) g_rate = 0.3; if (g_rate > 4) g_rate = 4; while (*v && *v != ' ') v++; a = v; continue; }
            if (o == 'p') { g_f0 = parse_num(v); while (*v && *v != ' ') v++; a = v; continue; }
            if (o == 'l') {
                int k = 0;
                while (*v && *v != ' ' && k < 3) { g_lang[k++] = (*v >= 'A' && *v <= 'Z') ? (char)(*v + 32) : *v; v++; }
                g_lang[k] = 0;
                while (*v && *v != ' ') v++;
                a = v; continue;
            }
        }
        break;
    }
    return a;
}

__attribute__((noinline)) static int real_main(const struct kapi *api)
{
    const char *a;
    u8 *txt = (u8 *)TXTBASE;
    int len, interactive = 0;

    K = api;
    sg = (Seg *)SEGBASE;
    CP = (int *)CPBASE;
    FB = (float *)FBASE;
    AUD = (s16 *)AUDBASE;
    naud = 0; aborted = 0; g_show = 0; t_synth = n_synth = 0; g_f0 = 0; g_rate = 1.0; g_voice = 'm';
    g_lang[0] = 'e'; g_lang[1] = 's'; g_lang[2] = 0;
    rng = 12345;
    sse_on();
    load_overrides();

    a = (K->args && K->args[0]) ? K->args : 0;
    if (a) a = parse_opts(a);
    if (a == (const char *)1) return 0;
    if (!a || !*a) interactive = 1;

    K->stream(2, 0, 0);
    if (!K->stream(0, 0, 0)) {
        K->print("SAY: NO HAY TARJETA DE AUDIO (HDA / AC'97)\n");
        return 1;
    }
    K->stream(4, 0, (unsigned)SR);
    K->stream(5, 0, 1);                       /* ganancia 1: el mezclador multiplica x6 por defecto (efectos de Doom) */
    {   /* un poco de silencio para que la tarjeta arranque sin comerse la primera silaba */
        s16 z[512];
        int i, left = 6000;                     /* ~270 ms */
        for (i = 0; i < 512; i++) z[i] = 0;
        while (left > 0) {
            int k = K->stream(1, z, left > 256 ? 256 : (unsigned)left);
            if (k <= 0) { K->poll(); K->sleep(2); } else left -= k;
        }
    }

    if (!interactive) {
        if (a[0] == '@') {
            len = load_text(a + 1, txt, TXT_MAX);
            if (len < 0) { K->print("SAY: NO ENCUENTRO "); K->print(a + 1); K->print("\n"); K->stream(2, 0, 0); return 1; }
        } else {
            len = xlen(a);
            if (len > 300) len = 300;
            { int i; for (i = 0; i < len; i++) txt[i] = (u8)a[i]; }
        }
        speak(txt, len);
        if (g_show && n_synth) {
            char m[96];
            int j = 0;
            unsigned v[3], w;
            const char *l1 = "SAY: ", *l2 = " MS DE VOZ SINTETIZADOS EN ", *l3 = " MS\n";
            v[0] = (unsigned)((u64)n_synth * 1000u / (unsigned)SR); v[1] = t_synth;
            while (*l1) m[j++] = *l1++;
            for (w = 0; w < 2; w++) {
                char d[12]; int n = 0; unsigned x = v[w];
                do { d[n++] = (char)('0' + x % 10); x /= 10; } while (x);
                while (n) m[j++] = d[--n];
                if (w == 0) { const char *p = l2; while (*p) m[j++] = *p++; }
            }
            { const char *p = l3; while (*p) m[j++] = *p++; }
            m[j] = 0;
            K->print(m);
        }
    } else {
        K->print("SAY (formantes, port de FTTS)  -  escribe una frase y ENTER; linea vacia o ESC = salir\n");
        for (;;) {
            int n = 0, c;
            K->print("SAY> ");
            for (;;) {
                c = K->poll();
                if (!c && K->esc()) c = 27;
                if (!c) { K->sleep(10); continue; }
                if (c == 27) { n = -1; break; }
                if (c == 13 || c == 10) { K->print("\n"); break; }
                if ((c == 8 || c == 20 || c == 127) && n > 0) { n--; K->print("\x08 \x08"); continue; }
                if (c >= 32 && n < 250) { char s[2]; txt[n++] = (u8)c; s[0] = (char)c; s[1] = 0; K->print(s); }
            }
            if (n <= 0) break;
            aborted = 0;
            txt[n] = 0;
            {
                const char *p = parse_opts((const char *)txt);
                if (p == (const char *)1) continue;
                if (*p) speak((const u8 *)p, n - (int)(p - (const char *)txt));
            }
        }
    }
    K->stream(2, 0, 0);
    return 0;
}

int mod_main(const struct kapi *api)
{
    sse_on();                                /* antes de que nada toque un registro xmm */
    return real_main(api);
}
