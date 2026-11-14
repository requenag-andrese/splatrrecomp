// sr-recomp: the Loadout tab (loadout_page.h). Every row writes a loadout_* setting (cfg_set); sr/loadout_live.c sees
// the change within a frame or two and puts it on the player, and sr-config.ini keeps it. The names are the game's
// own, read from the dump in the game's language (sr/loadout_names.c).
//
//   Custom loadout            loadout
//   Weapon type, Weapon       loadout_weapon (the type row picks the first of a type, the weapon row steps in it)
//   Weapon level              loadout_weapon_lv
//   Weapon power 1..3         loadout_weapon_powers   left / right: the power; A: its level
//   Tank, Bot Buddy           loadout_tank, loadout_buddy
//   Gadget (L / R / A)        loadout_gadget_l / _r / _a
//   Parts (L / R / A)         loadout_parts_*: a list of the gadget's part types, each off or at one of its levels
//   Relic power 1..5          loadout_powers
//   Back to your own          every loadout_* key unset but loadout
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "ui/icon_atlas.h"
#include "ui/loadout_page.h"
#include "ui/option_rows.h"
extern "C" {
#include "cfg.h"
#include "ui_text.h"
}
#include "../../sr/loadout_names.h"

enum { WEAPON_POWERS = 3, RELIC_POWERS = 5, MAX_PARTS = 9, MAX_LEVEL = 50, POWER_LEVELS = 3 };

static int g_pick = -1;         // the gadget slot whose parts list is open; -1 none
static int g_pick_closed = -1;  // the ImGui frame the list closed on: that frame's B / Esc is the list's, not the menu's

bool loadout_modal(void) { return g_pick >= 0 || g_pick_closed == ImGui::GetFrameCount(); }

static const char *value_of(const char *key) {
    const char *v = cfg_get(key);
    return v ? v : "";
}

// A row of this page's own is greyed out like the table's rows when the environment fixes its setting.
static void lock_if_env(const char *key) {
    if (cfg_from_env(key)) opt_disable_next(UI_MENU_SET_BY_ENV);
}

static void set(const char *key, const char *value) {
    cfg_set(key, value);
    opt_save_soon();
}

// The game's icon of an item for the next row: UI/Tex/<folder>/<id>.bntx.zs (Weapon, Gadget, Relic, GadgetParts,
// SortCategory), when it is ready.
static void icon(const char *folder, const char *id) {
    char path[128];
    ImTextureID tex;
    ImVec2 a, b;
    if (!id || !*id) return;
    snprintf(path, sizeof path, "UI/Tex/%s/%s.bntx.zs", folder, id);
    if (icon_get(path, &tex, &a, &b)) opt_icon_next(tex, a, b, path);
}

static int weapon_index(const LoNames *L, const char *id) {
    for (int i = 0; i < L->n_weapons; i++)
        if (!strcmp(L->weapons[i].id, id)) return i;
    return -1;
}

static const LoGadget *gadget_of(const LoNames *L, const char *id) {
    for (int i = 0; i < L->n_gadgets; i++)
        if (!strcmp(L->gadgets[i].id, id)) return &L->gadgets[i];
    return nullptr;
}

