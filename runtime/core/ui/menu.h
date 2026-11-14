// The in-game menu (ui/menu.cpp, Dear ImGui) and its seams to the platform side: the window and its input (ui/menu_win.cpp
// on Windows, ui/menu_sdl.cpp with SDL), the gamepad (input_pad_read, input.h) and the present path
// (gfx_present.c). The menu core calls no window system: events come in through menu_event_*, the drawing happens in the
// present path's command buffer.
//
// F1 or Esc on the keyboard, the controller's Home, and Minus and Plus held together (consumed in poll_merged,
// input_menu_gate in input.h) open it;
// F1, Esc, B or "Continue" close it. While it is open the game gets no input (input_menu, input.h) and keeps running; the
// cursor is released from the mouse camera. Tabs Screen, Game, Mods, Sound, Controls and Menu (L / R or Q / E switch
// them) hold option rows (ui/option_rows.h) over the cfg settings and the mods folder (mods.h); the bottom row has
// "Continue" and "Quit game".
// Test hooks (ui/menu.cpp): SR_MENU_TEST=<frame>[:<frames per page>] opens the menu at that presented frame and saves
// one swapchain shot per tab into SR_MENU_TEST_DIR; SR_MENU_TEST_SET=<frame>:<key>=<value>[,...][;...] sets settings
// at given frames without input (SAVE=1 writes sr-config.ini). With SR_MENU_TEST_DIR set, the launcher (below) keeps
// launcher-now.png there up to date; with SR_SHOTS_DIR set, it saves every frame there in order.
//
// Threads: the event functions are called by the window thread, menu_toggle by whichever thread polls the pad, the
// overlay functions by the present path under its lock. ImGui itself runs on the present path only.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- state, any thread
int menu_is_open(void);
void menu_toggle(void);                   // F1 / Esc on the keyboard, Minus+Plus on the pad
void menu_perf_cycle(void);               // F10: the performance overlay off, fps, full
void menu_screenshot_request(void);       // the controller's Capture: the next frame goes to screenshots/ next to sr.exe
void menu_screenshot_to(const char *path);   // sr-recomp: the next frame to that PNG (F9 reports)
void menu_notice(const char *text, double seconds);   // sr-recomp: a line over the game for that long

// ---- events from the window adapter, used only while the menu is open. key: an ImGuiKey; coordinates in pixels of the
// window's client area, which is the swapchain image.
void menu_event_key(int key, int down);
void menu_event_char(unsigned utf16);   // sr-recomp: a typed character (a text field of the menu)
void menu_event_mouse_pos(float x, float y);
void menu_event_mouse_button(int button, int down);   // 0 left, 1 right, 2 middle
void menu_event_wheel(float dx, float dy);
void menu_event_focus(int focused);

// ---- the overlay, drawn into the swapchain image after the game image (gfx_present.c, present lock held)
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;                        // submissions take vk_queue_lock
    VkFormat format;                      // of the swapchain image, a colour attachment
    uint32_t width, height;
    int image_w, image_h;                 // the game image this present shows (its crop)
    int scaler;                           // how the last present scaled it to the window: 0 not, 1 bilinear blit, 2 FSR 1
} MenuTarget;
// Lays out this frame's overlay; 1 when there is something to draw. frame: the presented frame (0 unknown). *shot: a
// path when the test hook wants this frame's swapchain image saved, else NULL. The first open starts ImGui's Vulkan
// back end and uploads the font. With the menu closed it costs two loads and a compare.
int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot);
// Records the frame menu_overlay_frame laid out into cb, which is inside dynamic rendering on the image.
void menu_overlay_draw(VkCommandBuffer cb);
void menu_vk_release(void);               // before the device goes away (gfx_window_release_vk)
// The check the font atlas runs on a system font, on the embedded font and damaged copies of it: 1 when it takes the
// whole font and refuses a cut-off one, one without a character map and one with an unknown tag (--probe-window).
int menu_font_check(void);

// ---- the launcher (Windows): the menu's first page when sr.exe starts without arguments, before anything of the game is
// loaded (launcher_win.c). Its buttons: Play, Precompile shaders (a progress bar and Cancel in its place while that runs),
// Settings (the menu's tabs, Back returns) and Quit. The menu counts as open throughout, so the window's input reaches it.
// A setting that applies at start and changed here makes Play restart the process and go straight into the game.
#define LAUNCHER_RESTART_ENV "SR_RESTART_LAUNCHER"   // set for the new process by a restart from the launcher's settings
void menu_launcher_begin(void);
int menu_launcher_done(void);             // Play was pressed, or ImGui cannot draw the page: the game starts
void menu_launcher_end(void);

// The shader build behind the launcher's button (launcher_win.c).
enum { LAUNCH_WARM_NONE, LAUNCH_WARM_DONE, LAUNCH_WARM_CANCELLED, LAUNCH_WARM_FAILED, LAUNCH_WARM_NO_START };
typedef struct {
    int running, stopping, outcome;       // outcome: of the last build
    int stale;                            // the driver cache was not filled for this GPU and driver: worth a build
    uint64_t done, total, ms;
    unsigned long code;                   // the exit code, or the error that kept it from starting
    char detail[512];                     // the process's own line on a failure
} LauncherWarm;
const LauncherWarm *launcher_warm_state(void);
void launcher_warm_start(void);
void launcher_warm_cancel(void);          // any thread
void launcher_warm_tick(void);            // takes the progress over and notices the end

// ---- what the menu asks of the platform side (gfx_window_win.c, gfx_window_sdl.c)
void gfx_window_apply_settings(void);     // SR_FULLSCREEN or SR_WINDOW_SIZE changed: the window follows
void gfx_window_request_close(void);      // as the window's close button: the process ends
void gfx_window_request_restart(void);    // the same, then sr.exe starts again with the same command line
int gfx_window_monitor_size(int *w, int *h);   // pixels of the monitor the window is on; 0 when unknown
double gfx_window_refresh_hz(void);            // its refresh rate (SR_FPS_LIMIT=display); 0 when unknown
double gfx_window_desktop_hz(void);            // its desktop's rate (dynamic refresh rate); 0 when unknown
int gfx_window_composed(void);                 // the desktop composes the window for sure (no focus, exclusive lost)
unsigned gfx_window_dpi(void);                 // the window's DPI (96 at 100% display scale); any thread
// The present mode the swapchain got, as a SR_VK_PRESENT value ("fifo" when mailbox is missing); "" before the first
// swapchain. Any thread.
const char *gfx_window_present_mode(void);
// Frames a second frame interpolation can put on the screen (vk_interp.c): the refresh rate, or less when VSync lets
// fewer out (a window the desktop composes at 60). 0 when unknown.
double vk_interp_rate_hz(void);

#ifdef __cplusplus
}
#endif
