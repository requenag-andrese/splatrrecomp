// sr-recomp: totk-pc-recomp's runtime/ui/menu.cpp (https://github.com/valentderah/totk-pc-recomp @
// 0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3, MIT), with the tabs cut to the settings this port reads: no Mods or Cheats
// tab, no TotK-only rows (resolution, shadows, FOV, colour, camera speed, walk key, button prompts, Discord), and
// SR's own rows added (DLSS on and off live, its model, per-object motion vectors, the crash ring, a screenshot), and
// the Amiibo tab back (ui/amiibo_page.cpp, core/amiibo.c, sr/ov_nfp.c). The
// overlay shows SR's DLSS and frame generation state, and the version line the game file's version.
//
// The in-game menu core: Dear ImGui state, the event queue the window adapter fills, gamepad navigation, the Vulkan back
// end on our device and the menu's pages. Free of any window system (menu.h). ImGui runs on the present path only,
// under gfx_present.c's present lock; the event functions only queue.
//
// The Vulkan set-up follows LAS Recomp's runtime/nvn/vulkan/imgui_glue.cpp (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai): dynamic rendering into the swapchain
// image, an internal descriptor pool, vertex buffers rotated over more frames than are in flight.
//
// The look (colours, rounding, the tab bar with L / R and Q / E, the window over a dimmed game) follows
// SavageWindRecomp's runtime/ui/menu.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai); the pages are rows of option_rows.cpp.
#include <atomic>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, NavCursorVisible
#include "backends/imgui_impl_vulkan.h"
// A private copy of stb_truetype, the parser Dear ImGui builds its font atlas with, to check a system font before the
// atlas gets it (font_usable). Its asserts only mark the font bad here.
static int g_font_bad;
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((x) ? (void)0 : (void)(g_font_bad = 1))
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "imstb_truetype.h"
#pragma clang diagnostic pop
#include "ui/amiibo_page.h"
#include "ui/binds_page.h"
#include "ui/option_rows.h"
#include "game_art.h"

extern "C" {
#include "amiibo.h"
#include "cfg.h"
#include "fs.h"
#include "input.h"
#include "input_map.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "ui_text.h"
// host.h and vk.h pull in cpu.h, which is C only: the functions the menu needs from them, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
double frame_step_limit_hz(void);
extern const char rt_port_version[];
extern const char sr_display_version[];   // the game file's version (control.nacp), modules.c
int ink_color_parse(const char *s, float rgb[3]);   // sr/ink_color.c: "rrggbb" to 0..1 each
int vk_dlaa_available(void);              // sr/vk_dlss.c: DLSS loaded at start (dlaa = 1, an RTX GPU, nvngx_dlss.dll)
int vk_dlss_is_off(void);                 // switched off for now (F8, the Screen tab)
void vk_dlss_set_off(int off);
const char *vk_dlss_overlay_line(void);   // the overlay's DLSS line; NULL when DLSS is not loaded
const char *sl_fg_overlay_line(void);     // sr/vk_dlssg.cpp: frame generation, NULL when it is off
}

// Roboto Medium (Apache-2.0, ui/fonts/LICENSE-Roboto.txt) with Latin and Cyrillic glyphs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

// ---- open state, any thread
static std::atomic<int> g_open;     // the menu is open
static std::atomic<int> g_reset;    // opened or closed since the last laid-out frame: drop stale input

// g_open, the input block and the log line change together under open_lock, so they cannot disagree when the window thread
// (F1, Esc), the polling thread (combo) and the present path (Continue, B) race.
static PlatMutex open_lock = PLAT_MUTEX_INIT;

// The launcher (menu.h). While it runs the menu stays open: what closes the menu goes from its settings back to its first
// page, and does nothing on that page.
enum { LAUNCH_OFF, LAUNCH_HOME, LAUNCH_SETTINGS, LAUNCH_LOADING };
static std::atomic<int> g_launch;

// want 0 or 1 sets the state; -1 flips it. Returns the new state.
static int change_open(int want) {
    plat_mutex_lock(&open_lock);
    if (g_launch.load() != LAUNCH_OFF) {
        int settings = LAUNCH_SETTINGS;
        if (want != 1 && g_launch.compare_exchange_strong(settings, LAUNCH_HOME)) g_reset.store(1);
        plat_mutex_unlock(&open_lock);
        return 1;
    }
    int open = want < 0 ? !g_open.load() : want;
    if (g_open.load() != open) {
        g_open.store(open);
        g_reset.store(1);
        input_menu(open);
        rt_log("menu: %s", open ? "open" : "closed");
    }
    plat_mutex_unlock(&open_lock);
    return open;
}

static void set_open(int open) { change_open(open); }

static int g_failed_flag();
extern "C" int menu_is_open(void) { return g_open.load(std::memory_order_relaxed); }
extern "C" void menu_toggle(void) {
    if (!g_failed_flag()) change_open(-1);
}

// ---- events from the window adapter, queued while the menu is open; the present path hands them to ImGui
enum { EV_KEY, EV_POS, EV_BUTTON, EV_WHEEL, EV_FOCUS, EV_CHAR, EV_MAX = 256 };
struct Ev { int type, a, b; float x, y; };
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static Ev ev_q[EV_MAX];
static int ev_n;

static void push(Ev e) {
    if (!menu_is_open()) return;
    plat_mutex_lock(&ev_lock);
    if (e.type == EV_POS && ev_n && ev_q[ev_n - 1].type == EV_POS) ev_q[ev_n - 1] = e;   // only the newest position
    else if (ev_n < EV_MAX) ev_q[ev_n++] = e;
    plat_mutex_unlock(&ev_lock);
}

extern "C" void menu_event_key(int key, int down) { push({EV_KEY, key, down, 0, 0}); }
extern "C" void menu_event_char(unsigned utf16) { push({EV_CHAR, (int)utf16, 0, 0, 0}); }
extern "C" void menu_event_mouse_pos(float x, float y) { push({EV_POS, 0, 0, x, y}); }
extern "C" void menu_event_mouse_button(int button, int down) { push({EV_BUTTON, button, down, 0, 0}); }
extern "C" void menu_event_wheel(float dx, float dy) { push({EV_WHEEL, 0, 0, dx, dy}); }
extern "C" void menu_event_focus(int focused) { push({EV_FOCUS, focused, 0, 0, 0}); }

static void drain_events(ImGuiIO &io) {
    Ev q[EV_MAX];
    plat_mutex_lock(&ev_lock);
    int n = ev_n;
    for (int i = 0; i < n; i++) q[i] = ev_q[i];
    ev_n = 0;
    plat_mutex_unlock(&ev_lock);
    for (int i = 0; i < n; i++) {
        const Ev &e = q[i];
        switch (e.type) {
        case EV_KEY: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_POS: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case EV_FOCUS: io.AddFocusEvent(e.a != 0); break;
        case EV_CHAR: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
        }
    }
}

// ---- gamepad navigation: the Switch buttons of our SDL3 gamepad to ImGui's gamepad keys. Nintendo layout: A (east)
// activates, B (south) cancels.
static const struct { int bit; ImGuiKey key; } pad_keys[] = {
    {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_X, ImGuiKey_GamepadFaceUp},
    {B_Y, ImGuiKey_GamepadFaceLeft}, {B_L, ImGuiKey_GamepadL1}, {B_R, ImGuiKey_GamepadR1},
    {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft},
    {B_UP, ImGuiKey_GamepadDpadUp}, {B_RIGHT, ImGuiKey_GamepadDpadRight}, {B_DOWN, ImGuiKey_GamepadDpadDown},
};
static uint64_t pad_prev;   // buttons at the last frame; a button held when the menu opened counts once released
static int stick_dir = -1;  // the D-pad button the left stick holds, -1 none

// The left stick moves the cursor as the D-pad does (ImGui's own stick handling only scrolls the page): past half way
// it holds the D-pad direction of its larger axis, kept until that axis falls under 0.35; ImGui repeats a held one.
static int stick_to_dpad(int32_t lx, int32_t ly) {
    float x = lx / 32767.0f, y = ly / 32767.0f;   // y up
    float along = stick_dir == B_LEFT ? -x : stick_dir == B_RIGHT ? x : stick_dir == B_UP ? y : stick_dir == B_DOWN ? -y : 0;
    if (stick_dir >= 0 && along > 0.35f) return stick_dir;
    if (fmaxf(fabsf(x), fabsf(y)) < 0.5f) return -1;
    return fabsf(x) > fabsf(y) ? (x < 0 ? B_LEFT : B_RIGHT) : (y > 0 ? B_UP : B_DOWN);
}

static void pad_events(ImGuiIO &io, int reset) {
    uint64_t b;
    int32_t lx, ly;
    int have = input_pad_read(&b, &lx, &ly);
    if (have) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    stick_dir = stick_to_dpad(lx, ly);
    if (stick_dir >= 0) b |= 1ull << stick_dir;
    if (reset) {
        pad_prev = b;
        return;
    }
    for (const auto &k : pad_keys) {
        uint64_t m = 1ull << k.bit;
        if ((b ^ pad_prev) & m) io.AddKeyEvent(k.key, (b & m) != 0);
    }
    pad_prev = b;
}

// ---- ImGui and its Vulkan back end (present path)
static int g_ctx_ok;              // context made
static std::atomic<int> g_failed; // set-up failed for good (the menu stays closed)
static int g_failed_flag() { return g_failed.load(std::memory_order_relaxed); }
static int g_vk_ok;               // back end initialised for g_vk_fmt
static VkFormat g_vk_fmt;
static float g_font_scale;        // the scale of the uploaded font atlas; 0 when none is uploaded
static float g_style_scale;       // the scale the style was last sized for
static ImGuiStyle g_base_style;   // the menu's style at 1080 lines
static uint64_t g_last_ns;
static uint64_t g_cost_ns, g_cost_frames;   // CPU time of layout and recording while open, logged per 600 frames

// Sizes are tuned for 1080 lines and follow the window's height. Under 720 lines at 100% display scale they stay at the
// 720 size; that floor grows with the window's DPI, so a small window on a 150% or 200% display (sizes are physical
// pixels) still gets text as large as the display scale asks for.
static float menu_scale(uint32_t h) {
    float least = 720.0f * (float)gfx_window_dpi() / 96.0f;
    return ((float)h >= least ? (float)h : least) / 1080.0f;
}

// SavageWindRecomp's colours: dark green panels, gold accents.
static void style_init(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 22); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    // sr-recomp: the game's menu colours, sampled from its difficulty screen
    ImVec4 mint(0.33f, 0.82f, 0.68f, 1), teal(0.05f, 0.56f, 0.53f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.15f, 0.11f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.84f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.68f, 0.62f, 1);
    c[ImGuiCol_Button] = ImVec4(0.03f, 0.27f, 0.23f, 1);
    c[ImGuiCol_ButtonHovered] = teal;
    c[ImGuiCol_ButtonActive] = ImVec4(0.05f, 0.67f, 0.62f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.02f, 0.21f, 0.17f, 1);
    c[ImGuiCol_TabHovered] = teal;
    c[ImGuiCol_TabSelected] = ImVec4(0.05f, 0.47f, 0.44f, 1);
    c[ImGuiCol_TabSelectedOverline] = mint;
    c[ImGuiCol_NavCursor] = mint;
    c[ImGuiCol_Separator] = ImVec4(0.15f, 0.36f, 0.30f, 1);
}

static void make_context(void) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "sr_menu";
    style_init(g_base_style);
    ImGui::GetStyle() = g_base_style;
    g_ctx_ok = 1;
}

static void vk_down(void) {
    if (!g_vk_ok) return;
    ImGui_ImplVulkan_Shutdown();
    g_vk_ok = 0;
    g_font_scale = 0;
}

static int vk_up(const MenuTarget *t) {
    if (g_vk_ok && g_vk_fmt == t->format) return 1;
    if (g_vk_ok) {   // the swapchain changed format: pipelines for the new one
        vk_queue_lock();
        vkQueueWaitIdle(t->queue);
        vk_queue_unlock();
        vk_down();
    }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = t->instance;
    ii.PhysicalDevice = t->phys;
    ii.Device = t->device;
    ii.QueueFamily = t->queue_family;
    ii.Queue = t->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 4;   // vertex buffers rotate per recorded frame: more than gfx_present.c's 2 frames in flight
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    g_vk_fmt = t->format;
    ii.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_vk_fmt;
    if (!ImGui_ImplVulkan_Init(&ii)) return 0;
    g_vk_ok = 1;
    return 1;
}

// Roboto Medium (Latin, Cyrillic, dashes and quotes) at 26 px per 1080 lines, with the glyphs of the Japanese, Korean
// and Chinese language names merged in from a system font when there is one (each language is named in its own script).
// A catalog row also holds menu text (Simplified Chinese), so every string of the row goes into the builder: the merged
// font must cover the whole menu of that language, not only its name. Made again when the window height changes; the old
// texture goes after the queue is idle (CreateFontsTexture).
static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x2027, 0};
// The launcher's title: "Recomp" says this is the port, not the game itself.
static const char kLauncherTitle[] = "Splatoon Raiders Recomp";
static const ImWchar kTitleRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
static ImFont *g_font_title;   // the launcher's title
static ImFont *g_font_btn;     // the game's display face at button size; NULL without the game's fonts

// sr-recomp: the launcher's pictures, composed from the game's masks (game_art.h) into custom rects of the font atlas, so
// they need no texture code of their own. Only while the launcher runs: its end rebuilds the atlas without them.
enum { SQUID_FRAMES = 24 };
enum { INK_FRAMES = 9, WIPE_FRAMES = 20 };
enum { ART_GREEN, ART_TEAL, ART_RED, ART_BAND, ART_BG, ART_INK, ART_WIPE = ART_INK + INK_FRAMES, ART_SQUID = ART_WIPE + WIPE_FRAMES,
       ART_COUNT = ART_SQUID + SQUID_FRAMES };
static struct { ImVec2 uv0, uv1; int w, h; } g_art[ART_COUNT];
static bool g_art_want;   // the masks are loaded: the next atlas holds the pictures
static bool g_art_on;     // this atlas holds them
#define SQUID_FRAME(n) "ArrowAnim_" n "^_D", "ArrowAnim_Mask" n "^s"
static const char *const kArtMasks[] = {
    "TabCmn_A_00^s", "TabCmn_A_01^s", "TabCmn_A_02^s", "RoughDot_00^s", "PrintTexture^s", "CmnBootBG_00^r",
    "InkBtnM_00^_E", "InkBtnM_03^_E", "InkBtnM_04^_E", "InkBtnM_10^_E", "InkBtnM_11^_E", "InkBtnM_12^_E", "InkBtnM_13^_E", "InkBtnM_14^_E", "InkBtnM_15^_E",
    "SpiralWipeA_00^_J", "SpiralWipeA_01^_J", "SpiralWipeA_02^_J", "SpiralWipeA_03^_J", "SpiralWipeA_04^_J", "SpiralWipeA_05^_J", "SpiralWipeA_06^_J",
    "SpiralWipeA_07^_J", "SpiralWipeA_08^_J", "SpiralWipeA_09^_J", "SpiralWipeA_10^_J", "SpiralWipeA_11^_J", "SpiralWipeA_12^_J", "SpiralWipeA_13^_J",
    "SpiralWipeA_14^_J", "SpiralWipeA_15^_J", "SpiralWipeA_16^_J", "SpiralWipeA_17^_J", "SpiralWipeA_18^_J", "SpiralWipeA_19^_J",
    "CursorIkaEyes_00^w", SQUID_FRAME("00"), SQUID_FRAME("01"), SQUID_FRAME("02"), SQUID_FRAME("03"), SQUID_FRAME("04"), SQUID_FRAME("05"),
    SQUID_FRAME("06"), SQUID_FRAME("07"), SQUID_FRAME("08"), SQUID_FRAME("09"), SQUID_FRAME("10"), SQUID_FRAME("11"), SQUID_FRAME("12"),
    SQUID_FRAME("13"), SQUID_FRAME("14"), SQUID_FRAME("15"), SQUID_FRAME("16"), SQUID_FRAME("17"), SQUID_FRAME("18"), SQUID_FRAME("19"),
    SQUID_FRAME("20"), SQUID_FRAME("21"), SQUID_FRAME("22"), SQUID_FRAME("23"), nullptr};