// ---- the weapon
static void weapon_rows(const LoNames *L) {
    static const char *values[64], *names[64];
    const char *cur_id = value_of("SR_LOADOUT_WEAPON");
    int cur = weapon_index(L, cur_id), n = 1, cur_row = *cur_id ? -1 : 0;
    values[0] = "", names[0] = menu_text(UI_SR_LOADOUT_OWN);
    for (int c = 0; c < L->n_cats && n < 64; c++, n++) {
        int first = -1;
        for (int i = 0; i < L->n_weapons && first < 0; i++)
            if (L->weapon_cat[i] == c) first = i;
        values[n] = cur >= 0 && L->weapon_cat[cur] == c ? L->weapons[cur].id : first >= 0 ? L->weapons[first].id : "";
        names[n] = L->cats[c].name;
        if (cur >= 0 && L->weapon_cat[cur] == c) cur_row = n;
    }
    if (cur >= 0) {   // the type's icon: SortCategory/Wpn_<category>
        char cat[48];
        snprintf(cat, sizeof cat, "Wpn_%s", L->cats[L->weapon_cat[cur]].id);
        icon("SortCategory", cat);
    }
    opt_choice_list("SR_LOADOUT_WEAPON", UI_SR_LOADOUT_WEAPON_KIND, values, names, n, UI_SR_LOADOUT_WEAPON_KIND_DESC, cur_row);
    cur = weapon_index(L, value_of("SR_LOADOUT_WEAPON"));
    if (cur >= 0) {
        lock_if_env("SR_LOADOUT_WEAPON");
        icon("Weapon", L->weapons[cur].id);
        int step = opt_step_row("##lo_weapon", menu_text(UI_SR_LOADOUT_WEAPON), L->weapons[cur].name,
                                menu_text(UI_SR_LOADOUT_WEAPON_DESC));
        int next = step == 2 ? cur + 1 : cur + (step == 1 || step == -1 ? step : 0);
        if (step == 2 && (next >= L->n_weapons || L->weapon_cat[next] != L->weapon_cat[cur]))   // A goes round the type
            while (next - 1 >= 0 && L->weapon_cat[next - 1] == L->weapon_cat[cur]) next--;
        if (next != cur && next >= 0 && next < L->n_weapons && L->weapon_cat[next] == L->weapon_cat[cur])
            set("SR_LOADOUT_WEAPON", L->weapons[next].id);
    }
    // the level: "your own", then 1 to MAX_LEVEL
    int lv = atoi(value_of("SR_LOADOUT_WEAPON_LV"));
    if (lv < 1) lv = 0;
    char shown[48];
    if (lv) snprintf(shown, sizeof shown, menu_text(UI_SR_LOADOUT_LEVEL), lv);
    else snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_OWN));
    bool held = false;
    lock_if_env("SR_LOADOUT_WEAPON_LV");
    int step = opt_step_row("##lo_weapon_lv", menu_text(UI_SR_LOADOUT_WEAPON_LV), shown,
                            menu_text(UI_SR_LOADOUT_WEAPON_LV_DESC), &held);
    if (step) {
        int d = step == 2 ? 1 : step * (held ? 5 : 1), nv = lv + d;
        if (step == 2 && nv > MAX_LEVEL) nv = 0;
        nv = nv < 0 ? 0 : nv > MAX_LEVEL ? MAX_LEVEL : nv;
        if (nv != lv) {
            char v[16];
            snprintf(v, sizeof v, "%d", nv);
            set("SR_LOADOUT_WEAPON_LV", nv ? v : "");
        }
    }
}

// ---- power slots (a weapon's three, the five relic ones): "own", "none" or "<power>:<level>", in a comma list
struct Slot { int kind, power, level; };   // kind 0 own, 1 none, 2 a power of the list
enum { OWN, NONE_SLOT, POWER };

static void parse_slots(const LoNames *L, const char *key, Slot *s, int n) {
    for (int i = 0; i < n; i++) s[i] = {OWN, -1, 1};
    const char *v = value_of(key);
    char item[96];
    for (int i = 0; i < n && *v;) {
        while (*v == ' ' || *v == ',') v++;
        size_t len = strcspn(v, ",");
        if (!len) break;
        snprintf(item, sizeof item, "%.*s", (int)len, v);
        v += len;
        char *end = item + strlen(item);
        while (end > item && end[-1] == ' ') *--end = 0;
        int level = 1;
        if (char *colon = strrchr(item, ':')) *colon = 0, level = atoi(colon + 1);
        if (!strcmp(item, "none") || !strcmp(item, "None")) s[i] = {NONE_SLOT, -1, 1};
        else if (strcmp(item, "own"))
            for (int p = 0; p < L->n_powers; p++)
                if (!strcmp(L->powers[p].id, item)) s[i] = {POWER, p, level < 1 ? 1 : level};
        i++;
    }
}

