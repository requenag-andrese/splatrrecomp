// Test harness for runtime/sr/loadout_names.c (tests/test_loadout_names.py): the lists lo_names_build makes from a
// text archive (an unpacked SARC), a file of part file names (one a line) and a file of gadget rows
// ("<id> <short name> <tank> <order>" a line).
//   loadout_names_harness <sarc> <parts> <gadgets> [<icons>]   prints the lists, one item a line (with <icons>, a file
//                                                     of part picture names, each part type's icon too)
//   loadout_names_harness mals <switch code>           prints the text archive of a language
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loadout_names.h"

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)len + 1);
    *n = b ? fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    if (b) b[*n] = 0;
    return b;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "mals")) {
        printf("%s\n", lo_names_mals(argv[2]));
        return 0;
    }
    if (argc != 4 && argc != 5) return 2;
    size_t n = 0, pn = 0, gn = 0;
    unsigned char *sarc = slurp(argv[1], &n), *ptxt = slurp(argv[2], &pn), *gtxt = slurp(argv[3], &gn);
    if (!sarc || !ptxt || !gtxt) return 3;
    const char *parts[4096];
    int n_parts = 0;
    for (char *line = strtok((char *)ptxt, "\r\n"); line && n_parts < 4096; line = strtok(NULL, "\r\n")) parts[n_parts++] = line;
    LoGadgetRow rows[64];
    int n_rows = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)gtxt, "\r\n", &save); line && n_rows < 64; line = strtok_r(NULL, "\r\n", &save)) {
        char id[64], sn[16];
        int tank, order;
        if (sscanf(line, "%63s %15s %d %d", id, sn, &tank, &order) == 4)
            rows[n_rows++] = (LoGadgetRow){strdup(id), strdup(sn), tank, order};
    }
    LoNames *L = lo_names_build(sarc, n, parts, n_parts, rows, n_rows);
    if (L && argc == 5) {
        size_t in = 0;
        unsigned char *itxt = slurp(argv[4], &in);
        const char *icons[1024];
        int n_icons = 0;
        char *s2 = NULL;
        for (char *line = itxt ? strtok_r((char *)itxt, "\r\n", &s2) : NULL; line && n_icons < 1024;
             line = strtok_r(NULL, "\r\n", &s2))
            icons[n_icons++] = line;
        lo_names_icons(L, icons, n_icons);
    }
    if (!L) {
        printf("none\n");
        return 0;
    }
    for (int c = 0; c < L->n_cats; c++) printf("cat %s=%s\n", L->cats[c].id, L->cats[c].name);
    for (int i = 0; i < L->n_weapons; i++) printf("weapon %s=%s cat %d\n", L->weapons[i].id, L->weapons[i].name, L->weapon_cat[i]);
    for (int i = 0; i < L->n_powers; i++) printf("power %s=%s%s\n", L->powers[i].id, L->powers[i].name, i < L->n_relic_powers ? " relic" : "");
    for (int t = 0; t < 4; t++) printf("tank %s=%s\n", L->tanks[t].id, L->tanks[t].name ? L->tanks[t].name : "(none)");
    for (int b = 0; b < 3; b++) printf("buddy %s=%s\n", L->buddies[b].id, L->buddies[b].name);
    for (int g = 0; g < L->n_gadgets; g++) {
        printf("gadget %s=%s\n", L->gadgets[g].id, L->gadgets[g].name);
        for (int t = 0; t < L->gadgets[g].n_types; t++) {
            const LoPartType *T = &L->gadgets[g].types[t];
            printf("  type %s=%s desc %s", T->id, T->name, T->desc ? T->desc : "(none)");
            if (argc == 5) printf(" icon %s", T->icon ? T->icon : "(none)");
            printf(":");
            for (int k = 0; k < T->n; k++) printf(" %s", T->parts[k]);
            printf("\n");
        }
    }
    return 0;
}
