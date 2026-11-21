/* driver de juguete: compila como PIE ELF con gcc pelon, habla por el kapi que recibe */
struct tkapi { void (*print)(const char *); };
static const char *msg = "   [driver ELF] hola, me cargo el loader y hablo por el kapi\n";
static int nums[3] = { 10, 20, 30 };
static int suma(void) { int i, s = 0; for (i = 0; i < 3; i++) s += nums[i]; return s; }
static int (*tabla[1])(void) = { suma };   /* puntero global -> fuerza R_X86_64_RELATIVE */
int mod_main(const struct tkapi *K) { K->print(msg); return tabla[0](); }  /* -> 60 */