// The game's cursor loop (Cursor_00_Loop): the 35-frame time at which each of its 24 pictures starts.
static const uint8_t kSquidAt[SQUID_FRAMES] = {0, 1, 2, 3, 4, 6, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 26, 27, 28, 29, 30, 32, 34};

static void art_at4(const GameMask *m, float u, float v, float out[4]) {   // an RGBA picture, nearest, 0 outside it
    if (u < 0 || v < 0 || u >= 1 || v >= 1) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    const uint8_t *p = m->px + ((size_t)(v * m->h) * m->w + (size_t)(u * m->w)) * 4;
    for (int i = 0; i < 4; i++) out[i] = p[i] / 255.0f;
}

static float art_at(const GameMask *m, float u, float v) {   // bilinear, u and v in 0..1, clamped
    float x = fminf(fmaxf(u, 0), 1) * (m->w - 1), y = fminf(fmaxf(v, 0), 1) * (m->h - 1);
    int x0 = (int)x, y0 = (int)y, x1 = x0 + 1 < m->w ? x0 + 1 : x0, y1 = y0 + 1 < m->h ? y0 + 1 : y0;
    float fx = x - x0, fy = y - y0;
    const uint8_t *p = m->px;
    float a = p[y0 * m->w + x0] * (1 - fx) + p[y0 * m->w + x1] * fx, b = p[y1 * m->w + x0] * (1 - fx) + p[y1 * m->w + x1] * fx;
    return (a * (1 - fy) + b * fy) / 255.0f;
}

static float art_tile(const GameMask *m, float x, float y) {   // repeated, one pixel of the mask per unit
    int ix = (int)x % m->w, iy = (int)y % m->h;
    return m->px[iy * m->w + ix] / 255.0f;
}

// A torn board: the game's three slices, stretched in the middle.
static float art_board_alpha(float x, float y, float w, float h) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    float v = y / h;
    if (x < h) return art_at(game_art_mask("TabCmn_A_00^s"), x / h, v);
    if (x >= w - h) return art_at(game_art_mask("TabCmn_A_01^s"), (x - (w - h)) / h, v);
    return art_at(game_art_mask("TabCmn_A_02^s"), (x - h) / (w - 2 * h), v);
}

static uint32_t art_rgba(float r, float g, float b, float a) {
    auto byte = [](float v) { return (uint32_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f); };
    return IM_COL32(byte(r), byte(g), byte(b), byte(a));
}

// Colour over the shadow's colour: both with their own alpha, straight
static uint32_t art_over(const float c[3], float ca, float sa) {
    float a = ca + sa * (1 - ca);
    if (a <= 0) return 0;
    float k = ca / a;
    return art_rgba(c[0] * k + 0.02f * (1 - k), c[1] * k + 0.05f * (1 - k), c[2] * k + 0.04f * (1 - k), a);
}

// Colour a to b across, the game's grain over it and its rings a little lighter, on the shadow at (sx, sy) below.
static void art_bake_board(uint32_t *d, int stride, int w, int h, int sx, int sy, const float a[3], const float b[3], float scale) {
    const GameMask *grain = game_art_mask("PrintTexture^s"), *rings = game_art_mask("RoughDot_00^s");
    float bw = (float)(w - sx), bh = (float)(h - sy);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float al = art_board_alpha((float)x, (float)y, bw, bh), sa = 0.7f * art_board_alpha((float)(x - sx), (float)(y - sy), bw, bh);
            float t = fminf(1, (float)x / bw * 0.85f + (float)y / bh * 0.15f);
            float g = art_tile(grain, x / scale, y / scale), r = art_tile(rings, x / scale, y / scale);
            float c[3];
            for (int i = 0; i < 3; i++) c[i] = (a[i] + (b[i] - a[i]) * t) * (0.80f + 0.32f * g) + 0.035f * r;
            d[y * stride + x] = art_over(c, al, sa);
        }
}

// An RGBA picture reduced to (w, h), four samples a pixel, its shape kept for tinting: white at the rim, a little darker
// where the game's ink is dark inside. keep 0 (the wipe): plain white in its shape.
static void art_bake_tintable(uint32_t *d, int stride, int w, int h, const GameMask *m, float dark) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float lum = 0, al = 0;
            for (int s = 0; s < 4; s++) {
                float p[4];
                art_at4(m, (x + 0.25f + 0.5f * (s & 1)) / w, (y + 0.25f + 0.5f * (s >> 1)) / h, p);
                lum += p[0] * p[3], al += p[3];
            }
            const float f = al > 0 ? lum / al : 1;
            const float v = 1 - dark * (1 - f);
            d[y * stride + x] = art_rgba(v, v, v, al / 4);
        }
}

// The band under the boards, white for tinting: torn at the top and the bottom, fading out at both ends, grainy.
static void art_bake_band(uint32_t *d, int stride, int w, int h, float scale) {
    const GameMask *m = game_art_mask("TabCmn_A_02^s"), *grain = game_art_mask("PrintTexture^s");
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float u = (float)x / w, e = fminf(u, 1 - u) / 0.14f;
            e = fminf(e, 1);
            e = e * e * (3 - 2 * e);
            float g = art_tile(grain, x / scale, y / scale);
            d[y * stride + x] = art_rgba(1, 1, 1, art_at(m, u, (float)y / h) * e * (0.72f + 0.28f * g));
        }
}

// One picture of the cursor loop in the game's layers (Cursor_00): its mask as the shadow, the white squid in mint, and the
// eyes sliding round the head, clipped by the mask. In the picture's 96 layout units the shadow lies at (6, 4).
static void art_bake_squid(uint32_t *d, int stride, int size, int frame) {
    char name[32];
    snprintf(name, sizeof name, "ArrowAnim_%02d^_D", frame);
    const GameMask *body = game_art_mask(name);
    snprintf(name, sizeof name, "ArrowAnim_Mask%02d^s", frame);
    const GameMask *mask = game_art_mask(name), *eyes = game_art_mask("CursorIkaEyes_00^w");
    const float t = kSquidAt[frame];
    const float slide = t <= 10 ? 0 : t <= 20 ? 0.778f * (t - 10) / 10 : t < 23 ? 0.778f : -0.78f * (35 - t) / 12;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size, b[4], e[4];
            art_at4(body, u, v, b);
            float m = art_at(mask, u, v), sa = 0.5f * art_at(mask, u - 6 / 96.0f, v - 4 / 96.0f);
            float c[3] = {0.33f * b[0], 0.82f * b[1], 0.68f * b[2]};
            art_at4(eyes, (u - 0.5f - slide * 0.36f) / 0.37f + 0.5f, (v - 0.385f) / 0.215f + 0.5f, e);
            const float ea = e[3] * m;
            for (int i = 0; i < 3; i++) c[i] = c[i] * (1 - ea) + e[i] * ea;
            d[y * stride + x] = art_over(c, fminf(1, b[3] + ea), sa);
        }
}

static void art_bake_mask(uint32_t *d, int stride, int w, int h, const GameMask *m) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) d[y * stride + x] = art_rgba(1, 1, 1, art_at(m, (float)x / w, (float)y / h));
}
static const struct { const char *lang; UiTextId name; } kCjk[] = {
    {"ja", UI_MENU_LANGNAME_JA}, {"ko", UI_MENU_LANGNAME_KO}, {"zh-Hans", UI_MENU_LANGNAME_ZH_HANS},
    {"zh-Hant", UI_MENU_LANGNAME_ZH_HANT},
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// 1 when Dear ImGui can take this font file: more than the 100 bytes AddFontFromMemoryTTF asserts on, every table of its
// directory inside the file (stb_truetype reads tables without bounds checks, so a cut-off file faults when the glyphs
// are made), and stb_truetype parses it. ImGui asserts on a font it cannot parse, and an assert ends the process with
// 0xc0000409 and no line in the log.
static int font_usable(const uint8_t *d, uint64_t n) {
    if (!d || n <= 100 || n > INT_MAX) return 0;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || (uint64_t)off + 12 > n) return 0;
    uint32_t tables = (uint32_t)d[off + 4] << 8 | d[off + 5];
    if ((uint64_t)off + 12 + 16ull * tables > n) return 0;
    for (uint32_t i = 0; i < tables; i++) {
        const uint8_t *r = d + off + 12 + 16 * i;
        if ((uint64_t)be32(r + 8) + be32(r + 12) > n) return 0;
    }
    stbtt_fontinfo fi;
    g_font_bad = 0;
    return stbtt_InitFont(&fi, d, off) && !g_font_bad;
}

extern "C" int menu_font_check(void) {
    uint64_t n = sizeof roboto_ttf;
    uint8_t *bad = (uint8_t *)malloc(n);
    if (!bad) return 0;
    memcpy(bad, roboto_ttf, n);
    uint32_t tables = (uint32_t)bad[4] << 8 | bad[5];
    for (uint32_t i = 0; i < tables; i++)
        if (!memcmp(bad + 12 + 16 * i, "cmap", 4)) memset(bad + 12 + 16 * i, 0, 4);   // no character map
    int ok = font_usable(roboto_ttf, n) && !font_usable(roboto_ttf, 4096) && !font_usable(roboto_ttf, 100) &&
             !font_usable(bad, n);
    memset(bad, 0, 4);             // an unknown font tag
    ok = ok && !font_usable(bad, n);
    free(bad);
    return ok;
}

// The glyphs of every string of a catalog row (ui_text_visit), for the font that draws that language.
static void add_row_glyphs(void *ctx, const char *text) {
    static_cast<ImFontGlyphRangesBuilder *>(ctx)->AddText(text);
}

static int fonts_up(float scale) {
    enum { N = sizeof kCjk / sizeof *kCjk };
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    // sr-recomp: the game's own faces from the player's dump, Roboto behind them for what they lack and in their place
    // without a dump. SpMain is the game's text face, NinDev-Treasure its titles and buttons.
    size_t main_n = 0, face_n = 0;
    uint8_t *main_font = game_art_font("SpMain", &main_n), *face_font = game_art_font("NinDev-Treasure_00", &face_n);
    if (main_font && !font_usable(main_font, main_n)) free(main_font), main_font = nullptr;
    if (face_font && !font_usable(face_font, face_n)) free(face_font), face_font = nullptr;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    if (main_font) {
        fa->AddFontFromMemoryTTF(main_font, (int)main_n, 27.0f * scale, &fc, kRanges);
        fc.MergeMode = true;
    }
    fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 26.0f * scale, &fc, kRanges);
    uint8_t *data[N] = {};
    ImVector<ImWchar> ranges[N];
    int merged = 0;
    for (int i = 0; i < N; i++) {
        char path[512];
        uint64_t size = 0;
        if (!plat_font_path(kCjk[i].lang, path, sizeof path) || !(data[i] = plat_file_read(path, &size))) continue;
        if (!font_usable(data[i], size)) {
            rt_log("menu: system font %s left out: the font parser cannot read it (%llu bytes)", path,
                   (unsigned long long)size);
            free(data[i]);
            data[i] = NULL;
            continue;
        }
        merged++;
        ImFontGlyphRangesBuilder b;
        b.AddText(ui_text_in("en", kCjk[i].name));
        ui_text_visit(kCjk[i].lang, add_row_glyphs, &b);
        b.BuildRanges(&ranges[i]);
        ImFontConfig mc;
        mc.MergeMode = true;
        mc.FontDataOwnedByAtlas = false;
        fa->AddFontFromMemoryTTF(data[i], (int)size, 26.0f * scale, &mc, ranges[i].Data);
    }
    ImFontConfig tc;   // after the merges: they go into the last font added
    tc.FontDataOwnedByAtlas = false;
    g_font_title = face_font ? fa->AddFontFromMemoryTTF(face_font, (int)face_n, 68.0f * scale, &tc, kTitleRanges)
                             : fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, 68.0f * scale, &tc, kTitleRanges);
    g_font_btn = face_font ? fa->AddFontFromMemoryTTF(face_font, (int)face_n, 40.0f * scale, &tc, kTitleRanges) : nullptr;
    // sr-recomp: the launcher's pictures. Boards and the blob are made at their largest size on screen.
    g_art_on = false;
    int rect[ART_COUNT] = {};
    const int shadow = (int)(9 * scale);
    if (g_art_want) {
        int size[ART_COUNT][2] = {{(int)(530 * scale) + shadow, (int)(176 * scale) + shadow},
                                  {(int)(530 * scale) + shadow, (int)(176 * scale) + shadow},
                                  {(int)(530 * scale) + shadow, (int)(176 * scale) + shadow},
                                  {(int)(1000 * scale), (int)(120 * scale)},
                                  {512, 256}};
        for (int i = ART_INK; i < ART_WIPE; i++) size[i][0] = (int)(480 * scale), size[i][1] = (int)(368 * scale);
        for (int i = ART_WIPE; i < ART_SQUID; i++) size[i][0] = 902, size[i][1] = 508;   // the whole window, stretched: not smaller
        for (int i = ART_SQUID; i < ART_COUNT; i++) size[i][0] = size[i][1] = (int)(132 * scale);   // the loop's stretch at most
        fa->TexDesiredWidth = 4096;
        for (int i = 0; i < ART_COUNT; i++) {
            rect[i] = fa->AddCustomRectRegular(size[i][0], size[i][1]);
            g_art[i].w = size[i][0], g_art[i].h = size[i][1];
        }
        unsigned char *px = nullptr;
        int tw = 0, th = 0;
        if (fa->Build() && (fa->GetTexDataAsRGBA32(&px, &tw, &th), px)) {
            static const float kColour[3][2][3] = {{{0.29f, 0.66f, 0.22f}, {0.31f, 0.70f, 0.24f}},
                                                   {{0.17f, 0.56f, 0.56f}, {0.20f, 0.60f, 0.62f}},
                                                   {{0.55f, 0.08f, 0.16f}, {0.68f, 0.28f, 0.14f}}};
            // the Select animation's pictures, in its order (BtnSequence_00_Select)
            static const char *const kInk[INK_FRAMES] = {"InkBtnM_00^_E", "InkBtnM_03^_E", "InkBtnM_04^_E", "InkBtnM_10^_E", "InkBtnM_11^_E",
                                                         "InkBtnM_12^_E", "InkBtnM_13^_E", "InkBtnM_14^_E", "InkBtnM_15^_E"};
            for (int i = 0; i < ART_COUNT; i++) {
                const ImFontAtlasCustomRect *r = fa->GetCustomRectByIndex(rect[i]);
                uint32_t *d = (uint32_t *)px + (size_t)r->Y * tw + r->X;
                if (i <= ART_RED) art_bake_board(d, tw, r->Width, r->Height, shadow, shadow, kColour[i][0], kColour[i][1], scale);
                else if (i == ART_BAND) art_bake_band(d, tw, r->Width, r->Height, scale);
                else if (i >= ART_SQUID) art_bake_squid(d, tw, r->Width, i - ART_SQUID);
                else if (i >= ART_WIPE) {
                    char wipe[32];
                    snprintf(wipe, sizeof wipe, "SpiralWipeA_%02d^_J", i - ART_WIPE);
                    art_bake_tintable(d, tw, r->Width, r->Height, game_art_mask(wipe), 0);
                } else if (i >= ART_INK) art_bake_tintable(d, tw, r->Width, r->Height, game_art_mask(kInk[i - ART_INK]), 0.2f);
                else art_bake_mask(d, tw, r->Width, r->Height, game_art_mask("CmnBootBG_00^r"));
                fa->CalcCustomRectUV(r, &g_art[i].uv0, &g_art[i].uv1);
            }
            g_art_on = true;
        }
    }
    vk_queue_lock();   // the upload submits on the queue and waits for it
    int ok = ImGui_ImplVulkan_CreateFontsTexture();
    vk_queue_unlock();
    fa->ClearInputData();   // the glyphs are in the texture: the font files and the pixels are not needed again
    fa->ClearTexData();
    fa->TexDesiredWidth = 0;
    for (uint8_t *d : data) free(d);
    free(main_font);
    free(face_font);
    rt_log("menu: font atlas at %.1f px, language names from %d of %d system fonts", 26.0f * scale, merged, (int)N);
    g_font_scale = ok ? scale : 0;
    return ok;
}

