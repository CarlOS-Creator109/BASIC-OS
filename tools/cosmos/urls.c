/* urls.c - url_resolve() del motor de Cosmos Browser (sacado de net.c sin cambios) */
#include "cosmos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void remove_dots(char *path) {
    /* path empieza con '/' ; quita ./ y ../ */
    char *segs[256]; int n = 0;
    char *copy = strdup(path);
    char *save = NULL;
    int trailing = strlen(path) > 0 && path[strlen(path) - 1] == '/';
    for (char *s = strtok_r(copy, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (n > 0) n--; continue; }
        if (n < 256) segs[n++] = s;
    }
    char *o = path;
    for (int i = 0; i < n; i++) { *o++ = '/'; size_t l = strlen(segs[i]); memmove(o, segs[i], l); o += l; }
    if (n == 0 || trailing) *o++ = '/';
    *o = 0;
    free(copy);
}

static int has_scheme(const char *s) {
    if (!isalpha((unsigned char)*s)) return 0;
    const char *p = s + 1;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    return *p == ':';
}

void url_resolve(const char *base, const char *ref, char *out, size_t cap) {
    char r[2048];
    while (isspace((unsigned char)*ref)) ref++;
    snprintf(r, sizeof r, "%s", ref);
    size_t rl = strlen(r);
    while (rl && isspace((unsigned char)r[rl - 1])) r[--rl] = 0;
    if (has_scheme(r)) { snprintf(out, cap, "%s", r); return; }
    if (!base || !*base) { snprintf(out, cap, "%s", r); return; }
    const char *sep = strstr(base, "://");
    if (!sep) {                                  /* base tipo cosmos:home */
        snprintf(out, cap, "%s", r);
        return;
    }
    char scheme[16]; size_t sl = sep - base;
    if (sl >= sizeof scheme) sl = sizeof scheme - 1;
    memcpy(scheme, base, sl); scheme[sl] = 0;
    if (r[0] == '/' && r[1] == '/') { snprintf(out, cap, "%s:%s", scheme, r); return; }
    const char *auth = sep + 3;
    const char *pe = auth + strcspn(auth, "/?#");           /* fin de autoridad */
    char origin[1024];
    size_t ol = pe - base; if (ol >= sizeof origin) ol = sizeof origin - 1;
    memcpy(origin, base, ol); origin[ol] = 0;
    char bpath[1536] = "/";
    const char *pq = pe + strcspn(pe, "?#");
    if (pq > pe) { size_t pl = pq - pe; if (pl >= sizeof bpath) pl = sizeof bpath - 1; memcpy(bpath, pe, pl); bpath[pl] = 0; }
    char bnofrag[2048];
    snprintf(bnofrag, sizeof bnofrag, "%s", base);
    char *h = strchr(bnofrag, '#'); if (h) *h = 0;
    if (r[0] == 0) { snprintf(out, cap, "%s", bnofrag); return; }
    if (r[0] == '#') { snprintf(out, cap, "%s%s", bnofrag, r); return; }
    if (r[0] == '?') {
        char b2[2048]; snprintf(b2, sizeof b2, "%s", bnofrag);
        char *qm = strchr(b2, '?'); if (qm) *qm = 0;
        snprintf(out, cap, "%s%s", b2, r);
        return;
    }
    char np[3072];
    if (r[0] == '/') snprintf(np, sizeof np, "%s", r);
    else {
        char dir[1536]; snprintf(dir, sizeof dir, "%s", bpath);
        char *ls = strrchr(dir, '/'); if (ls) ls[1] = 0;
        snprintf(np, sizeof np, "%s%s", dir, r);
    }
    /* separar ruta de ?query#frag */
    char tail[1024] = "";
    size_t cut = strcspn(np, "?#");
    snprintf(tail, sizeof tail, "%s", np + cut);
    np[cut] = 0;
    remove_dots(np);
    snprintf(out, cap, "%s%s%s", origin, np, tail);
}

