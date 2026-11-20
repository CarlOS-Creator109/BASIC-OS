/* tlstest.c - prueba el cliente TLS 1.3 de BN en Linux, contra un servidor real (sockets).
 * uso: tlstest <host> <puerto> [ruta]      (ej: tlstest 127.0.0.1 8443 /) */
#include "tls.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int s_send(void *io, const u8 *p, int n) { return (int)send(*(int *)io, p, n, 0); }
static int s_recv(void *io, u8 *p, int n) { return (int)recv(*(int *)io, p, n, 0); }
static void s_log(const char *s) { fputs(s, stderr); }

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8443;
    const char *path = argc > 3 ? argv[3] : "/";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port); inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { perror("connect"); return 1; }

    static tls_conn c; char req[512]; u8 buf[8192]; int n, total = 0;
    c.log = s_log;
    tls_seed((u32)time(0) ^ (u32)getpid());
    if (tls_connect(&c, &fd, s_send, s_recv, host)) { fprintf(stderr, "handshake FALLO\n"); return 1; }
    fprintf(stderr, "== handshake TLS 1.3 OK ==\n");

    n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BN/1\r\nConnection: close\r\n\r\n", path, host);
    if (tls_write(&c, (u8 *)req, n)) { fprintf(stderr, "write FALLO\n"); return 1; }

    for (;;) {
        n = tls_read(&c, buf, sizeof buf);
        if (n <= 0) break;
        fwrite(buf, 1, n, stdout); total += n;
    }
    fprintf(stderr, "\n== %d bytes de aplicacion descifrados ==\n", total);
    return 0;
}