extern "C" void menu_vk_release(void) {
    if (g_ctx_ok) vk_down();
}

// ---- test hooks
// SR_MENU_TEST=<frame>[:<frames per page>]: at that presented frame the menu opens on its first tab; each tab stays
// <frames per page> frames (default 30) and its last frame's swapchain image goes to SR_MENU_TEST_DIR (default: the
// working directory) as menu-test-<frame>-<page>.png; after the last tab the menu closes.
// SR_MENU_TEST_SET=<frame>:<key>=<value>[,<key>=<value>...][;<frame>:...]: cfg_set of each key (SR_ prefix
// optional) at that presented frame; changes live settings with no input, menu open or not. sr-config.ini is written only by
// the pseudo-key SAVE=1 (cfg_save, as a row change does; a failed write shows the menu's save note).
enum { TAB_SCREEN, TAB_GAME, TAB_INK, TAB_SOUND, TAB_CONTROLS, TAB_AMIIBO, TAB_MENU, TAB_ADVANCED, TAB_COUNT };
enum { TEST_PAGE_FRAMES = 30, TEST_SETS = 16 };
struct TestSet { uint64_t frame; char key[64], value[64]; };
static struct {
    int parsed;
    uint64_t at;          // SR_MENU_TEST frame; 0 none
    uint64_t per_page;
    uint64_t opened;      // the frame the hook opened the menu; 0 not yet
    int page;             // the tab being shot; -1 none
    TestSet sets[TEST_SETS];
    int n_sets, next_set; // SR_MENU_TEST_SET, in frame order
} g_test = {0, 0, 0, 0, -1, {}, 0, 0};

static void test_parse(void) {
    g_test.parsed = 1;
    if (const char *e = cfg_get("SR_MENU_TEST")) {
        long long at = atoll(e), per = 0;
        if (const char *c = strchr(e, ':')) per = atoll(c + 1);
        g_test.at = at > 0 ? (uint64_t)at : 0;
        g_test.per_page = per > 0 ? (uint64_t)per : TEST_PAGE_FRAMES;
    }
    const char *s = cfg_get("SR_MENU_TEST_SET");
    while (s && *s) {   // groups "<frame>:<key>=<value>,..." separated by ';'
        char *end;
        unsigned long long f = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        while (*s && *s != ';' && g_test.n_sets < TEST_SETS) {
            size_t n = strcspn(s, ",;");
            const char *eq = (const char *)memchr(s, '=', n);
            if (eq) {
                TestSet &t = g_test.sets[g_test.n_sets++];
                t.frame = f;
                int kn = (int)(eq - s), vn = (int)(s + n - eq - 1);
                snprintf(t.key, sizeof t.key, "%s%.*s", strncmp(s, "SR_", 3) ? "SR_" : "", kn, s);
                snprintf(t.value, sizeof t.value, "%.*s", vn, eq + 1);
            }
            s += n;
            if (*s == ',') s++;
        }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
    }
    if (g_test.at || g_test.n_sets)
        rt_log("menu: test hook: open at frame %llu, %llu frames per page, %d settings changes",
               (unsigned long long)g_test.at, (unsigned long long)g_test.per_page, g_test.n_sets);
}

static void test_apply_sets(uint64_t frame) {
    for (; g_test.next_set < g_test.n_sets && g_test.sets[g_test.next_set].frame <= frame; g_test.next_set++) {
        const TestSet &t = g_test.sets[g_test.next_set];
        rt_log("menu: test hook: frame %llu: %s=%s", (unsigned long long)frame, t.key, t.value);
        if (!strcmp(t.key, "SR_SAVE")) rt_log("menu: test hook: cfg_save %s", cfg_save() ? "wrote" : "failed");
        else cfg_set(t.key, t.value);
    }
}

// ---- the lists the Screen and Game tabs build
enum { MAX_GPUS = 8 };
static char g_gpu_names[MAX_GPUS][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int g_gpu_n = -1;   // -1: not listed yet

// The Vulkan devices in vkEnumeratePhysicalDevices order, which SR_GPU_INDEX counts in (vk_device.c pick_device).
static void list_gpus(VkInstance inst) {
    if (g_gpu_n >= 0) return;
    VkPhysicalDevice pd[MAX_GPUS];
    uint32_t n = MAX_GPUS;
    if (vkEnumeratePhysicalDevices(inst, &n, pd) < 0) n = 0;   // VK_INCOMPLETE keeps the first MAX_GPUS
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        snprintf(g_gpu_names[i], sizeof g_gpu_names[i], "%s", p.deviceName);
    }
    g_gpu_n = (int)n;
}

// Window sizes: the common 16:9 sizes that fit the monitor, the 1920x1080 default and the current size.
static const int kSizes[][2] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800},
                                {3840, 2160}};
enum { MAX_SIZES = sizeof kSizes / sizeof *kSizes + 1 };

static void row_window_size(void) {
    int sz[MAX_SIZES][2], n = 0, mw = 0, mh = 0, cw = 0, ch = 0;
    int have = gfx_window_monitor_size(&mw, &mh);
    int cur_ok = sscanf(cfg_value("SR_WINDOW_SIZE"), "%dx%d", &cw, &ch) == 2;
    bool listed = false;
    for (size_t i = 0; i < sizeof kSizes / sizeof *kSizes; i++) {
        int w = kSizes[i][0], h = kSizes[i][1];
        bool cur = cur_ok && w == cw && h == ch;
        if (have && (w > mw || h > mh) && !cur && !(w == 1920 && h == 1080)) continue;
        listed |= cur;
        sz[n][0] = w, sz[n][1] = h, n++;
    }
    if (cur_ok && !listed) {   // the current size where its area puts it
        int at = n;
        while (at > 0 && (long)sz[at - 1][0] * sz[at - 1][1] > (long)cw * ch) at--;
        memmove(sz[at + 1], sz[at], sizeof sz[0] * (n - at));
        sz[at][0] = cw, sz[at][1] = ch, n++;
    }
    char vals[MAX_SIZES][24], shown[MAX_SIZES][32];
    const char *pv[MAX_SIZES], *pn[MAX_SIZES];
    for (int i = 0; i < n; i++) {
        snprintf(vals[i], sizeof vals[i], "%dx%d", sz[i][0], sz[i][1]);
        snprintf(shown[i], sizeof shown[i], "%d × %d", sz[i][0], sz[i][1]);   // U+00D7, the multiplication sign
        pv[i] = vals[i];
        pn[i] = shown[i];
    }
    opt_choice_list("SR_WINDOW_SIZE", UI_MENU_WINDOW_SIZE, pv, pn, n, UI_MENU_WINDOW_SIZE_DESC);
}

static void row_gpu(void) {
    const char *vals[MAX_GPUS + 1], *names[MAX_GPUS + 1];
    char idx[MAX_GPUS][4];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GPU_AUTO);
    for (int i = 0; i < g_gpu_n; i++) {
        snprintf(idx[i], sizeof idx[i], "%d", i);
        vals[i + 1] = idx[i];
        names[i + 1] = g_gpu_names[i];
    }
    opt_choice_list("SR_GPU_INDEX", UI_MENU_GPU, vals, names, g_gpu_n + 1, UI_MENU_GPU_DESC);
}

// The game language: one entry per Mals language file the running game build ships (ui_text_game_langs), each under
// its own name. A code that names no entry exactly (older "en", "fr-CA" on a build without it) selects the first entry
// of the same catalog language.
static void row_game_lang(void) {
    UiGameLang langs[16];
    int n = ui_text_game_langs(999, langs, 16);
    const char *vals[17], *names[17];
    vals[0] = "";
    names[0] = menu_text(UI_MENU_GAME_LANG_AUTO);
    const char *now = cfg_value("SR_LANG");
    int cur = *now ? -1 : 0;
    for (int i = 1; i <= n; i++) {
        vals[i] = langs[i - 1].code;
        names[i] = menu_text(langs[i - 1].name);
        if (*now && !strcmp(now, vals[i])) cur = i;
    }
    for (int i = 1; i <= n && *now && cur < 0; i++)
        if (!strcmp(ui_text_lang(now), ui_text_lang(vals[i]))) cur = i;
    opt_choice_list("SR_LANG", UI_MENU_GAME_LANG, vals, names, n + 1, UI_MENU_GAME_LANG_DESC, cur);
}

// The menu's name of a SR_VK_PRESENT mode (gfx_window_present_mode); NULL for another.
static const char *present_name(const char *mode) {
    static const struct { const char *mode; UiTextId name; } names[] = {
        {"mailbox", UI_MENU_PRESENT_MAILBOX}, {"fifo", UI_MENU_PRESENT_VSYNC}, {"immediate", UI_MENU_PRESENT_IMMEDIATE},
    };
    for (const auto &n : names)
        if (!strcmp(n.mode, mode)) return menu_text(n.name);
    return nullptr;
}

// "in use: VSync" under the Presentation value when the swapchain got another mode than the setting (a driver without
// mailbox gets fifo); NULL when they agree or before the first swapchain.
static const char *present_note(void) {
    const char *got = gfx_window_present_mode(), *name = present_name(got);
    if (!*got || !strcmp(got, cfg_value("SR_VK_PRESENT")) || !name) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PRESENT_IN_USE), name);
    return note;
}

// Under Frame interpolation when the screen holds the multiplier back, by the same rule as vk_interp.c wanted(): a game
// at 60 gets no more frames than the refresh rate shows, or than VSync was measured letting out. NULL when it is off or
// fits.
static const char *interp_note(void) {
    const char *e = cfg_value("SR_FRAME_INTERP");
    const int m = e && e[0] >= '2' && e[0] <= '4' ? e[0] - '0' : 0;
    const double hz = vk_interp_rate_hz();
    const int fits = hz > 0 ? (int)((hz + 3) / 60) : m;
    if (m < 2 || fits >= m) return nullptr;
    static char note[128];
    if (fits < 2) snprintf(note, sizeof note, menu_text(UI_SR_INTERP_NOTE_OFF), hz);
    else snprintf(note, sizeof note, menu_text(UI_SR_INTERP_NOTE), fits, hz);
    return note;
}

// ---- the tabs
// DLSS (sr/vk_dlss.c): SR_DLAA loads it at start. While it is loaded the row switches it on and off at once, as F8 does
// (and writes the setting, so the next start follows); started without it, turning it on takes a restart.
static void row_dlss(void) {
    bool live = vk_dlaa_available() != 0;
    bool on = live ? !vk_dlss_is_off() : cfg_bool("SR_DLAA") != 0;
    static char value[96];
    snprintf(value, sizeof value, "%s", menu_text(on ? UI_MENU_ON : UI_MENU_OFF));
    if (on && !live) snprintf(value, sizeof value, "%s (%s)", menu_text(UI_MENU_ON), menu_text(UI_MENU_AFTER_RESTART));
    // "##dlss", not the setting's name: the row is live while DLSS is loaded, so it takes no "after restart" mark
    if (opt_toggle_text("##dlss", menu_text(UI_MENU_DLAA), menu_text(UI_SR_DLSS_DESC), on, value)) {
        cfg_set("SR_DLAA", on ? "0" : "1");
        opt_save_soon();
        if (live) vk_dlss_set_off(on);
    }
}

static void tab_screen(float scale) {
    opt_begin("##screen", scale);
    opt_toggle("SR_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
    opt_toggle("SR_FULLSCREEN_EXCLUSIVE", UI_MENU_FULLSCREEN_EXCL, UI_MENU_FULLSCREEN_EXCL_DESC);
    row_window_size();
    static const UiTextId present[] = {UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE};
    opt_choice("SR_VK_PRESENT", UI_MENU_PRESENT, present, 3, UI_MENU_PRESENT_DESC, present_note());
    row_gpu();
    static const UiTextId perf[] = {UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL};
    opt_choice("SR_PERF_OVERLAY", UI_MENU_PERF_OVERLAY, perf, 3, UI_MENU_PERF_OVERLAY_DESC);
    opt_group(UI_MENU_GRAPHICS);
    static const UiTextId aspect[] = {UI_MENU_ASPECT_FIT, UI_MENU_ASPECT_STRETCH};
    opt_choice("SR_ASPECT", UI_MENU_ASPECT, aspect, 2, UI_MENU_ASPECT_DESC);
    static const char *const aniso[] = {"game", "2", "4", "8", "16"};
    const char *aniso_names[] = {menu_text(UI_MENU_ANISO_GAME), "2x", "4x", "8x", "16x"};
    opt_choice_list("SR_ANISO", UI_MENU_ANISO, aniso, aniso_names, 5, UI_MENU_ANISO_DESC);
    row_dlss();
    static const UiTextId preset[] = {UI_MENU_DLSS_PRESET_DEFAULT, UI_MENU_DLSS_PRESET_K, UI_SR_DLSS_PRESET_L,
                                      UI_SR_DLSS_PRESET_M};
    opt_choice("SR_DLSS_PRESET", UI_MENU_DLSS_PRESET, preset, 4, UI_SR_DLSS_PRESET_DESC);
    opt_toggle("SR_DLAA_OBJMV", UI_SR_DLSS_OBJMV, UI_SR_DLSS_OBJMV_DESC);
    static const char *const interp[] = {"off", "2", "3", "4"};
    const char *interp_names[] = {menu_text(UI_SR_INTERP_OFF), "2x", "3x", "4x"};
    opt_choice_list("SR_FRAME_INTERP", UI_SR_INTERP, interp, interp_names, 4, UI_SR_INTERP_DESC, -1, interp_note());
    opt_group(UI_MENU_HDR_GROUP);
    opt_toggle("SR_HDR", UI_MENU_HDR, UI_MENU_HDR_DESC);
    opt_slider("SR_HDR_PEAK_NITS", UI_MENU_HDR_PEAK, 400, 10000, UI_MENU_HDR_PEAK_DESC, 50, "%.0f");
    opt_slider("SR_HDR_GAME_NITS", UI_MENU_HDR_GAME, 80, 500, UI_MENU_HDR_GAME_DESC, 5, "%.0f");
    opt_slider("SR_HDR_UI_NITS", UI_MENU_HDR_UI, 80, 500, UI_MENU_HDR_UI_DESC, 5, "%.0f");
    opt_slider("SR_HDR_HUE", UI_MENU_HDR_HUE, 0, 100, UI_MENU_HDR_HUE_DESC, 5, "%.0f %%");
    opt_end();
}

// A confirmation is open (Controls: restart, Advanced: reset): B and Esc close it, not the menu; L / R stay on the page.
static bool g_modal;          // a confirmation is open this frame (each sets it; the menu clears it first)
static bool g_restart_ask;    // "Restart now" or the Restart button: the confirmation opens at the window's level
static const char *g_restart_back;   // the row the cursor goes back to when it is cancelled; NULL: none

// The note under the layout: Automatic with a gamepad connected, the layout it picked ("in use: Xbox"). NULL otherwise.
static const char *pad_layout_note(void) {
    int got = input_pad_layout();
    if (got < 0 || strcmp(cfg_value("SR_PAD_LAYOUT"), "auto")) return nullptr;
    static char note[96];
    snprintf(note, sizeof note, menu_text(UI_MENU_PAD_LAYOUT_IN_USE),
             menu_text(got == PAD_LAYOUT_XBOX ? UI_MENU_PAD_LAYOUT_XBOX : UI_MENU_PAD_LAYOUT_NINTENDO));
    return note;
}

// The confirmation of "Restart now" and of the bottom row's Restart, over the menu; Cancel has the cursor first. Restart
// writes sr-config.ini, then closes the game as Quit does and starts it again (gfx_window_request_restart). It waits
// while the game writes a save, and the game stays when sr-config.ini cannot be written, as the changed settings would
// not come back. Called once a frame at the menu window's level, where both open it.
static void restart_confirm(void) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##restart_confirm", nullptr,
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    g_modal = true;
    bool launcher = g_launch.load() != LAUNCH_OFF;
    ImGui::TextUnformatted(launcher ? launcher_text(UI_LAUNCHER_RESTART_CONFIRM) : menu_text(UI_MENU_RESTART_CONFIRM));
    bool saving = fs_save_busy() != 0;
    if (saving || !cfg_last_save_ok())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s",
                           menu_text(saving ? UI_MENU_RESTART_SAVING : UI_MENU_SAVE_FAILED));
    ImGui::Spacing();
    const char *yes = menu_text(UI_MENU_RESTART_YES), *no = menu_text(UI_MENU_RESET_NO);
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
    bool close = ImGui::Button(no, ImVec2(bw, 0));
    if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::BeginDisabled(saving);
    if (ImGui::Button(yes, ImVec2(bw, 0)) && cfg_save()) {
        if (launcher) plat_setenv(LAUNCHER_RESTART_ENV, "1");
        gfx_window_request_restart();
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        close = true;
    if (close) {
        ImGui::CloseCurrentPopup();
        if (g_restart_back) opt_focus_key(g_restart_back);   // back on the row it came from
    }
    ImGui::EndPopup();
}

