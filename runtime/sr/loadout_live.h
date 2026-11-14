// sr-recomp: the live loadout (loadout_live.c). The parts a test can run without the game: which fields of the game's
// loadout object the settings own, and what to write when the game or the settings change them.
#ifndef SR_LOADOUT_LIVE_H
#define SR_LOADOUT_LIVE_H
#include <stdint.h>

enum { LIVE_WEAPON_POWERS = 3, LIVE_RELIC_POWERS = 5, LIVE_GADGETS = 4, LIVE_PARTS = 9 };
// tank, weapon and its level, the powers' (type, level) pairs, and per gadget: type, "empty", two counts and the parts
enum { LIVE_FIELDS = 3 + 2 * LIVE_WEAPON_POWERS + 2 * LIVE_RELIC_POWERS + LIVE_GADGETS * (4 + LIVE_PARTS) };
enum { LIVE_TABLE_WEAPON, LIVE_TABLE_GADGET, LIVE_TABLE_POWER, LIVE_TABLES };

// The game's name tables: the number the object holds for a name of a table, -1 when the game has no such name.
typedef struct LiveNames {
    int (*index_of)(void *ctx, int table, const char *name);
    void *ctx;
} LiveNames;

typedef struct LiveState {
    int seen, settling, dirty;      // dirty: a setting changed and the target is not written yet
    uint32_t game[LIVE_FIELDS];     // the game's own values, as it last filled the object
    uint32_t target[LIVE_FIELDS];   // what the object should hold: the game's with the settings over them
    uint32_t settle[LIVE_FIELDS];   // a change by the game, waiting for a second look
} LiveState;

uint32_t loadout_live_offset(int field);   // a field's offset in the loadout object
int loadout_live_wanted(void);
int loadout_live_compose(const LiveNames *names, const uint32_t *game, uint32_t *target);
int loadout_live_step(LiveState *st, const LiveNames *names, const uint32_t *now, int fresh, uint32_t *out);
void loadout_live_start(void);
#endif
