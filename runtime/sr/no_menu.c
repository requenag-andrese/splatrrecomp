// The null profile has no window and no Vulkan, so it has no menu either: ui/menu.h answered as "no menu" (it never
// opens and takes no input) for the files of that profile that call it. The Vulkan profile builds the real menu,
// runtime/core/ui/menu.cpp (totk-pc-recomp's, with SR's rows), and its performance overlay.
#ifdef SR_NULL_PROFILE
#include <stdint.h>

#include "../core/ui/menu.h"

int menu_is_open(void) { return 0; }
void menu_toggle(void) {}
void menu_perf_cycle(void) {}
void menu_screenshot_request(void) {}
void menu_event_key(int key, int down) { (void)key, (void)down; }
void menu_event_mouse_pos(float x, float y) { (void)x, (void)y; }
void menu_event_mouse_button(int button, int down) { (void)button, (void)down; }
void menu_event_wheel(float dx, float dy) { (void)dx, (void)dy; }
void menu_event_focus(int focused) { (void)focused; }
void menu_vk_release(void) {}
int menu_font_check(void) { return 1; }
#endif