static void tab_game(float scale) {
    opt_begin("##game", scale);
    // no frame limit row: the game steps 1/60 s per frame, so any other rate runs it fast or slow
    row_game_lang();
    static const UiTextId mode[] = {UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD};
    opt_choice("SR_OPERATION_MODE", UI_MENU_OPERATION_MODE, mode, 2, UI_MENU_OPERATION_MODE_DESC);
    opt_toggle("SR_ASSUME_FOCUS", UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC);
    if (opt_button("##restart", UI_MENU_RESTART, UI_SR_RESTART_DESC)) g_restart_ask = true, g_restart_back = "##restart";
    opt_end();
}

// ---- sr-recomp: the Ink tab (sr/ink_color.c): the player's and the enemies' ink, each the game's own colour or a
// custom one, kept as "rrggbb" in SR_INK_COLOR / SR_ENEMY_INK_COLOR. A custom colour is set three ways: a colour wheel
// and a hex code field for the mouse and the keyboard (neither takes the controller's cursor), and three stepper rows
// (hue, saturation, brightness: left / right, or a click on the bar) that a controller reaches.
struct InkEdit {
    const char *key;
    float hsv[3];     // what the rows and the wheel show, kept here so a grey or a black keeps its hue
    char last[8];     // the setting's value hsv came from ("": none yet)
    char custom[8];   // the colour "Custom" comes back to after "Game's own"
    char hex[7];      // the hex field's text while it is edited
};
static InkEdit g_ink[2] = {{"SR_INK_COLOR", {0, 0, 0}, "", "ff5a1f", ""}, {"SR_ENEMY_INK_COLOR", {0, 0, 0}, "", "7a3cff", ""}};

// Where a row's bar goes: over the last row drawn, between its label and its value.
static void ink_bar_rect(ImVec2 *a, ImVec2 *b) {
    ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    float w = hi.x - lo.x, h = hi.y - lo.y;
    *a = ImVec2(lo.x + w * 0.36f, lo.y + h * 0.3f);
    *b = ImVec2(lo.x + w * 0.68f, hi.y - h * 0.3f);
}

static ImU32 ink_u32(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
}

// The bar of channel c (0 hue, 1 saturation, 2 brightness) of a colour, with a mark at its value; c < 0: the colour.
static void ink_bar(const float hsv[3], int c) {
    ImVec2 a, b;
    ink_bar_rect(&a, &b);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (c < 0) {
        dl->AddRectFilled(a, b, ink_u32(hsv[0], hsv[1], hsv[2]), (b.y - a.y) * 0.3f);
    } else {
        const int parts = c == 0 ? 6 : 1;   // the hue bar goes round the six primaries
        for (int k = 0; k < parts; k++) {
            float t0 = (float)k / parts, t1 = (float)(k + 1) / parts, x0 = a.x + (b.x - a.x) * t0, x1 = a.x + (b.x - a.x) * t1;
            ImU32 l = c == 0 ? ink_u32(t0, 1, 1) : c == 1 ? ink_u32(hsv[0], 0, hsv[2]) : ink_u32(hsv[0], hsv[1], 0);
            ImU32 r = c == 0 ? ink_u32(t1, 1, 1) : c == 1 ? ink_u32(hsv[0], 1, hsv[2]) : ink_u32(hsv[0], hsv[1], 1);
            dl->AddRectFilledMultiColor(ImVec2(x0, a.y), ImVec2(x1, b.y), l, r, r, l);
        }
        float x = a.x + (b.x - a.x) * hsv[c];
        dl->AddRectFilled(ImVec2(x - 2, a.y - 3), ImVec2(x + 2, b.y + 3), IM_COL32(255, 255, 255, 255), 2);
        dl->AddRect(ImVec2(x - 3, a.y - 4), ImVec2(x + 3, b.y + 4), IM_COL32(0, 0, 0, 200), 2);
    }
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 160), c < 0 ? (b.y - a.y) * 0.3f : 0);
}

static void ink_hex_of(const float rgb[3], char out[8]) {
    snprintf(out, 8, "%02x%02x%02x", (int)lroundf(rgb[0] * 255), (int)lroundf(rgb[1] * 255), (int)lroundf(rgb[2] * 255));
}

// e.hsv from a colour, keeping the hue of a grey and the hue and saturation of a black (they have none of their own).
static void ink_take_rgb(InkEdit &e, const float rgb[3]) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], h, s, v);
    if (s > 0 && v > 0) e.hsv[0] = h;
    if (v > 0) e.hsv[1] = s;
    e.hsv[2] = v;
}

// The hex code field, drawn as a row: mouse and keyboard only. True when a whole code was typed or pasted.
static bool ink_hex_row(InkEdit &e, float scale) {
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.2f), pad = 16 * scale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 40), 9 * scale);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 38), 9 * scale);
    dl->AddText(ImVec2(p.x + pad, p.y + fs * 0.6f), IM_COL32(240, 236, 224, 255), menu_text(UI_SR_INK_HEX));
    float iw = fs * 4.6f, fh = ImGui::GetFrameHeight();
    ImVec2 at(p.x + w - pad - iw, p.y + (h - fh) * 0.5f);
    dl->AddText(ImVec2(at.x - fs * 0.7f, p.y + fs * 0.6f), IM_COL32(170, 170, 160, 255), "#");
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemWidth(iw);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::GetActiveID() != ImGui::GetID("##hex")) snprintf(e.hex, sizeof e.hex, "%s", e.last);   // not being edited
    // the filter drops a pasted '#'; six hexadecimal digits are a colour
    bool edited = ImGui::InputText("##hex", e.hex, sizeof e.hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopItemFlag();
    ImGui::SetCursorScreenPos(after);
    float rgb[3];
    if (!edited || !ink_color_parse(e.hex, rgb)) return false;
    ink_take_rgb(e, rgb);
    return true;
}

static void ink_rows(InkEdit &e, UiTextId label, UiTextId desc, float scale) {
    const char *now = cfg_get(e.key);
    float rgb[3];
    bool custom = ink_color_parse(now, rgb) != 0;
    if (custom) {
        char hex[8];
        ink_hex_of(rgb, hex);
        if (strcmp(hex, e.last)) {   // set outside these rows (the file, a reset, the first time): the rows take it
            snprintf(e.last, sizeof e.last, "%s", hex);
            snprintf(e.custom, sizeof e.custom, "%s", hex);
            ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], e.hsv[0], e.hsv[1], e.hsv[2]);
        }
    }
    char value[64];
    if (custom) snprintf(value, sizeof value, "%s  #%s", menu_text(UI_SR_INK_CUSTOM), e.last);
    else snprintf(value, sizeof value, "%s", menu_text(UI_SR_INK_GAME));
    int step = opt_text_row(e.key, menu_text(label), value, NULL, menu_text(desc), false);
    if (custom) ink_bar(e.hsv, -1);
    if (step) {   // any step switches between the game's own and the custom colour
        if (custom) cfg_reset(&e.key, 1);
        else cfg_set(e.key, e.custom);
        opt_save_soon();
        return;
    }
    if (!custom) return;
    bool changed = false;
    ImGui::PushID(e.key);
    // the wheel, as tall as the four rows beside it
    float row_h = floorf(ImGui::GetFontSize() * 2.2f), gap = ImGui::GetStyle().ItemSpacing.y, wheel = row_h * 4 + gap * 3;
    float picked[3];
    ImGui::ColorConvertHSVtoRGB(e.hsv[0], e.hsv[1], e.hsv[2], picked[0], picked[1], picked[2]);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetNextItemWidth(wheel);
    if (ImGui::ColorPicker3("##wheel", picked, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                                   ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha |
                                                   ImGuiColorEditFlags_NoLabel)) {
        ink_take_rgb(e, picked);
        changed = true;
    }
    ImGui::PopItemFlag();
    ImGui::SameLine();
    ImGui::BeginGroup();
    changed |= ink_hex_row(e, scale);
    static const struct { UiTextId label; float range, step; const char *fmt; } rows[3] = {
        {UI_SR_INK_HUE, 360, 5, "%.0f\xC2\xB0"}, {UI_SR_INK_SATURATION, 100, 5, "%.0f %%"}, {UI_SR_INK_BRIGHTNESS, 100, 5, "%.0f %%"}};
    for (int c = 0; c < 3; c++) {
        char id[16], shown[24];
        float v = e.hsv[c] * rows[c].range;
        snprintf(id, sizeof id, "##hsb%d", c);
        snprintf(shown, sizeof shown, rows[c].fmt, v);
        bool held = false;
        int s = opt_step_row(id, menu_text(rows[c].label), shown, menu_text(UI_SR_INK_HSB_DESC), &held);
        ImVec2 a, b;
        ink_bar_rect(&a, &b);
        ink_bar(e.hsv, c);
        if (!s) continue;
        float mx = ImGui::GetIO().MousePos.x;
        if (s == 2 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && mx >= a.x - 6 && mx <= b.x + 6) {
            v = fminf(fmaxf((mx - a.x) / (b.x - a.x), 0), 1) * rows[c].range;   // a click on the bar: that point of it
        } else {
            v = roundf(v / rows[c].step) * rows[c].step + (s == 2 ? 1 : s) * rows[c].step * (held ? 3 : 1);
            if (c == 0) v = fmodf(v + 360, 360);   // the hue goes round
        }
        e.hsv[c] = fminf(fmaxf(v, 0), c == 0 ? 359.0f : 100.0f) / rows[c].range;
        changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
    if (!changed) return;
    ImGui::ColorConvertHSVtoRGB(e.hsv[0], e.hsv[1], e.hsv[2], rgb[0], rgb[1], rgb[2]);
    ink_hex_of(rgb, e.last);
    snprintf(e.custom, sizeof e.custom, "%s", e.last);
    cfg_set(e.key, e.last);
    opt_save_soon();
}

static void tab_ink(float scale) {
    opt_begin("##ink", scale);
    opt_text(UI_SR_INK_NOTE);
    ink_rows(g_ink[0], UI_SR_INK_PLAYER, UI_SR_INK_PLAYER_DESC, scale);
    ink_rows(g_ink[1], UI_SR_INK_ENEMY, UI_SR_INK_ENEMY_DESC, scale);
    opt_end();
}

static void tab_sound(float scale) {
    opt_begin("##sound", scale);
    opt_slider("SR_VOLUME", UI_MENU_VOLUME, 0, 100, UI_MENU_VOLUME_DESC, 5, "%.0f %%");
    static const UiTextId channels[] = {UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51};
    opt_choice("SR_AUDIO_CHANNELS", UI_MENU_AUDIO_CHANNELS, channels, 3, UI_MENU_AUDIO_CHANNELS_DESC);
    opt_end();
}

static void tab_controls(float scale) {
    opt_begin("##controls", scale);
    static const UiTextId layout[] = {UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO, UI_MENU_PAD_LAYOUT_XBOX};
    opt_choice("SR_PAD_LAYOUT", UI_MENU_PAD_LAYOUT, layout, 3, UI_SR_PAD_LAYOUT_DESC, pad_layout_note());
    opt_toggle("SR_MOTION", UI_MENU_MOTION, UI_MENU_MOTION_DESC);
    opt_slider("SR_RUMBLE_STRENGTH", UI_MENU_RUMBLE_STRENGTH, 0, 300, UI_MENU_RUMBLE_STRENGTH_DESC, 10, "%.0f %%");
    opt_slider("SR_RUMBLE_CURVE", UI_MENU_RUMBLE_CURVE, 0.2f, 1.5f, UI_MENU_RUMBLE_CURVE_DESC, 0.05f, "%.2f");
    opt_toggle("SR_RUMBLE_TRIGGERS", UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC);
    opt_slider("SR_MOUSE_SENS", UI_MENU_MOUSE_SENS, 0, 10, UI_MENU_MOUSE_SENS_DESC, 0.1f, "%.1f");
    // Analog sticks: one dead zone row per stick, falling back to SR_STICK_DEADZONE (both sticks) while unset. With
    // a gamepad connected each row shows its stick's deflection now, before the dead zone: a drifting stick's resting
    // value, to set it above.
    opt_group(UI_MENU_GROUP_STICKS);
    double now[2];
    bool pad = input_pad_stick_percent(now) != 0;
    char stick_note[2][64];
    for (int s = 0; s < 2; s++)
        snprintf(stick_note[s], sizeof stick_note[s], menu_text(UI_MENU_STICK_DEADZONE_NOW), now[s]);
    opt_slider("SR_LSTICK_DEADZONE", UI_MENU_LSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[0] : nullptr, "SR_STICK_DEADZONE");
    opt_slider("SR_RSTICK_DEADZONE", UI_MENU_RSTICK_DEADZONE, 0, 50, UI_MENU_STICK_DEADZONE_DESC, 1, "%.0f %%",
               pad ? stick_note[1] : nullptr, "SR_STICK_DEADZONE");
    binds_rows(scale);
    opt_end();
    binds_after_page();
}

