// sr-recomp: the game's own fonts and menu pictures, read from the player's dump at run time (game_art.c). Nothing of them
// is stored in this repository or next to sr.exe. Every call answers 0 or NULL when the dump cannot give them, and the
// menu then keeps its built-in look.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// An OpenType font in memory, ready for a font loader; free() it. face: "scft/<name>.bfotf" in the game's font archive.
// Its line metrics are set to the font's em square: the originals hold room for tall scripts, which makes Latin text come
// out small at a given line height.
uint8_t *game_art_font(const char *name, size_t *size);

// A RomFS file, zstd unpacked (a .zs file); free() it. sr-recomp: the loadout tab's names (sr/loadout_names.c).
uint8_t *game_art_read(const char *path, size_t *out);
// One icon of the game (a .bntx.zs of UI/Tex with an ASTC picture) as size x size RGBA, fitted and centred; free() it.
// NULL when the dump has no such file. sr-recomp: the loadout tab (ui/icon_atlas.cpp).
uint8_t *game_art_icon(const char *path, int size);

// Pictures of the Boot layout archive. _load unpacks that archive (175 MB, once, then dropped) and decodes the named
// pictures; they stay until _free. NULL names end the list. channels: 1 for a BC4 mask, 4 (RGBA) for an ASTC picture.
typedef struct { const char *name; int w, h, channels; uint8_t *px; } GameMask;
int game_art_masks_load(const char *const *names);
const GameMask *game_art_mask(const char *name);
void game_art_masks_free(void);

#ifdef __cplusplus
}
#endif
