// sr-recomp: the Loadout tab of the menu (ui/loadout_page.cpp): the player's loadout, chosen by the game's own names
// and put on at once by sr/loadout_live.c. Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end.
void loadout_rows(float scale);
// The parts list of a gadget is open (or B / Esc closed it this frame): B, Esc and the tab keys stay on the page.
bool loadout_modal(void);