static void write_slots(const LoNames *L, const char *key, const Slot *s, int n) {
    char out[512] = "";
    int last = -1;
    for (int i = 0; i < n; i++)
        if (s[i].kind != OWN) last = i;
    for (int i = 0; i <= last; i++) {
        char item[96];
        if (s[i].kind == OWN) snprintf(item, sizeof item, "own");
        else if (s[i].kind == NONE_SLOT) snprintf(item, sizeof item, "none");
        else snprintf(item, sizeof item, "%s:%d", L->powers[s[i].power].id, s[i].level);
        size_t used = strlen(out);
        snprintf(out + used, sizeof out - used, "%s%s", i ? "," : "", item);
    }
    set(key, out);
}

static void power_rows(const LoNames *L, const char *key, int slots, int choices, UiTextId label, UiTextId desc) {
    Slot s[RELIC_POWERS];
    parse_slots(L, key, s, slots);
    for (int i = 0; i < slots; i++) {
        char id[32], lab[96], shown[128];
        snprintf(id, sizeof id, "##lo_%s_%d", key, i);
        snprintf(lab, sizeof lab, menu_text(label), i + 1);
        if (s[i].kind == OWN) snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_OWN));
        else if (s[i].kind == NONE_SLOT) snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_NONE));
        else snprintf(shown, sizeof shown, "%s %d", L->powers[s[i].power].name, s[i].level);
        lock_if_env(key);
        if (s[i].kind == POWER) icon("Relic", L->powers[s[i].power].id);
        int step = opt_step_row(id, lab, shown, menu_text(desc));
        if (!step) continue;
        // the list: own, none, the powers; A steps the level of a power
        int at = s[i].kind == OWN ? 0 : s[i].kind == NONE_SLOT ? 1 : 2 + s[i].power, n = 2 + choices;
        if (step == 2) {
            if (s[i].kind != POWER) continue;
            s[i].level = s[i].level % POWER_LEVELS + 1;
        } else {
            int to = at + step;
            if (to < 0 || to >= n) continue;
            s[i] = to == 0 ? Slot{OWN, -1, 1} : to == 1 ? Slot{NONE_SLOT, -1, 1} : Slot{POWER, to - 2, s[i].kind == POWER ? s[i].level : 1};
        }
        write_slots(L, key, s, slots);
    }
}

// ---- gadgets and their parts
static const char *const kGadgetKey[3] = {"SR_LOADOUT_GADGET_L", "SR_LOADOUT_GADGET_R", "SR_LOADOUT_GADGET_A"};
static const char *const kPartsKey[3] = {"SR_LOADOUT_PARTS_L", "SR_LOADOUT_PARTS_R", "SR_LOADOUT_PARTS_A"};
static const UiTextId kGadgetLabel[3] = {UI_SR_LOADOUT_GADGET_L, UI_SR_LOADOUT_GADGET_R, UI_SR_LOADOUT_GADGET_A};
static const UiTextId kPartsLabel[3] = {UI_SR_LOADOUT_PARTS_L, UI_SR_LOADOUT_PARTS_R, UI_SR_LOADOUT_PARTS_A};

static int count_items(const char *v) {
    int n = 0;
    for (const char *p = v; *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t len = strcspn(p, ",");
        if (len && strncmp(p, "none", 4)) n++;
        p += len;
    }
    return n;
}

static bool has_item(const char *list, const char *item) {
    size_t n = strlen(item);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t len = strcspn(p, ",");
        while (len && p[len - 1] == ' ') len--;
        if (len == n && !strncmp(p, item, n)) return true;
        p += strcspn(p, ",");
    }
    return false;
}