static void tab_menu(float scale) {
    opt_begin("##menu", scale);
    static const UiTextId langs[] = {UI_MENU_MENU_LANG_AUTO, UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_RU,
                                     UI_MENU_LANGNAME_PT_BR, UI_MENU_LANGNAME_ZH_HANS};
    opt_choice("SR_MENU_LANG", UI_MENU_MENU_LANG, langs, sizeof langs / sizeof *langs, UI_MENU_MENU_LANG_DESC);
    opt_end();
}

// ---- Advanced: the crash ring, a screenshot, the bug report helpers and the reset of every setting

// Every setting the menu shows back to its default (the environment's keys stay), written to sr-config.ini.
static void reset_all(void) {
    const char *keys[128];
    int n = 0;
    for (const CfgMeta *m; n < 128 && (m = cfg_meta_at(n)); n++) keys[n] = m->key;
    int done = cfg_reset(keys, n);
    opt_save_soon();
    rt_log("menu: reset %d settings to their defaults", done);
}

// A button's result shown in its value for a few seconds: 1 done (done_text), -1 failed.
struct ActionNote { uint64_t until; int result; };

static const char *action_value(const ActionNote &a, UiTextId done_text) {
    if (!a.result || plat_now_ns() > a.until) return "";
    return menu_text(a.result > 0 ? done_text : UI_MENU_ACTION_FAILED);
}

static void action_done(ActionNote &a, bool ok) { a = {plat_now_ns() + 3000000000ull, ok ? 1 : -1}; }

// A screenshot of the game without the menu: the button closes the menu, and the next frame's swapchain image (the
// performance overlay included when it is on) goes to screenshots/ next to sr.exe (menu_overlay_frame).
static std::atomic<int> g_shot_req;
static char g_shot_path[600];

static bool shot_path(void) {
    char dir[512];
    if (!plat_exe_path(dir, sizeof dir)) return false;
    char *cut = plat_last_sep(dir), *fs = strrchr(dir, '/');
    if (fs > cut) cut = fs;
    if (!cut || (size_t)(cut - dir) + sizeof "/screenshots" > sizeof dir) return false;
    strcpy(cut, "/screenshots");
    plat_file_mkdir_p(dir);
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
    snprintf(g_shot_path, sizeof g_shot_path, "%s/sr-%s.png", dir, stamp);
    return true;
}

// The controller's Capture (input.c, the polling thread): what the window shows, the menu too when it is open. A press
// while one is still waiting does nothing: the path is read by the present path.
extern "C" void menu_screenshot_request(void) {
    if (g_shot_req.load() || !shot_path()) return;
    g_shot_req.store(1);
}

// sr-recomp: the next presented frame to this PNG (vk_dump.c, F9 reports); nothing when a screenshot is waiting.
extern "C" void menu_screenshot_to(const char *path) {
    if (g_shot_req.load() || strlen(path) >= sizeof g_shot_path) return;
    strcpy(g_shot_path, path);
    g_shot_req.store(1);
}

// sr-recomp: a line over the game for a few seconds (vk_dump.c: "Report saved"), drawn whether the menu is open or not.
static PlatMutex g_notice_lock = PLAT_MUTEX_INIT;
static char g_notice[200];
static std::atomic<uint64_t> g_notice_until;

extern "C" void menu_notice(const char *text, double seconds) {
    plat_mutex_lock(&g_notice_lock);
    snprintf(g_notice, sizeof g_notice, "%s", text);
    plat_mutex_unlock(&g_notice_lock);
    g_notice_until.store(plat_now_ns() + (uint64_t)(seconds * 1e9));
}

static bool notice_active(void) { return plat_now_ns() < g_notice_until.load(); }

static void notice_draw(const MenuTarget *t, float scale) {
    if (!notice_active()) return;
    char text[sizeof g_notice];
    plat_mutex_lock(&g_notice_lock);
    memcpy(text, g_notice, sizeof text);
    plat_mutex_unlock(&g_notice_lock);
    ImDrawList *list = ImGui::GetForegroundDrawList();
    ImVec2 sz = ImGui::CalcTextSize(text);
    float pad = 8 * scale;
    ImVec2 p((t->width - sz.x) * 0.5f - pad, 16 * scale);   // the top centre: the game's HUD keeps its corners
    list->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + pad * 2), IM_COL32(0, 0, 0, 170), 6 * scale);
    list->AddText(ImVec2(p.x + pad, p.y + pad), IM_COL32(255, 255, 255, 240), text);
}

static void tab_advanced(float scale) {
    static ActionNote copy_note, log_note, shot_note;
    opt_begin("##advanced", scale);
    opt_group(UI_MENU_GROUP_DIAG);
    static const UiTextId ring[] = {UI_MENU_OFF, UI_SR_RING_ON, UI_SR_RING_FULL};
    opt_choice("SR_RING", UI_SR_RING, ring, 3, UI_SR_RING_DESC);
    // no screenshot row in the launcher: there is no game to take a picture of
    if (g_launch.load() == LAUNCH_OFF &&
        opt_button("##screenshot", UI_SR_SCREENSHOT, UI_SR_SCREENSHOT_DESC, action_value(shot_note, UI_SR_SCREENSHOT))) {
        if (shot_path()) {
            g_shot_req.store(1);
            set_open(0);
        } else {
            action_done(shot_note, false);
        }
    }
    if (opt_button("##open_log", UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC, action_value(log_note, UI_MENU_OPEN_LOG))) {
        char dir[512];
        bool ok = sysinfo_log_folder(dir, sizeof dir) && plat_open_folder(dir);
        if (!ok) action_done(log_note, false);   // success shows as the opened folder
    }
    if (opt_button("##copy_info", UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC,
                   action_value(copy_note, UI_MENU_COPY_INFO_DONE))) {
        static char text[16384];
        action_done(copy_note, sysinfo_text(text, sizeof text) > 0 && plat_clipboard_set(text));
    }
    bool ask = opt_button("##reset", UI_MENU_RESET, UI_MENU_RESET_DESC);
    opt_end();
    if (ask) ImGui::OpenPopup("##reset_confirm");   // at the level BeginPopupModal looks it up

    // the confirmation, over the menu; Cancel has the cursor first
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##reset_confirm", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        g_modal = true;
        ImGui::TextUnformatted(menu_text(UI_MENU_RESET_CONFIRM));
        ImGui::Spacing();
        const char *yes = menu_text(UI_MENU_RESET_YES), *no = menu_text(UI_MENU_RESET_NO);
        const ImGuiStyle &st = ImGui::GetStyle();
        float bw = fmaxf(ImGui::CalcTextSize(yes).x, ImGui::CalcTextSize(no).x) + st.FramePadding.x * 4;
        bool close = false;
        if (ImGui::Button(no, ImVec2(bw, 0))) close = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(yes, ImVec2(bw, 0))) {
            reset_all();
            close = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            ImGui::CloseCurrentPopup();
            opt_focus_key("##reset");   // back on the row it came from
        }
        ImGui::EndPopup();
    }
}

static const UiTextId kTabNames[TAB_COUNT] = {UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_SR_TAB_INK, UI_MENU_TAB_SOUND,
                                              UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO, UI_MENU_TAB_MENU,
                                              UI_MENU_TAB_ADVANCED};
static int g_tab;               // the tab shown
static int g_tab_request = -1;  // select this tab at the next layout; -1 none
static int g_focus_page = -1;   // this tab's first row takes the cursor once it is shown; -1 none

// ---- the window: title, tabs (L / R or Q / E switch), the page, and the bottom row with "Continue" and "Quit game".
// B on the gamepad and Esc close it.
static void layout_menu(uint32_t w, uint32_t h, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, 120));
    float ww = fminf(w * 0.88f, 1400 * scale), wh = fminf(h * 0.86f, 820 * scale);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings);
    if (g_font_btn) ImGui::PushFont(g_font_btn);
    ImGui::TextUnformatted(menu_text(UI_MENU_TITLE));
    if (g_font_btn) ImGui::PopFont();
    // the port version and the game update, right-aligned on the title line: what a bug report needs first
    char version[96];
    snprintf(version, sizeof version, menu_text(UI_MENU_VERSION), rt_port_version,
             sr_display_version[0] ? sr_display_version : "?");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::CalcTextSize(version).x);
    ImGui::TextDisabled("%s", version);

    const bool modal_before = g_modal || binds_modal();   // last frame's: this frame's confirmations set it again
    g_modal = false;
    int step = modal_before || ImGui::GetIO().WantTextInput                                              ? 0
             : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
             : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1
                                                                                                         : 0;
    if (step) {
        g_tab_request = (g_tab + step + TAB_COUNT) % TAB_COUNT;
        if (g_tab_request == TAB_MENU) g_tab_request = (g_tab_request + step + TAB_COUNT) % TAB_COUNT;   // hidden
    }
    if (g_tab_request >= 0) g_focus_page = g_tab_request;   // the new page's first row takes the cursor

    const ImGuiStyle &st = ImGui::GetStyle();
    float bottom = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < TAB_COUNT; k++) {
            if (k == TAB_MENU) continue;   // the menu's language: English only for now, so nothing to set there
            char label[128];
            snprintf(label, sizeof label, "  %s  ###tab%d", menu_text(kTabNames[k]), k);   // the id stays per language
            if (!ImGui::BeginTabItem(label, nullptr, g_tab_request == k ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            g_tab = k;
            if (g_focus_page == k) {
                opt_focus_first();
                g_focus_page = -1;
            }
            ImGui::Spacing();
            // the page's rows are a NavFlattened child of this window itself: ImGui clips the items of a direct child
            // only, so behind a second child level the rows scrolled out above the tabs won D-pad moves from the
            // buttons and the tabs (up from "Continue" went to the tabs, down from the tabs to "Continue")
            opt_reserve_bottom(bottom);
            switch (k) {
            case TAB_SCREEN: tab_screen(scale); break;
            case TAB_GAME: tab_game(scale); break;
            case TAB_INK: tab_ink(scale); break;
            case TAB_SOUND: tab_sound(scale); break;
            case TAB_CONTROLS: tab_controls(scale); break;
            case TAB_AMIIBO: tab_amiibo(scale); break;
            case TAB_MENU: tab_menu(scale); break;
            case TAB_ADVANCED: tab_advanced(scale); break;
            }
            ImGui::EndTabItem();
        }
        g_tab_request = -1;
        ImGui::EndTabBar();
    }

    // bottom row: the save status on the left, the two buttons on the right
    ImGui::Spacing();
    float y = ImGui::GetCursorPosY();
    if (!cfg_last_save_ok()) {   // stays until a save succeeds; the settings hold for the session
        ImGui::SetCursorPosY(y + st.FramePadding.y);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", menu_text(UI_MENU_SAVE_FAILED));
    }
    const bool launcher = g_launch.load() != LAUNCH_OFF;
    const char *cont = launcher ? launcher_text(UI_LAUNCHER_BACK) : menu_text(UI_MENU_CONTINUE);
    const char *quit = launcher ? launcher_text(UI_LAUNCHER_QUIT) : menu_text(UI_MENU_QUIT);
    // sr-recomp: Restart in the game's menu, beside them (the launcher restarts on Play when a start setting changed)
    const char *again = menu_text(UI_MENU_RESTART_BUTTON);
    const int nb = launcher ? 2 : 3;
    float bw = fmaxf(ImGui::CalcTextSize(cont).x, ImGui::CalcTextSize(quit).x);
    if (!launcher) bw = fmaxf(bw, ImGui::CalcTextSize(again).x);
    bw += st.FramePadding.x * 4;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - st.WindowPadding.x - bw * nb - st.ItemSpacing.x * (nb - 1), y));
    if (ImGui::Button(cont, ImVec2(bw, 0))) set_open(0);
    if (!launcher) {
        ImGui::SameLine();
        if (ImGui::Button(again, ImVec2(bw, 0))) g_restart_ask = true, g_restart_back = nullptr;
    }
    ImGui::SameLine();
    if (ImGui::Button(quit, ImVec2(bw, 0))) gfx_window_request_close();
    if (g_restart_ask) ImGui::OpenPopup("##restart_confirm"), g_restart_ask = false;   // at the level it is looked up
    restart_confirm();
    bool was_modal = g_modal || binds_modal();
    if (!was_modal && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        set_open(0);
    ImGui::End();
}

static int g_save_pending;   // a changed setting still waits for its write to sr-config.ini (opt_save_tick)

// ---- the launcher's first page (menu.h, launcher_win.c), a centred column over the window's gradient
static std::atomic<int> g_launch_go;    // Play: the game starts
static int g_launch_focus;              // the first button takes the cursor at the next layout
static uint64_t g_launch_cfg;           // launcher_start_settings when the launcher opened

static int g_launch_sel = 1;            // the game-look page: the button shown as chosen; Play first

// The window's background: the game's dark green with its blotches, a plain gradient without the game's pictures.
static void launcher_backdrop(uint32_t w, uint32_t h) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (!g_art_on) {
        const ImU32 top = IM_COL32(21, 41, 32, 255), bottom = IM_COL32(7, 11, 9, 255);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)w, (float)h), top, top, bottom, bottom);
        return;
    }
    dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(0, 38, 28, 255));
    const float tw = fmaxf((float)w, 1280) * 0.42f, th = tw / 2;
    for (float y = 0; y < h; y += th)
        for (float x = 0; x < w; x += tw)
            dl->AddImage(ImGui::GetIO().Fonts->TexID, ImVec2(x, y), ImVec2(x + tw, y + th), g_art[ART_BG].uv0, g_art[ART_BG].uv1,
                         IM_COL32(0, 26, 18, 90));
}

// A hash of the settings the process reads once, when it starts. DLSS_FG and FSR_FG count: their rows are live, but
// switching one on or off needs a start (cfg.c).
static uint64_t launcher_start_settings(void) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](const char *key) {
        char name[96];
        snprintf(name, sizeof name, "SR_%s", key);
        for (const char *p = cfg_value(name); p && *p; p++) hash = (hash ^ (uint8_t)*p) * 1099511628211ull;
        hash = (hash ^ 0xff) * 1099511628211ull;
    };
    const CfgMeta *m;
    for (int i = 0; (m = cfg_meta_at(i)); i++)
        if (!m->live || !strcmp(m->key, "DLSS_FG") || !strcmp(m->key, "FSR_FG")) mix(m->key);
    return hash;
}

static void launcher_play(void) {
    g_save_pending = opt_save_tick(1);
    g_launch.store(LAUNCH_LOADING);
    if (launcher_start_settings() != g_launch_cfg && cfg_save()) {
        rt_log("launcher: a setting read at start changed; restarting into the game");
        gfx_window_request_restart();   // the page says Loading until this process ends; no launcher in the new one
    } else {
        g_launch_go.store(1);
    }
}

// One of the page's buttons. The description line follows the mouse, else the cursor.
static bool launcher_button(UiTextId label, UiTextId desc, float w, float h, bool primary, const char **hovered,
                            const char **focused) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.44f, 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.38f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.62f, 0.43f, 1));
    }
    bool hit = ImGui::Button(launcher_text(label), ImVec2(w, h));
    if (primary) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) *hovered = launcher_text(desc);
    if (ImGui::IsItemFocused()) *focused = launcher_text(desc);
    return hit;
}

