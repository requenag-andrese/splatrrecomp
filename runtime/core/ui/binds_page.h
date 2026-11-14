// sr-recomp: totk-pc-recomp's runtime/ui/binds_page.h (https://github.com/valentderah/totk-pc-recomp @
// 0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3, MIT), with SR_ setting names and sr-config.ini in place of TOTK_ and totk.ini.
//
// The remapping rows of the menu's Controls tab (ui/binds_page.cpp). Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end: every action's two keys and its controller button, remapped by selecting a
// cell and pressing the new one, and the reset of them all. binds_after_page, after opt_end: the window that waits for
// the press.
void binds_rows(float scale);
void binds_after_page(void);
// A cell waits for a press (or its press is still reaching the menu): B, Esc, L / R and the tab keys stay on the page.
bool binds_modal(void);