static void gadget_rows(const LoNames *L) {
    static const char *values[64], *names[64];
    int n = 0;
    values[n] = "", names[n++] = menu_text(UI_SR_LOADOUT_OWN);
    values[n] = "none", names[n++] = menu_text(UI_SR_LOADOUT_EMPTY);
    for (int i = 0; i < L->n_gadgets && n < 64; i++) values[n] = L->gadgets[i].id, names[n++] = L->gadgets[i].name;
    for (int s = 0; s < 3; s++) {
        icon("Gadget", gadget_of(L, value_of(kGadgetKey[s])) ? value_of(kGadgetKey[s]) : nullptr);
        opt_choice_list(kGadgetKey[s], kGadgetLabel[s], values, names, n, UI_SR_LOADOUT_GADGET_DESC);
        const LoGadget *G = gadget_of(L, value_of(kGadgetKey[s]));
        const char *parts = value_of(kPartsKey[s]);
        char shown[48], id[24];
        if (!*parts) snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_OWN));
        else if (!count_items(parts)) snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_NONE));
        else snprintf(shown, sizeof shown, menu_text(UI_SR_LOADOUT_PARTS_COUNT), count_items(parts));
        if (!G) opt_disable_next(UI_SR_LOADOUT_PARTS_PICK_GADGET);
        else lock_if_env(kPartsKey[s]);
        snprintf(id, sizeof id, "##lo_parts_%d", s);
        if (opt_button(id, kPartsLabel[s], UI_SR_LOADOUT_PARTS_DESC, shown) && G) {
            g_pick = s;
            opt_focus_first();
        }
    }
}

static void close_pick(void) {
    char id[24];
    snprintf(id, sizeof id, "##lo_parts_%d", g_pick);
    g_pick = -1;
    g_pick_closed = ImGui::GetFrameCount();
    opt_focus_key(id);   // back on the row it came from
}

// The parts list of the open slot: one row per part type of its gadget, off or at one of the type's levels.
static void parts_page(const LoNames *L) {
    const LoGadget *G = gadget_of(L, value_of(kGadgetKey[g_pick]));
    if (!G || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        close_pick();
        return;
    }
    const char *key = kPartsKey[g_pick];
    const char *now = value_of(key);
    int level[128] = {0}, on = 0, types = G->n_types < 128 ? G->n_types : 128;
    for (int t = 0; t < types; t++)
        for (int k = 0; k < G->types[t].n; k++)
            if (has_item(now, G->types[t].parts[k])) level[t] = k + 1;
    for (int t = 0; t < types; t++) on += level[t] > 0;
    char title[160];
    snprintf(title, sizeof title, menu_text(UI_SR_LOADOUT_PARTS_TITLE), G->name, on, MAX_PARTS);
    opt_group_text(title);
    if (opt_button("##lo_parts_done", UI_SR_LOADOUT_PARTS_DONE, UI_SR_LOADOUT_PARTS_DONE_DESC)) {
        close_pick();
        return;
    }
    if (opt_button("##lo_parts_own", UI_SR_LOADOUT_PARTS_OWN, UI_SR_LOADOUT_PARTS_OWN_DESC, *now ? "" : menu_text(UI_SR_LOADOUT_SELECTED))) {
        set(key, "");
        close_pick();
        return;
    }
    bool changed = false;
    for (int t = 0; t < types; t++) {
        const LoPartType &T = G->types[t];
        char id[24], shown[64], desc[1024];
        snprintf(id, sizeof id, "##lo_pt_%d", t);
        if (level[t]) snprintf(shown, sizeof shown, menu_text(UI_SR_LOADOUT_PART_LEVEL), level[t], T.n);
        else snprintf(shown, sizeof shown, "%s", menu_text(UI_SR_LOADOUT_PART_OFF));
        snprintf(desc, sizeof desc, "%s%s%s", T.desc ? T.desc : "", T.desc ? "\n\n" : "", menu_text(UI_SR_LOADOUT_PART_DESC));
        bool held = false;
        icon("GadgetParts", T.icon);
        int step = opt_step_row(id, T.name, shown, desc, &held);
        if (!step) continue;
        int nv = step == 2 ? (level[t] ? 0 : T.n) : level[t] + step * (held ? 3 : 1);
        nv = nv < 0 ? 0 : nv > T.n ? T.n : nv;
        if (!level[t] && nv && on >= MAX_PARTS) continue;   // the gadget holds no more
        if (nv != level[t]) level[t] = nv, changed = true;
    }
    if (!changed) return;
    char out[2048] = "";
    for (int t = 0; t < types; t++) {
        if (!level[t]) continue;
        size_t used = strlen(out);
        snprintf(out + used, sizeof out - used, "%s%s", used ? "," : "", G->types[t].parts[level[t] - 1]);
    }
    set(key, *out ? out : "none");
}

