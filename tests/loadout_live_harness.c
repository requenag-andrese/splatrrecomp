// Test harness for runtime/sr/loadout_live.c (tests/test_loadout_live.py): the object's fields as an array, the game's
// name tables as three short lists. Arguments run in order:
//   KEY=value        a setting (SR_LOADOUT_*); the next look sees the settings as changed
//   set:i=v,i=v      the game writes fields (by index)
//   look             one look at the fields; prints "write i=v,..." (what it wrote, applied to the fields) or "none"
//   offset:i         prints field i's offset in the object
//   hash:name        prints the id of a name
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loadout_live.h"

uint32_t loadout_hash(const char *s);

static const char *g_env[64];
static int g_nenv;

const char *cfg_get(const char *name) {
    size_t n = strlen(name);
    for (int i = g_nenv - 1; i >= 0; i--)
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

// What the runtime gives the game-side half of loadout_live.c, unused here.
typedef struct RtLayoutEntry { const char *name; uint64_t base, size; } RtLayoutEntry;
const RtLayoutEntry rt_layout[1] = {{"none", 0, 0}};
const int rt_layout_count = 0;
unsigned cfg_version(void) { return 0; }
int plat_thread_spawn(void (*fn)(void *), void *arg, const char *name, size_t stack) {
    (void)fn, (void)arg, (void)name, (void)stack;
    return 0;
}
void plat_sleep_ms(uint32_t ms) { (void)ms; }

static const char *const tables[LIVE_TABLES][8] = {
    [LIVE_TABLE_WEAPON] = {"Invalid", "Shooter_A", "Shooter_B", "Roller_Z"},
    [LIVE_TABLE_GADGET] = {"Invalid", "Slam", "Boomerang", "Hook", "SpMorayShower", "SpMantaSurfing", "SpSharkCharge"},
    [LIVE_TABLE_POWER] = {"Invalid", "Cmn_A", "Cmn_MultiJump", "Wpn_ToFrozen"},
};

static int index_of(void *ctx, int table, const char *name) {
    (void)ctx;
    for (int i = 1; i < 8 && tables[table][i]; i++)
        if (!strcmp(tables[table][i], name)) return i - 1;
    return -1;
}

int main(int argc, char **argv) {
    static const LiveNames names = {index_of, NULL};
    static LiveState st;
    static uint32_t now[LIVE_FIELDS], out[LIVE_FIELDS];
    int fresh = 1;
    for (int a = 1; a < argc; a++) {
        const char *arg = argv[a];
        if (!strncmp(arg, "set:", 4)) {
            for (const char *p = arg + 4; *p;) {
                char *end;
                long i = strtol(p, &end, 10);
                unsigned long v = strtoul(end + 1, &end, 10);
                if (i >= 0 && i < LIVE_FIELDS) now[i] = (uint32_t)v;
                p = *end ? end + 1 : end;
            }
        } else if (!strcmp(arg, "look")) {
            if (loadout_live_step(&st, &names, now, fresh, out)) {
                printf("write");
                for (int i = 0, n = 0; i < LIVE_FIELDS; i++)
                    if (out[i] != now[i]) printf("%s%d=%u", n++ ? "," : " ", i, out[i]);
                printf("\n");
                memcpy(now, out, sizeof now);
            } else {
                printf("none\n");
            }
            fresh = 0;
        } else if (!strncmp(arg, "offset:", 7)) {
            printf("%u\n", loadout_live_offset(atoi(arg + 7)));
        } else if (!strncmp(arg, "hash:", 5)) {
            printf("%u\n", loadout_hash(arg + 5));
        } else if (strchr(arg, '=') && g_nenv < 64) {
            g_env[g_nenv++] = arg;
            fresh = 1;
        }
    }
    return 0;
}
