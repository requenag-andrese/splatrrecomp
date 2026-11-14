// Test harness for runtime/sr/ink_color.c (tests/test_ink_color.py): patches an unpacked table file and writes it as
// the zstd frame the runtime serves.   ink_color_harness <in> <out> <player rrggbb or -> <enemy rrggbb or ->
// Prints the number of floats set; the output file is written only when that is above 0.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ink_wanted(void);
int ink_table_patch(uint8_t *raw, size_t n);
void *ink_table_frame(const uint8_t *raw, size_t n, size_t *out_n);

static const char *g_player, *g_enemy;

const char *cfg_get(const char *name) {
    return !strcmp(name, "SR_INK_COLOR") ? g_player : !strcmp(name, "SR_ENEMY_INK_COLOR") ? g_enemy : NULL;
}

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    g_player = strcmp(argv[3], "-") ? argv[3] : NULL;
    g_enemy = strcmp(argv[4], "-") ? argv[4] : NULL;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t raw[1 << 20];
    size_t n = fread(raw, 1, sizeof raw, f), zn = 0;
    fclose(f);
    int set = ink_wanted() ? ink_table_patch(raw, n) : 0;
    printf("%d\n", set);
    if (set > 0) {
        void *z = ink_table_frame(raw, n, &zn);
        f = fopen(argv[2], "wb");
        if (!z || !f || fwrite(z, 1, zn, f) != zn) return 2;
        fclose(f);
    }
    return 0;
}