void loadout_rows(float scale) {
    (void)scale;
    const LoNames *L = lo_names();
    if (g_pick >= 0 && L) {
        parts_page(L);
        return;
    }
    opt_text(UI_SR_EXPERIMENTAL, true);
    opt_text(UI_SR_LOADOUT_NOTE);
    opt_toggle("SR_LOADOUT", UI_SR_LOADOUT_ON, UI_SR_LOADOUT_ON_DESC);
    if (!L) {
        opt_text(UI_SR_LOADOUT_NO_NAMES, true);
        return;
    }
    opt_group(UI_SR_LOADOUT_GROUP_WEAPON);
    weapon_rows(L);
    power_rows(L, "SR_LOADOUT_WEAPON_POWERS", WEAPON_POWERS, L->n_powers, UI_SR_LOADOUT_WEAPON_POWER,
               UI_SR_LOADOUT_WEAPON_POWER_DESC);

    opt_group(UI_SR_LOADOUT_GROUP_TANK);
    static const char *tank_values[5] = {"", "Default", "Speed", "Power", "Tactical"}, *tank_names[5];
    tank_names[0] = menu_text(UI_SR_LOADOUT_OWN);
    for (int t = 0; t < 4; t++) tank_names[t + 1] = L->tanks[t].name ? L->tanks[t].name : menu_text(UI_SR_LOADOUT_TANK_DEFAULT);
    opt_choice_list("SR_LOADOUT_TANK", UI_SR_LOADOUT_TANK, tank_values, tank_names, 5, UI_SR_LOADOUT_TANK_DESC);
    static const char *buddy_values[4] = {"", "Shiver", "Frye", "BigMan"}, *buddy_names[4];
    buddy_names[0] = menu_text(UI_SR_LOADOUT_OWN);
    for (int b = 0; b < 3; b++) buddy_names[b + 1] = L->buddies[b].name;
    static const char *const buddy_special[3] = {"SpSharkCharge", "SpMorayShower", "SpMantaSurfing"};   // their Showstoppers
    for (int b = 0; b < 3; b++)
        if (!strcmp(value_of("SR_LOADOUT_BUDDY"), buddy_values[b + 1])) icon("Gadget", buddy_special[b]);
    opt_choice_list("SR_LOADOUT_BUDDY", UI_SR_LOADOUT_BUDDY, buddy_values, buddy_names, 4, UI_SR_LOADOUT_BUDDY_DESC);

    opt_group(UI_SR_LOADOUT_GROUP_GADGETS);
    gadget_rows(L);

    opt_group(UI_SR_LOADOUT_GROUP_RELICS);
    power_rows(L, "SR_LOADOUT_POWERS", RELIC_POWERS, L->n_relic_powers, UI_SR_LOADOUT_RELIC_POWER,
               UI_SR_LOADOUT_RELIC_POWER_DESC);

    ImGui::Dummy(ImVec2(0, 4));
    if (opt_button("##lo_clear", UI_SR_LOADOUT_CLEAR, UI_SR_LOADOUT_CLEAR_DESC)) {
        static const char *const keys[] = {"SR_LOADOUT_WEAPON", "SR_LOADOUT_WEAPON_LV", "SR_LOADOUT_WEAPON_POWERS",
                                           "SR_LOADOUT_TANK", "SR_LOADOUT_BUDDY", "SR_LOADOUT_GADGET_L",
                                           "SR_LOADOUT_GADGET_R", "SR_LOADOUT_GADGET_A", "SR_LOADOUT_PARTS_L",
                                           "SR_LOADOUT_PARTS_R", "SR_LOADOUT_PARTS_A", "SR_LOADOUT_POWERS"};
        for (const char *k : keys) cfg_set(k, "");
        opt_save_soon();
    }
}
