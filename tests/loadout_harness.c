// Test harness for runtime/sr/byml.c and loadout.c (tests/test_loadout.py).
//   loadout_harness roundtrip <in> <out>                  the table read and written again
//   loadout_harness loadout <in> <out> KEY=value ...      with the SR_LOADOUT_* settings given; prints "built <bytes>"
//                                                         or "none" (no change, or it would not fit)
//   loadout_harness hash <name>                           the id of a name
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "byml.h"

uint32_t loadout_hash(const char *s);
uint8_t *loadout_table_build(const uint8_t *raw, size_t n, size_t *out_n);

static char **g_env;
static int g_nenv;

const char *cfg_get(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < g_nenv; i++)
        if (!strncmp(g_env[i], name, n) && g_env[i][n] == '=') return g_env[i] + n + 1;
    return NULL;
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static uint8_t buf[4 << 20];
    *n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return buf;
}

static int spit(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(d, 1, n, f) == n;
    if (f) fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "hash")) {
        printf("%u\n", loadout_hash(argv[2]));
        return 0;
    }
    if (argc < 4) return 2;
    size_t n = 0, out_n = 0;
    uint8_t *raw = slurp(argv[2], &n), *out = NULL;
    if (!raw) return 2;
    if (!strcmp(argv[1], "roundtrip")) {
        Byml *b = byml_parse(raw, n);
        if (!b) {
            puts("unreadable");
            return 0;
        }
        out = byml_write(b, &out_n);
        byml_free(b);
    } else {
        g_env = argv + 4, g_nenv = argc - 4;
        out = loadout_table_build(raw, n, &out_n);
    }
    if (!out) {
        puts("none");
        return 0;
    }
    printf("built %zu\n", out_n);
    return spit(argv[3], out, out_n) ? 0 : 2;
}