// The cursor on the item just laid out, visible: Enter or A presses it at once.
static void launcher_take_cursor(void) {
    ImGui::SetItemDefaultFocus();   // a window that appears gives the cursor to its first item otherwise, Precompile on the boards
    if (!g_launch_focus) return;
    g_launch_focus = 0;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

static void launcher_clock(uint64_t ms, char *out, size_t cap) {
    uint64_t s = ms / 1000;
    snprintf(out, cap, "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

// Text centred in the page's column, line by line as it wraps.
static void launcher_centred(const char *text, const ImVec4 &colour, float left, float col) {
    ImFont *font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char *p = text, *end = text + strlen(text);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    while (p < end) {
        const char *e = font->CalcWordWrapPositionA(size / font->FontSize, p, end, col);
        if (e == p) e = p + 1;
        const char *stop = e;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\n')) stop--;
        ImGui::SetCursorPosX(left + (col - ImGui::CalcTextSize(p, stop).x) / 2);
        ImGui::TextUnformatted(p, stop);
        p = e;
        while (p < end && (*p == ' ' || *p == '\n')) p++;
    }
    ImGui::PopStyleColor();
}

// The line under the buttons after a build: how it ended.
static void launcher_outcome(const LauncherWarm *s, float left, float col) {
    if (s->outcome == LAUNCH_WARM_NONE) {
        if (s->stale) launcher_centred(launcher_text(UI_LAUNCHER_WARM_STALE), ImVec4(0.95f, 0.56f, 0.11f, 1), left, col);
        return;
    }
    char clock[32], line[640];
    launcher_clock(s->ms, clock, sizeof clock);
    unsigned long long d = s->done, t = s->total;
    bool bad = s->outcome == LAUNCH_WARM_FAILED || s->outcome == LAUNCH_WARM_NO_START;
    switch (s->outcome) {
    case LAUNCH_WARM_DONE: snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_DONE), d, clock); break;
    case LAUNCH_WARM_CANCELLED: snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_CANCELLED), d, t, clock); break;
    case LAUNCH_WARM_FAILED: snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_FAILED), s->code); break;
    default: snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_NO_START), s->code); break;
    }
    ImVec4 colour = bad ? ImVec4(0.95f, 0.45f, 0.35f, 1) : ImVec4(0.95f, 0.56f, 0.11f, 1);
    launcher_centred(line, colour, left, col);
    if (bad && s->detail[0]) launcher_centred(s->detail, colour, left, col);
    if (s->stale) launcher_centred(launcher_text(UI_LAUNCHER_WARM_STALE), ImVec4(0.95f, 0.56f, 0.11f, 1), left, col);
}

// ---- the page in the game's look: three boards in a row, the chosen one larger and tilted, a band that takes its colour,
// and small ink blobs for what matters less. Selection, like the game's: the cursor, or the mouse when it moves.
static void art_quad(ImDrawList *dl, int art, ImVec2 c, float w, float h, ImU32 tint = IM_COL32_WHITE) {
    dl->AddImage(ImGui::GetIO().Fonts->TexID, ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), g_art[art].uv0,
                 g_art[art].uv1, tint);
}

// Text centred on c in the game's display face, cream on its dark offset shadow.
static void art_label(ImDrawList *dl, const char *text, ImVec2 c, float size, float shadow, float fit = 0) {
    ImVec2 t = g_font_btn->CalcTextSizeA(size, FLT_MAX, 0, text);
    if (fit > 0 && t.x > fit) {   // a long label gets smaller to stay on its board
        size *= fit / t.x;
        t = g_font_btn->CalcTextSizeA(size, FLT_MAX, 0, text);
    }
    ImVec2 p(c.x - t.x / 2, c.y - t.y / 2);
    dl->AddText(g_font_btn, size, ImVec2(p.x + shadow, p.y + shadow * 1.3f), IM_COL32(1, 22, 16, 230), text);
    dl->AddText(g_font_btn, size, p, IM_COL32(229, 229, 215, 255), text);
}

static void art_rotate(ImDrawList *dl, int from, ImVec2 c, float degrees) {
    const float s = sinf(degrees * 3.14159265f / 180), k = cosf(degrees * 3.14159265f / 180);
    for (int i = from; i < dl->VtxBuffer.Size; i++) {
        ImVec2 &p = dl->VtxBuffer[i].pos;
        float x = p.x - c.x, y = p.y - c.y;
        p = ImVec2(c.x + x * k - y * s, c.y + x * s + y * k);
    }
}

// An invisible hit area with the page's selection rule. 1 when pressed.
static bool art_hit(const char *id, int index, ImVec2 c, float w, float h) {
    ImGui::SetCursorScreenPos(ImVec2(c.x - w / 2, c.y - h / 2));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));   // the chosen board itself shows the cursor
    bool hit = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        g_launch_sel = index;
    }
    if (ImGui::IsItemFocused()) g_launch_sel = index;
    return hit;
}

// ---- motion, with the game's timing at 60 frames a second. A button's Select animation takes 6 frames and its Decide 8:
// 1.1 to 0.9 by frame 1, held to frame 3, back by frame 4. The cursor loops over 35 frames.
enum { ART_ITEMS = 4 };
static struct {
    float on[ART_ITEMS], speed[ART_ITEMS];   // 0 to 1, a little past it on the way: how chosen each button looks
    float decide[ART_ITEMS];                 // seconds since it was pressed; below 0 when not
    int pending;                             // the button whose press acts when its Decide ends; -1 none
    float band[4];                           // the band's colour, on its way to the chosen button's
    float loop;                              // the cursor loop's time, in its frames
    ImVec2 at;                               // where the cursor's tip is
    int cursor_for;                          // the button it last went to; -1 none (it appears there)
    float ink[ART_ITEMS];                    // the ink buttons' Select animation, in its frames: 0 to 8, back at twice the speed
    bool ink_up[ART_ITEMS];
    float wipe;                              // Play's spiral wipe, in its frames; below 0 when not
} A = {{0, 1, 0, 0}, {}, {-1, -1, -1, -1}, -1, {0, 20, 14, 235}, 0, {}, -1, {}, {}, -1};

static void art_motion(float dt, int sel) {
    // a spring a little short of settling without overshoot: there in about 6 frames, past the end by about a tenth
    for (float left = dt; left > 0; left -= 1 / 240.0f) {
        const float h = fminf(left, 1 / 240.0f);
        for (int i = 0; i < ART_ITEMS; i++) {
            const float f = 1400 * ((i == sel ? 1.0f : 0.0f) - A.on[i]) - 44 * A.speed[i];
            A.speed[i] += f * h;
            A.on[i] += A.speed[i] * h;
        }
    }
    for (int i = 0; i < ART_ITEMS; i++) {
        if (A.decide[i] >= 0) A.decide[i] = A.decide[i] + dt > 8 / 60.0f ? -1 : A.decide[i] + dt;
        A.ink_up[i] = i == sel;
        A.ink[i] = i == sel ? fminf(8, A.ink[i] + dt * 60) : fmaxf(0, A.ink[i] - dt * 120);
    }
    A.loop = fmodf(A.loop + dt * 60, 35);
}

// The Decide animation's size and turn, relative to the chosen size.
static float art_decide(int i, float *turn) {
    *turn = 0;
    if (A.decide[i] < 0) return 1;
    const float f = A.decide[i] * 60;
    *turn = f < 1 ? -f : -1;
    return f < 1 ? 1 - 0.18f * f : f < 3 ? 0.82f : f < 4 ? 0.82f + 0.09f * (f - 3) : 0.91f + 0.09f * (f - 4) / 4;
}

static float art_keys(const float (*k)[2], int n, float t) {   // straight between key frames
    for (int i = 1; i < n; i++)
        if (t <= k[i][0]) return k[i - 1][1] + (k[i][1] - k[i - 1][1]) * (t - k[i - 1][0]) / (k[i][0] - k[i - 1][0]);
    return k[n - 1][1];
}

// The cursor with its tip at p, as the game's layout has it: turned 43 degrees, the picture's centre 45 units behind the
// tip, squashed and stretched by the loop.
static void art_cursor(ImDrawList *dl, ImVec2 p, float scale) {
    static const float kSx[][2] = {{0, 1.03f}, {8, 1.11f}, {10, 1.11f}, {20, 0.94f}, {24, 0.94f}, {35, 1.03f}};
    static const float kSy[][2] = {{0, 1.03f}, {8, 0.93f}, {10, 0.93f}, {20, 1.33f}, {24, 1.33f}, {35, 1.03f}};
    const float sx = art_keys(kSx, 6, A.loop), sy = art_keys(kSy, 6, A.loop);
    int frame = 0;
    for (int i = 0; i < SQUID_FRAMES; i++)
        if (kSquidAt[i] <= A.loop) frame = i;
    const float s = sinf(43 * 3.14159265f / 180), c = cosf(43 * 3.14159265f / 180);
    const float half = 48 * scale, cy = 45 * scale;
    const float cx_[4] = {-half, half, half, -half}, cy_[4] = {-half, -half, half, half};
    ImVec2 q[4];
    for (int i = 0; i < 4; i++) {
        const float x = cx_[i] * sx, y = (cy_[i] + cy) * sy;   // screen axes: the tail lies below the tip
        q[i] = ImVec2(p.x + 3 * scale + x * c - y * s, p.y - 3 * scale + x * s + y * c);
    }
    const auto &g = g_art[ART_SQUID + frame];
    dl->AddImageQuad(ImGui::GetIO().Fonts->TexID, q[0], q[1], q[2], q[3], g.uv0, ImVec2(g.uv1.x, g.uv0.y), g.uv1, ImVec2(g.uv0.x, g.uv1.y));
}

static void art_cursor_to(ImDrawList *dl, int item, ImVec2 target, float dt, float scale) {
    if (A.cursor_for < 0) A.at = target;
    else if (item != A.cursor_for) A.loop = 16;   // it leaves as the dart of its loop
    A.cursor_for = item;
    const float k = 1 - expf(-dt * 34);
    A.at = ImVec2(A.at.x + (target.x - A.at.x) * k, A.at.y + (target.y - A.at.y) * k);
    art_cursor(dl, A.at, scale);
}

// An ink button as the game's BtnSequence_00: teal at rest. Chosen, its ink bursts through nine pictures in eight frames, orange
// over the teal within three, and the label shrinks with the burst and pops back at frame 7.
static void art_blob(ImDrawList *dl, const char *text, ImVec2 c, float scale, int item, float press) {
    const float t = A.ink[item], w = 480 * scale * press, h = 368 * scale * press, a = fminf(t / 3, 1);
    const int frame = (int)t > INK_FRAMES - 1 ? INK_FRAMES - 1 : (int)t;
    art_quad(dl, ART_INK + frame, ImVec2(c.x + 6 * scale, c.y + 6 * scale), w, h, IM_COL32(0, 8, 6, 150));
    art_quad(dl, ART_INK, c, w, h, IM_COL32(12, 172, 157, (int)((1 - a) * 255)));
    art_quad(dl, ART_INK + frame, c, w, h, IM_COL32(236, 104, 64, (int)(a * 255)));
    const float pop = A.ink_up[item] && t > 1 && t < 7 ? (t < 6 ? 1 - 0.31f * (t - 1) / 5 : 0.69f + 0.31f * (t - 6)) : 1;
    art_label(dl, text, c, 34 * scale * press * pop, 2 * scale);
}

