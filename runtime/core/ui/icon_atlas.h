// sr-recomp: the game's own icons in the menu (ui/icon_atlas.cpp): read from the player's dump when a page first asks
// for one, decoded on a thread of their own and kept in two textures of the menu (small and large). Nothing of them is stored on disk.
// Present path only (ImGui, Vulkan).
#pragma once
#include "imgui.h"

extern "C" {
#include "ui/menu.h"
}

// The icon of a RomFS picture ("UI/Tex/Weapon/Shooter_B.bntx.zs"): true and where it is in the texture once it is
// ready; false while it is read (a later frame has it) or when the dump has none.
bool icon_get(const char *path, ImTextureID *tex, ImVec2 *uv0, ImVec2 *uv1);
// The same at the game's own size (512 pixels), for an icon drawn large; a few at a time.
bool icon_get_large(const char *path, ImTextureID *tex, ImVec2 *uv0, ImVec2 *uv1);
// Once a frame the menu lays out, before it draws: the texture made, and the icons decoded since the last frame copied
// into it. 0 when the texture cannot be made (the pages then show no icons).
int icons_frame(const MenuTarget *t);
// The texture goes (the menu's Vulkan back end is shut down); the decoded icons are read again when asked for.
void icons_release(void);
