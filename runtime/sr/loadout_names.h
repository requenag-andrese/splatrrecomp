// sr-recomp: what the loadout tab of the menu offers, with the names the game shows (sr/loadout_names.c). Every name
// is read from the player's dump, in the language the game runs in: the game's text archive
// (Mals/<language>.Product.<version>.sarc.zs: Glossary/WeaponName, GadgetName, RelicPower, ...), the list of gadget part
// files (Gyml/GadgetParts) and the gadget table (RSDB/CoopGadgetInfo). None of them is kept in this repository.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *id, *name; } LoName;   // id: the game's internal name (what sr-config.ini takes)
typedef struct {
    const char *id, *name, *desc;   // a part type ("Boom_Multi"), its name and what it does (desc may be NULL)
    const char *icon;               // its picture's name in UI/Tex/GadgetParts (lo_names_icons); NULL: none
    int n;
    const char **parts;             // the type's part files by level, lowest first ("Boom_11_Multi_01", ...)
} LoPartType;
typedef struct {
    const char *id, *name;
    int n_types;
    LoPartType *types;   // by the number in their files' names
} LoGadget;

typedef struct {
    LoName *weapons;             // A to F of every kind, by category and kind
    int *weapon_cat, n_weapons;  // weapon i's category: an index into cats
    LoName *cats;
    int n_cats;
    LoGadget *gadgets;           // the ones a gadget slot takes, by tank and the game's own order
    int n_gadgets;
    LoName *powers;              // the relic powers, then the ones only a weapon carries
    int n_powers, n_relic_powers;
    LoName tanks[4];             // ids as loadout_tank takes them: Default, Speed, Power, Tactical; a name may be NULL
    LoName buddies[3];           // Shiver, Frye, BigMan
} LoNames;

// The game's gadget table, one row per gadget: its id, the short name its parts' files start with, the tank it belongs
// to (0 Speed, 1 Power, 2 Tactical, 3 none: a Showstopper) and its place in the game's lists.
typedef struct { const char *id, *short_name; int tank, order; } LoGadgetRow;

// Loaded once, from the dump; NULL when the dump cannot give them (then the tab says so).
const LoNames *lo_names(void);
// The lists from the text archive (a SARC, unpacked), the part files' names (without folder or extension) and the
// gadget table. NULL when the archive is not one.
LoNames *lo_names_build(const uint8_t *sarc, size_t n, const char *const *parts, int n_parts, const LoGadgetRow *gadgets,
                        int n_gadgets);
// Each part type's icon among the names of the game's part pictures: its own name, else (the game misspells a few file
// names: "Foward") the longest unused picture of the same gadget whose name is in the type's in order.
void lo_names_icons(LoNames *names, const char *const *icons, int n_icons);
// The text archive of a Switch language code: "USen" for "en-US", ...; "USen" for one the game has no text for.
const char *lo_names_mals(const char *switch_code);

#ifdef __cplusplus
}
#endif