static void layout_launcher_art(uint32_t w, uint32_t h, float scale) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h), ImGuiCond_Always);
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBackground);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float cx = w * 0.5f, col = fminf(w * 0.8f, 900 * scale), left = cx - col / 2;
    const ImVec4 cream = ImGui::GetStyle().Colors[ImGuiCol_Text], amber(0.95f, 0.56f, 0.11f, 1);
    {
        float size = g_font_title->FontSize;
        ImVec2 t = g_font_title->CalcTextSizeA(size, FLT_MAX, 0, kLauncherTitle);
        if (t.x > w * 0.92f) size *= w * 0.92f / t.x, t.x = w * 0.92f;   // a narrow window: smaller, on one line
        ImVec2 p(cx - t.x / 2, h * 0.12f);
        dl->AddText(g_font_title, size, ImVec2(p.x + 3 * scale, p.y + 4 * scale), IM_COL32(1, 22, 16, 230), kLauncherTitle);
        dl->AddText(g_font_title, size, p, IM_COL32(229, 229, 215, 255), kLauncherTitle);
    }
    const LauncherWarm *s = launcher_warm_state();
    static int was_running;
    if (was_running != s->running) g_launch_focus = 1, g_launch_sel = 1, A.cursor_for = -1;   // the page's buttons changed
    was_running = s->running;
    const float dt = ImGui::GetIO().DeltaTime;
    if (g_launch.load() == LAUNCH_LOADING) {   // what the wipe closed on, until the game's first frame
        dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(236, 104, 64, 255));
        art_label(dl, launcher_text(UI_LAUNCHER_LOADING), ImVec2(cx, h * 0.5f), 44 * scale, 3 * scale);
        A.loop = fmodf(A.loop + dt * 60, 35);
        art_cursor(dl, ImVec2(cx + 150 * scale, h * 0.5f + 20 * scale), scale);
    } else if (s->running) {
        art_label(dl, launcher_text(UI_LAUNCHER_WARM_TITLE), ImVec2(cx, h * 0.36f), 44 * scale, 3 * scale);
        // the bar: the band, and the band in teal up to the progress
        const ImVec2 bar(cx, h * 0.5f);
        const float bw = 1000 * scale, bh = 120 * scale, part = s->total ? (float)((double)s->done / (double)s->total) : 0;
        art_quad(dl, ART_BAND, bar, bw, bh, IM_COL32(0, 18, 13, 255));
        dl->PushClipRect(ImVec2(bar.x - bw / 2, bar.y - bh), ImVec2(bar.x - bw / 2 + bw * part, bar.y + bh));
        art_quad(dl, ART_BAND, bar, bw, bh, IM_COL32(12, 172, 157, 255));
        dl->PopClipRect();
        // the ink's head and the squid swimming ahead of it
        const ImVec2 head(bar.x - bw / 2 + bw * part, bar.y);
        const float splat = 1 + 0.06f * sinf(A.loop * 3.14159265f * 2 / 35);
        if (part > 0) art_quad(dl, ART_INK + INK_FRAMES - 1, head, 230 * scale * splat, 176 * scale * splat, IM_COL32(12, 172, 157, 255));
        char clock[32], line[256], text[320];
        launcher_clock(s->ms, clock, sizeof clock);
        if (s->stopping) snprintf(line, sizeof line, "%s", launcher_text(UI_LAUNCHER_WARM_STOPPING));
        else if (!s->total) snprintf(line, sizeof line, "%s", launcher_text(UI_LAUNCHER_WARM_STARTING));
        else snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_PROGRESS), (unsigned long long)s->done, (unsigned long long)s->total);
        snprintf(text, sizeof text, "%s", line);
        ImGui::SetCursorPosY(bar.y - ImGui::GetFontSize() * 0.5f);
        launcher_centred(text, cream, left, col);
        snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_TIME), clock);
        ImGui::SetCursorPosY(bar.y + bh * 0.62f);
        launcher_centred(line, cream, left, col);
        ImGui::Spacing();
        launcher_centred(launcher_text(UI_LAUNCHER_WARM_NOTE), amber, left, col);
        ImVec2 c(cx, h * 0.85f);
        ImGui::BeginDisabled(s->stopping != 0);
        bool cancel = art_hit("##cancel", 0, c, 260 * scale, 110 * scale);
        launcher_take_cursor();
        art_motion(dt, s->stopping ? -1 : 0);
        float turn;
        const float press = art_decide(0, &turn);
        const int from = dl->VtxBuffer.Size;
        art_blob(dl, launcher_text(UI_LAUNCHER_WARM_CANCEL), c, scale, 0, press);
        art_rotate(dl, from, c, turn);
        art_cursor_to(dl, 0, ImVec2(head.x + 50 * scale, head.y + 6 * scale), dt, scale);
        if ((cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) && !s->stopping) {
            A.decide[0] = 0;
            launcher_warm_cancel();
        }
        ImGui::EndDisabled();
    } else {
        static const struct { UiTextId label, desc; int art; ImU32 band; } kBoards[3] = {
            {UI_LAUNCHER_WARM, UI_LAUNCHER_WARM_DESC, ART_GREEN, IM_COL32(24, 84, 26, 235)},
            {UI_LAUNCHER_PLAY, UI_LAUNCHER_PLAY_DESC, ART_TEAL, IM_COL32(0, 20, 14, 235)},
            {UI_LAUNCHER_SETTINGS, UI_LAUNCHER_SETTINGS_DESC, ART_RED, IM_COL32(92, 18, 30, 235)},
        };
        const float gap = 496 * scale, bw = 440 * scale, bh = 146 * scale, y = h * 0.4f;
        int pressed = -1;
        // left to right, so that left and right go where they look like going
        for (int i = 0; i < 3; i++) {
            char id[16];
            snprintf(id, sizeof id, "##board%d", i);
            ImVec2 c(cx + (i - 1) * gap, y);
            if (art_hit(id, i, c, bw, bh * 1.2f)) pressed = i;
            if (i == 1) launcher_take_cursor();
        }
        const ImVec2 quit(cx, h * 0.73f);
        if (art_hit("##quit", 3, quit, 260 * scale, 110 * scale)) pressed = 3;
        const int sel = g_launch_sel;
        art_motion(dt, sel);
        // the band under the boards, on its way to the chosen one's colour
        const ImVec2 band(cx, h * 0.585f);
        const ImU32 want = sel < 3 ? kBoards[sel].band : IM_COL32(0, 20, 14, 235);
        const float kb = 1 - expf(-dt * 24);
        for (int i = 0; i < 4; i++) A.band[i] += ((float)((want >> (i * 8)) & 255) - A.band[i]) * kb;
        art_quad(dl, ART_BAND, band, 1000 * scale, 120 * scale, IM_COL32((int)A.band[0], (int)A.band[1], (int)A.band[2], (int)A.band[3]));
        static const UiTextId kQuit = UI_LAUNCHER_QUIT_DESC;
        const char *desc = launcher_text(sel < 3 ? kBoards[sel].desc : kQuit);
        float lines = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, col * 0.84f, desc).y;
        ImGui::SetCursorPosY(band.y - lines / 2);
        launcher_centred(desc, cream, cx - col * 0.42f, col * 0.84f);
        // the boards: the chosen one last, over its neighbours
        ImVec2 tip = ImVec2(quit.x - 100 * scale, quit.y + 46 * scale);
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < 3; i++) {
                bool on = i == sel;
                if (on != (pass == 1)) continue;
                ImVec2 c(cx + (i - 1) * gap, y);
                float turn;
                const float press = art_decide(i, &turn);
                const float k = (1 + 0.2f * A.on[i]) * press, angle = -3 * A.on[i] + turn;
                const float pad = (float)g_art[kBoards[i].art].w / (float)(g_art[kBoards[i].art].w - (int)(9 * scale));
                int from = dl->VtxBuffer.Size;
                art_quad(dl, kBoards[i].art, ImVec2(c.x + (pad - 1) * bw * k / 2, c.y + (pad - 1) * bh * k / 2), bw * k * pad, bh * k * pad);
                art_label(dl, launcher_text(kBoards[i].label), c, (36 + 12 * A.on[i]) * press * scale, (2.5f + A.on[i]) * scale, bw * k * 0.84f);
                art_rotate(dl, from, c, angle);
                if (on) {   // its bottom left corner, turned with it
                    const float x = -bw * k * 0.46f, yy = bh * k * 0.36f, r = angle * 3.14159265f / 180;
                    tip = ImVec2(c.x + x * cosf(r) - yy * sinf(r), c.y + x * sinf(r) + yy * cosf(r));
                }
            }
        {
            float turn;
            const float press = art_decide(3, &turn);
            const int from = dl->VtxBuffer.Size;
            art_blob(dl, launcher_text(UI_LAUNCHER_QUIT), quit, scale, 3, press);
            art_rotate(dl, from, quit, turn);
        }
        art_cursor_to(dl, sel, tip, dt, scale);
        // a press acts when its Decide animation ends; another press waits for that
        if (pressed >= 0 && A.pending < 0 && A.wipe < 0) {
            A.pending = pressed;
            A.decide[pressed] = 0;
        }
        if (A.wipe >= 0) {   // Play: the game's spiral closes over the page, then the game loads behind it
            A.wipe += dt * 60;
            const auto &g = g_art[ART_WIPE + (A.wipe < WIPE_FRAMES ? (int)A.wipe : WIPE_FRAMES - 1)];
            ImGui::GetForegroundDrawList()->AddImage(ImGui::GetIO().Fonts->TexID, ImVec2(0, 0), ImVec2((float)w, (float)h), g.uv0, g.uv1,
                                                     IM_COL32(236, 104, 64, 255));
            if (A.wipe >= WIPE_FRAMES) {
                A.wipe = -1;
                launcher_play();
            }
        }
        if (A.pending >= 0 && A.decide[A.pending] < 0) {
            const int act = A.pending;
            A.pending = -1;
            if (act == 1) A.wipe = 0;
            else if (act == 0) launcher_warm_start();
            else if (act == 2) g_launch.store(LAUNCH_SETTINGS), g_reset.store(1);
            else gfx_window_request_close();
        }
        ImGui::SetCursorPosY(h * 0.885f);
        launcher_outcome(s, left, col);
    }
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::SetCursorPos(ImVec2(w - st.WindowPadding.x - ImGui::CalcTextSize(rt_port_version).x,
                               h - st.WindowPadding.y - ImGui::GetTextLineHeight()));
    ImGui::TextDisabled("%s", rt_port_version);
    ImGui::End();
}

static void layout_launcher(uint32_t w, uint32_t h, float scale) {
    launcher_backdrop(w, h);
    if (g_art_on && g_font_btn) {
        layout_launcher_art(w, h, scale);
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h), ImGuiCond_Always);
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBackground);
    const float col = fminf(w * 0.6f, 680 * scale), left = (w - col) / 2, tall = 66 * scale;
    const ImGuiStyle &style = ImGui::GetStyle();
    const ImVec4 normal = style.Colors[ImGuiCol_Text], dim = style.Colors[ImGuiCol_TextDisabled];
    const float indent = left - style.WindowPadding.x;   // the window's padding comes on top of an indent
    ImGui::Indent(indent);
    ImGui::SetCursorPosY(h * 0.22f);
    ImGui::PushFont(g_font_title);
    launcher_centred(kLauncherTitle, ImVec4(0.93f, 0.76f, 0.33f, 1), left, col);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 26 * scale));
    const LauncherWarm *s = launcher_warm_state();
    static int was_running;
    if (was_running != s->running) g_launch_focus = 1;   // the page's buttons changed
    was_running = s->running;
    if (g_launch.load() == LAUNCH_LOADING) {
        launcher_centred(launcher_text(UI_LAUNCHER_LOADING), normal, left, col);
    } else if (s->running) {
        launcher_centred(launcher_text(UI_LAUNCHER_WARM_TITLE), normal, left, col);
        float part = s->total ? (float)((double)s->done / (double)s->total) : 0;
        ImGui::ProgressBar(part, ImVec2(col, 0), "");
        char clock[32], line[256];
        if (s->stopping) launcher_centred(launcher_text(UI_LAUNCHER_WARM_STOPPING), normal, left, col);
        else if (!s->total) launcher_centred(launcher_text(UI_LAUNCHER_WARM_STARTING), normal, left, col);
        else {
            snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_PROGRESS), (unsigned long long)s->done,
                     (unsigned long long)s->total);
            launcher_centred(line, normal, left, col);
        }
        launcher_clock(s->ms, clock, sizeof clock);
        snprintf(line, sizeof line, launcher_text(UI_LAUNCHER_WARM_TIME), clock);
        launcher_centred(line, normal, left, col);
        ImGui::Spacing();
        launcher_centred(launcher_text(UI_LAUNCHER_WARM_NOTE), dim, left, col);
        ImGui::Spacing();
        ImGui::SetCursorPosX(left + col * 0.25f);
        ImGui::BeginDisabled(s->stopping != 0);
        bool cancel =ImGui::Button(launcher_text(UI_LAUNCHER_WARM_CANCEL), ImVec2(col * 0.5f, tall * 0.8f));
        launcher_take_cursor();
        if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            launcher_warm_cancel();
        ImGui::EndDisabled();
    } else {
        const char *hovered = nullptr, *focused = nullptr;
        bool play = launcher_button(UI_LAUNCHER_PLAY, UI_LAUNCHER_PLAY_DESC, col, tall, true, &hovered, &focused);
        launcher_take_cursor();
        if (play) launcher_play();
        if (launcher_button(UI_LAUNCHER_WARM, UI_LAUNCHER_WARM_DESC, col, tall, false, &hovered, &focused))
            launcher_warm_start();
        if (launcher_button(UI_LAUNCHER_SETTINGS, UI_LAUNCHER_SETTINGS_DESC, col, tall, false, &hovered, &focused)) {
            g_launch.store(LAUNCH_SETTINGS);
            g_reset.store(1);
        }
        if (launcher_button(UI_LAUNCHER_QUIT, UI_LAUNCHER_QUIT_DESC, col, tall, false, &hovered, &focused))
            gfx_window_request_close();
        ImGui::Spacing();
        const char *desc = hovered ? hovered : focused;
        if (desc) launcher_centred(desc, dim, left, col);
        ImGui::Spacing();
        launcher_outcome(s, left, col);
    }
    ImGui::Unindent(indent);
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::SetCursorPos(ImVec2(w - st.WindowPadding.x - ImGui::CalcTextSize(rt_port_version).x,
                               h - st.WindowPadding.y - ImGui::GetTextLineHeight()));
    ImGui::TextDisabled("%s", rt_port_version);
    ImGui::End();
}

extern "C" void menu_launcher_begin(void) {
    g_launch_cfg = launcher_start_settings();
    g_launch_focus = 1;
    g_art_want = game_art_masks_load(kArtMasks) != 0;
    if (!g_art_want) rt_log("launcher: the game's pictures are not available; plain look");
    g_font_scale = 0;   // the atlas is made again with the pictures
    plat_mutex_lock(&ev_lock);
    ev_n = 0;   // nothing from before it opened moves its selection
    plat_mutex_unlock(&ev_lock);
    set_open(1);
    g_launch.store(LAUNCH_HOME);
}

extern "C" int menu_launcher_done(void) { return g_launch_go.load() || g_failed_flag(); }

extern "C" void menu_launcher_end(void) {
    g_launch.store(LAUNCH_OFF);
    set_open(0);
    g_art_want = false;
    g_font_scale = 0;   // the next atlas without the pictures
    game_art_masks_free();
}

// ---- the performance overlay (SR_PERF_OVERLAY), in the top right corner. "fps": the frame rate over the presents of
// the last half second. "full": a panel of perfmon.c's numbers, in the style of SavageWindRecomp's perf_overlay_draw
// (menu.mm): the machine's load and temperatures, the frame rate and frame times with a graph, the render thread's waits.
// Off, it costs a cfg_version load per present.
static unsigned g_perf_ver = ~0u;
static int g_perf_mode;                  // 0 off, 1 fps, 2 full
static std::atomic<int> g_perf_save;     // F10 changed the mode: sr-config.ini is written on the present thread
static uint64_t g_perf_last_ns, g_perf_win_ns, g_perf_max_ns;
static int g_perf_frames;
static char g_perf_text[96];

static int perf_mode_of(const char *s) { return !strcmp(s, "full") ? 2 : !strcmp(s, "fps") ? 1 : 0; }

extern "C" void menu_perf_cycle(void) {
    static const char *const modes[] = {"off", "fps", "full"};
    cfg_set("SR_PERF_OVERLAY", modes[(perf_mode_of(cfg_value("SR_PERF_OVERLAY")) + 1) % 3]);
    g_perf_save.store(1);
}

// The device the game renders on, for perfmon's GPU counters (by LUID), NVML (by name) and ADL (by PCI bus, else
// name); once, before full mode.
static void perf_gpu_once(const MenuTarget *t) {
    static bool done;
    if (done) return;
    done = true;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ep);
    bool has_bus = false;
    if (ep && vkEnumerateDeviceExtensionProperties(t->phys, NULL, &ne, ep) >= 0)
        for (uint32_t i = 0; i < ne && !has_bus; i++) has_bus = !strcmp(ep[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
    free(ep);
    VkPhysicalDevicePCIBusInfoPropertiesEXT bus = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, has_bus ? &bus : nullptr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(t->phys, &p2);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(t->phys, &mp);
    uint64_t vram = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram += mp.memoryHeaps[h].size;
    perfmon_set_gpu(p2.properties.deviceName, id.deviceLUIDValid ? id.deviceLUID : NULL, has_bus ? (int)bus.pciBus : -1,
                    (float)(vram >> 20));
}

static void perf_tick(uint64_t now, const MenuTarget *t) {
    if (cfg_version() != g_perf_ver) {
        g_perf_ver = cfg_version();
        int m = perf_mode_of(cfg_value("SR_PERF_OVERLAY"));
        if (m != g_perf_mode) {
            g_perf_last_ns = 0, g_perf_text[0] = 0;
            if (m == 2) perf_gpu_once(t);
            perfmon_mode(m);
        }
        g_perf_mode = m;
    }
    if (g_perf_mode != 1) return;
    uint64_t d = now - g_perf_last_ns;
    if (!g_perf_last_ns || d > 2000000000ull) {   // first frame, or a stall the window should not average in
        g_perf_win_ns = g_perf_max_ns = 0;
        g_perf_frames = 0;
    } else {
        g_perf_win_ns += d;
        g_perf_frames++;
        if (d > g_perf_max_ns) g_perf_max_ns = d;
        if (g_perf_win_ns >= 500000000ull) {
            snprintf(g_perf_text, sizeof g_perf_text, menu_text(UI_PERF_LINE_FPS), g_perf_frames * 1e9 / g_perf_win_ns);
            g_perf_win_ns = g_perf_max_ns = 0;
            g_perf_frames = 0;
        }
    }
    g_perf_last_ns = now;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
// on samples of 8s (Roboto's digits share one width), and the numbers are right-aligned in it.
struct PerfCol { UiTextId fmt; float sample; };

static float perf_text_w(UiTextId fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    return ImGui::CalcTextSize(b).x;
}

// A fixed column as wide as the widest of the formats (with their samples) and labels given.
static void perf_column(const char *id, const PerfCol *cols, int n, const UiTextId *labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(menu_text(UI_MENU_OV_NONE)).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, perf_text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(menu_text(labels[i])).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

static void perf_right(const char *s, bool dim) {
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (dim) ImGui::TextDisabled("%s", s);
    else ImGui::TextUnformatted(s);
}

// One cell of the full panel's table: fmt (a catalog format, one number) with v, or "-" when the machine gives none.
static void perf_cell(UiTextId fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return perf_right(menu_text(UI_MENU_OV_NONE), true);
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    perf_right(b, false);
}

static void perf_label(UiTextId label, const ImVec4 &c) {
    ImGui::TableNextColumn();
    ImGui::TextColored(c, "%s", menu_text(label));
}

static float perf_gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// The picture lines: the window, the game image the present shows (SR's 3D scene size is DLSS's line), how the
// present scales the image to the window (gfx_present.c, the last present), the frame limit and the presentation mode
// in use.
static void perf_picture(const MenuTarget *t, const ImVec4 &c) {
    char v[4][64];
    snprintf(v[0], sizeof v[0], menu_text(UI_MENU_OV_FMT_SIZE), t->width, t->height);
    snprintf(v[1], sizeof v[1], menu_text(UI_MENU_OV_FMT_SIZE), (unsigned)t->image_w, (unsigned)t->image_h);
    if (t->scaler == 2) {
        int s = cfg_int("SR_SHARPNESS");
        snprintf(v[2], sizeof v[2], menu_text(UI_MENU_OV_FMT_FSR), s < 0 ? 0 : s > 100 ? 100 : s);
    } else {
        // FSR enlarges only (vk_fsr_record): an image larger than the window is shrunk by the blit even with FSR selected
        int down = t->scaler == 1 && !strcmp(cfg_value("SR_UPSCALER"), "fsr") &&
                   (t->image_w > (int)t->width || t->image_h > (int)t->height);
        snprintf(v[2], sizeof v[2], "%s",
                 menu_text(down ? UI_MENU_OV_SCALE_DOWN : t->scaler ? UI_MENU_OV_SCALE_BILINEAR : UI_MENU_OV_SCALE_NONE));
    }
    double hz = frame_step_limit_hz();
    snprintf(v[3], sizeof v[3], "%s", menu_text(UI_MENU_OV_NONE));
    if (hz > 0) snprintf(v[3], sizeof v[3], menu_text(UI_MENU_OV_FPS), hz);
    const char *pm = present_name(gfx_window_present_mode());
    snprintf(v[4], sizeof v[4], "%s", pm ? pm : menu_text(UI_MENU_OV_NONE));
    static const UiTextId labels[5] = {UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCALING, UI_MENU_OV_LIMIT,
                                       UI_MENU_OV_PRESENT};
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (auto &s : v) vw = fmaxf(vw, ImGui::CalcTextSize(s).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < 5; i++) {
        perf_label(labels[i], c);
        ImGui::TableNextColumn();
        perf_right(v[i], false);
    }
    ImGui::EndTable();
}

static void perf_full_draw(const MenuTarget *t, float scale) {
    float w = (float)t->width;
    PerfStats s;
    perfmon_snapshot(&s);
    ImGui::SetNextWindowPos(ImVec2(w - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    // under the menu while it is open (the settings stay readable), else over everything
    if (g_open.load(std::memory_order_relaxed)) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::SetWindowFontScale(0.72f);
    const ImVec4 gpu_c(0.55f, 0.83f, 0.31f, 1), cpu_c(0.18f, 0.59f, 0.80f, 1), ram_c(0.80f, 0.45f, 0.70f, 1),
        fps_c(0.95f, 0.95f, 0.95f, 1), wait_c(0.93f, 0.76f, 0.33f, 1), pic_c(0.62f, 0.70f, 0.95f, 1);
    // the port version and the game update first, so a screenshot in a bug report carries both; formatted once per
    // menu language
    static char ver[96];
    static const char *ver_fmt;
    if (ver_fmt != menu_text(UI_MENU_VERSION)) {
        ver_fmt = menu_text(UI_MENU_VERSION);
        snprintf(ver, sizeof ver, ver_fmt, rt_port_version, sr_display_version[0] ? sr_display_version : "?");
    }
    ImGui::TextDisabled("%s", ver);
    if (ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) {
        static const UiTextId labels[] = {UI_MENU_OV_GPU, UI_MENU_OV_VRAM, UI_MENU_OV_CPU, UI_MENU_OV_RAM,
                                          UI_MENU_OV_FPS_LABEL, UI_MENU_OV_FRAME_LABEL};
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_PCT, 100}, {UI_MENU_OV_FMT_GB, 888.8f}, {UI_MENU_OV_FMT_FPS, 888},
                                     {UI_MENU_OV_FRAME_MS, 888.8f}};
        static const PerfCol c2[] = {{UI_MENU_OV_FMT_TEMP, 188}, {UI_MENU_OV_FMT_GB_OF, 888.8f}, {UI_MENU_OV_LOW1, 888},
                                     {UI_MENU_OV_MAX_MS, 8888.8f}};
        static const PerfCol c3[] = {{UI_MENU_OV_FMT_POWER, 888}, {UI_MENU_OV_FMT_MHZ, 8888}, {UI_MENU_OV_GAME_PCT, 100},
                                     {UI_MENU_OV_GAME_GB, 888.8f}, {UI_MENU_OV_LOW01, 888}};
        perf_column("##c0", nullptr, 0, labels, 6);
        perf_column("##c1", c1, 4);
        perf_column("##c2", c2, 4);
        perf_column("##c3", c3, 5);
        perf_label(UI_MENU_OV_GPU, gpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.gpu_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
        perf_cell(UI_MENU_OV_FMT_POWER, s.gpu_power_w);
        perf_label(UI_MENU_OV_VRAM, gpu_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.vram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.vram_total_mb));
        perf_cell(UI_MENU_OV_FMT_MHZ, s.gpu_clock_mhz);
        perf_label(UI_MENU_OV_CPU, cpu_c);
        perf_cell(UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
        perf_cell(UI_MENU_OV_FMT_TEMP, s.cpu_temp_c);
        perf_cell(UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
        perf_label(UI_MENU_OV_RAM, ram_c);
        perf_cell(UI_MENU_OV_FMT_GB, perf_gb(s.ram_used_mb));
        perf_cell(UI_MENU_OV_FMT_GB_OF, perf_gb(s.ram_total_mb));
        perf_cell(UI_MENU_OV_GAME_GB, perf_gb(s.ram_game_mb));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 6 * scale);   // a gap between the machine and the frames
        // the frames: the rate over the last second with the lows over the last ten seconds, then the frame times
        perf_label(UI_MENU_OV_FPS_LABEL, fps_c);
        perf_cell(UI_MENU_OV_FMT_FPS, s.fps);
        perf_cell(UI_MENU_OV_LOW1, s.low1_fps);
        perf_cell(UI_MENU_OV_LOW01, s.low01_fps);
        perf_label(UI_MENU_OV_FRAME_LABEL, fps_c);
        perf_cell(UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
        perf_cell(UI_MENU_OV_MAX_MS, s.frame_ms_max);
        ImGui::TableNextColumn();
        ImGui::EndTable();
    }
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    // as wide as the panel the tables size; the plot does not count toward that width, so the panel can still shrink
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float max_x = win->DC.CursorMaxPos.x;
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    win->DC.CursorMaxPos.x = max_x;
    // per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's
    // right edge, and the table still asks for no more than its widest label and number
    if (ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) {
        static const PerfCol c1[] = {{UI_MENU_OV_FMT_MS, 888.88f}, {UI_MENU_OV_PER_FRAME, 888.8f}};
        ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
        perf_column("##w1", c1, 2);
        perf_label(UI_MENU_OV_REPLAY, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.replay_ms);
        perf_label(UI_MENU_OV_GPU_TIME, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.gpu_ms);
        perf_label(UI_MENU_OV_PIPE_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.pipe_wait_ms);
        perf_label(UI_MENU_OV_SHADER_WAIT, wait_c);
        perf_cell(UI_MENU_OV_FMT_MS, s.xlate_ms);
        perf_label(UI_MENU_OV_TEX_UPLOADS, wait_c);
        perf_cell(UI_MENU_OV_PER_FRAME, s.tex_uploads);
        ImGui::EndTable();
    }
    perf_picture(t, pic_c);
    if (const char *dl = vk_dlss_overlay_line()) ImGui::TextColored(pic_c, "%s", dl);
    if (const char *fg = sl_fg_overlay_line()) ImGui::TextColored(pic_c, "%s", fg);
    if (s.gpu_name[0]) ImGui::TextDisabled("%s", s.gpu_name);
    if (s.cpu_name[0]) ImGui::TextDisabled("%s", s.cpu_name);
    ImGui::End();
}

static void perf_draw(const MenuTarget *t, float scale) {
    if (g_perf_mode == 2) return perf_full_draw(t, scale);
    float w = (float)t->width;
    if (!g_perf_mode || !g_perf_text[0]) return;
    // with the menu open: under its window, over the dimming layout_menu drew before
    ImDrawList *list = g_open.load(std::memory_order_relaxed) ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    char text[sizeof g_perf_text + 160];   // the frame rate, then DLSS and frame generation when they are on
    const char *dl = vk_dlss_overlay_line(), *fg = sl_fg_overlay_line();
    snprintf(text, sizeof text, "%s%s%s%s%s", g_perf_text, dl ? "\n" : "", dl ? dl : "", fg ? "\n" : "", fg ? fg : "");
    ImVec2 sz = ImGui::CalcTextSize(text);
    float pad = 6 * scale;
    ImVec2 p(w - 12 * scale - sz.x - pad * 2, 12 * scale);   // the top right corner: the game's HUD leaves it empty
    list->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + pad * 2), IM_COL32(0, 0, 0, 150), 4 * scale);
    list->AddText(ImVec2(p.x + pad, p.y + pad), IM_COL32(255, 255, 255, 235), text);
}

// Test hook (sr-recomp): SR_OVERLAY_SHOT=<frame>:<png>[,<count>] saves that presented frame's swapchain image, overlay
// and menu included (--shot saves the game image, before them); with a count, that many frames from there, <png>
// getting _<frame> before its extension.
static void overlay_shot(uint64_t frame, const char **shot) {
    static int parsed;
    static unsigned long long at, count = 1;
    static char path[1024], one[1100];
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("SR_OVERLAY_SHOT");
        if (!e || sscanf(e, "%llu:%1023[^,\n]", &at, path) != 2) at = 0;
        const char *c = e ? strrchr(e, ',') : NULL;
        if (c && atoi(c + 1) > 1) count = (unsigned long long)atoi(c + 1);
    }
    if (!at || frame < at || frame >= at + count) return;
    if (count == 1) {
        *shot = path;
        return;
    }
    const char *dot = strrchr(path, '.');
    int stem = dot ? (int)(dot - path) : (int)strlen(path);
    snprintf(one, sizeof one, "%.*s_%llu%s", stem, path, (unsigned long long)frame, dot ? dot : "");
    *shot = one;
}

extern "C" int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot) {
    *shot = NULL;
    overlay_shot(frame, shot);
    if (g_shot_req.exchange(0)) {   // the Advanced tab's button (it closes the menu first) or the controller's Capture
        *shot = g_shot_path;
        rt_log("menu: screenshot to %s", g_shot_path);
    }
    if (!g_test.parsed) test_parse();
    if (g_test.next_set < g_test.n_sets && frame >= g_test.sets[g_test.next_set].frame) test_apply_sets(frame);
    if (g_perf_save.exchange(0)) cfg_save();
    perf_tick(plat_now_ns(), t);
    amiibo_tick();   // the figure leaves the reader after its hold time (amiibo.c)
    bool open = g_open.load(std::memory_order_relaxed);
    if (!open) {
        if (g_save_pending) g_save_pending = opt_save_tick(1);   // closed: the last change goes to sr-config.ini now
        if (g_test.at && !g_test.opened && frame >= g_test.at) {
            g_test.opened = frame;
            g_test.page = 0;
            g_tab_request = 0;
            set_open(1);
            open = true;
        } else if (!g_perf_mode && !notice_active()) {
            return 0;
        }
    }
    if (g_failed_flag()) {
        set_open(0);
        return 0;
    }
    uint64_t t0 = plat_now_ns();
    if (!g_ctx_ok) make_context();
    float scale = menu_scale(t->height);
    if (!vk_up(t) || (g_font_scale != scale && !fonts_up(scale))) {
        rt_log("menu: Dear ImGui Vulkan back end set-up failed; the menu stays closed");
        g_failed.store(1);
        set_open(0);
        return 0;
    }
    if (g_style_scale != scale) {
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(scale);
        g_style_scale = scale;
    }
    list_gpus(t->instance);
    ImGuiIO &io = ImGui::GetIO();
    int reset = g_reset.exchange(0);
    if (reset) {
        io.ClearInputKeys();
        io.ClearInputMouse();
        g_last_ns = 0;
        g_focus_page = g_tab_request >= 0 ? g_tab_request : g_tab;   // opening: the first row has the cursor
        g_launch_focus = 1;
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);   // a confirmation left open on closing
        g_modal = false;
    }
    io.DisplaySize = ImVec2((float)t->width, (float)t->height);
    double dt = g_last_ns ? (t0 - g_last_ns) / 1e9 : 1 / 60.0;
    io.DeltaTime = (float)(dt < 1e-4 ? 1e-4 : dt > 0.1 ? 0.1 : dt);
    g_last_ns = t0;
    if (!open) {   // the overlay alone: no input reaches ImGui
    } else if (g_test.page < 0) {
        drain_events(io);
        pad_events(io, reset);
    } else {   // the test hook's pages: the user's input in a background test window must not change settings
        plat_mutex_lock(&ev_lock);
        ev_n = 0;
        plat_mutex_unlock(&ev_lock);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    int launch = g_launch.load();
    if (open && launch != LAUNCH_OFF && launch != LAUNCH_SETTINGS) {
        layout_launcher(t->width, t->height, scale);
    } else if (open) {
        if (launch == LAUNCH_SETTINGS) launcher_backdrop(t->width, t->height);
        layout_menu(t->width, t->height, scale);
    }
    perf_draw(t, scale);
    notice_draw(t, scale);
    ImGui::Render();
    if (open) g_save_pending = opt_save_tick(0);
    g_cost_ns += plat_now_ns() - t0;
    if (launch != LAUNCH_OFF) {   // the launcher's test hook: nothing presents a game frame to count, so every 15th frame
        const char *dir = cfg_get("SR_MENU_TEST_DIR");
        static unsigned shots;
        static char path[512];
        const char *series = cfg_get("SR_SHOTS_DIR");   // with it, every frame in order, to look at the motion
        ++shots;
        if (series) {
            snprintf(path, sizeof path, "%s/launcher-%05u.png", series, shots);
            *shot = path;
        } else if (dir && !(shots % 15)) {
            snprintf(path, sizeof path, "%s/launcher-now.png", dir);
            *shot = path;
        }
    }
    if (g_test.page >= 0 && frame >= g_test.opened + g_test.per_page * (uint64_t)(g_test.page + 1)) {
        static char path[512];   // this frame's image goes to a file; then the next tab, or the menu closes
        const char *dir = cfg_get("SR_MENU_TEST_DIR");
        snprintf(path, sizeof path, "%s/menu-test-%llu-%d.png", dir ? dir : ".", (unsigned long long)frame, g_test.page);
        *shot = path;
        if (++g_test.page == TAB_COUNT) {
            g_test.page = -1;
            set_open(0);   // the next frame lays out nothing
        } else {
            g_tab_request = g_test.page;
        }
    }
    return ImGui::GetDrawData()->TotalVtxCount > 0;
}

extern "C" void menu_overlay_draw(VkCommandBuffer cb) {
    uint64_t t0 = plat_now_ns();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    g_cost_ns += plat_now_ns() - t0;
    if (++g_cost_frames == 600) {
        rt_log("menu: %.3f ms CPU per open frame (layout and recording, mean of 600)", g_cost_ns / 600 / 1e6);
        g_cost_ns = g_cost_frames = 0;
    }
}
