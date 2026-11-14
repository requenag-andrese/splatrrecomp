// sr-recomp: what sr.exe does before the game when started without arguments (main.c): the launcher page of the settings
// menu (ui/menu.cpp) in the game's own window, with nothing of the game loaded yet. Play returns to main, which boots the
// game into the same window and Vulkan device. Any argument skips it (--play only that), and so does a restart from the
// in-game menu; a restart from the launcher's settings comes back to it (SR_RESTART_LAUNCHER, main.c).
//
// The shader build runs as a second sr.exe --warm-shaders, as from a command line: a driver crash in it (the journal of
// vk_pipeline.c) leaves this process and its window standing, and its pipeline cache stays out of this process's device.
// It reports through its stdout. Cancel signals an event its workers check between variants, so every build returns and
// the journal is deleted: killing the process would leave the variants in flight on the skip list as driver crashes.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"
#ifdef SR_VULKAN_PROFILE
#include "gfx.h"
#include "vk.h"
#endif

#define WARM_STOP_ENV "SR_WARM_STOP"   // the inherited event's handle value; set only for the launcher's child
#define WARM_PROGRESS "warm-progress"

// ---- the --warm-shaders process (vk_pipeline.c lb_warm_prepared)

static HANDLE warm_stop;

void launcher_warm_child_init(void) {
    char v[32];
    if (plat_getenv(WARM_STOP_ENV, v, sizeof v)) warm_stop = (HANDLE)(uintptr_t)strtoull(v, NULL, 10);
}

int launcher_warm_stopped(void) {
    return warm_stop && WaitForSingleObject(warm_stop, 0) == WAIT_OBJECT_0;
}

void launcher_warm_progress(uint64_t done, uint64_t total) {
    if (warm_stop) printf(WARM_PROGRESS " %llu %llu\n", (unsigned long long)done, (unsigned long long)total);
}

// ---- the launcher's side. Start and tick run on the launcher's loop, which also lays out the page; cancel also on the
// window thread (the window closed during a build), so the event stays open once made.

static LauncherWarm S;
static HANDLE process, out_pipe, reader, stop_event;
static volatile LONG64 got_done, got_total;
static char got_error[512];   // the child's last "warm-shaders:" line
static ULONGLONG t0;

static void warm_line(const char *line) {
    unsigned long long d, t;
    if (sscanf(line, WARM_PROGRESS " %llu %llu", &d, &t) == 2) {
        InterlockedExchange64(&got_total, (LONG64)t);
        InterlockedExchange64(&got_done, (LONG64)d);
    } else if (!strncmp(line, "warm-shaders:", 13)) {
        snprintf(got_error, sizeof got_error, "%s", line);
    }
}

static DWORD WINAPI warm_reader(void *arg) {
    (void)arg;
    char buf[4096], line[1024];
    size_t n = 0;
    DWORD got;
    while (ReadFile(out_pipe, buf, sizeof buf, &got, NULL) && got)
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\r') continue;
            if (buf[i] == '\n' || n == sizeof line - 1) line[n] = 0, warm_line(line), n = 0;
            else line[n++] = buf[i];
        }
    return 0;
}

const LauncherWarm *launcher_warm_state(void) { return &S; }

void launcher_warm_start(void) {
    if (S.running) return;
    const int stale = S.stale;
    memset(&S, 0, sizeof S);
    S.stale = stale;
    got_done = got_total = 0;
    got_error[0] = 0;
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE wr = NULL;
    if (!stop_event) stop_event = CreateEventW(&sa, TRUE, FALSE, NULL);
    wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 2 + 32];
    DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    int ok = stop_event && len && len < MAX_PATH * 2 && CreatePipe(&out_pipe, &wr, &sa, 0);
    if (ok) {
        ResetEvent(stop_event);
        SetHandleInformation(out_pipe, HANDLE_FLAG_INHERIT, 0);
        char v[32];
        snprintf(v, sizeof v, "%llu", (unsigned long long)(uintptr_t)stop_event);
        plat_setenv(WARM_STOP_ENV, v);
        _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --warm-shaders", exe);
        cmd[sizeof cmd / sizeof *cmd - 1] = 0;
        STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                           .hStdOutput = wr, .hStdError = wr};
        PROCESS_INFORMATION pi;
        ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
        plat_setenv(WARM_STOP_ENV, NULL);
        if (ok) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
        }
    }
    DWORD error = GetLastError();
    if (wr) CloseHandle(wr);
    if (!ok) {
        if (out_pipe) CloseHandle(out_pipe);
        out_pipe = NULL;
        S.outcome = LAUNCH_WARM_NO_START;
        S.code = error;
        rt_log("launcher: the shader build did not start (error %lu)", error);
        return;
    }
    reader = CreateThread(NULL, 0, warm_reader, NULL, 0, NULL);
    t0 = GetTickCount64();
    S.running = 1;
    rt_log("launcher: shader build started");
}

void launcher_warm_cancel(void) {
    if (!S.running || !stop_event) return;
    S.stopping = 1;
    SetEvent(stop_event);
}

void launcher_warm_tick(void) {
    if (!S.running) return;
    S.done = (uint64_t)InterlockedCompareExchange64(&got_done, 0, 0);
    S.total = (uint64_t)InterlockedCompareExchange64(&got_total, 0, 0);
    S.ms = GetTickCount64() - t0;
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return;
    DWORD read_end = reader ? WaitForSingleObject(reader, 2000) : WAIT_OBJECT_0;   // the pipe ends with the process
    if (reader) CloseHandle(reader);
    reader = NULL;
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    process = NULL;
    if (read_end != WAIT_TIMEOUT) CloseHandle(out_pipe);   // a reader that outlived its wait still reads it
    out_pipe = NULL;
    S.done = (uint64_t)got_done;
    S.total = (uint64_t)got_total;
    S.code = code;
    S.outcome = code ? LAUNCH_WARM_FAILED : S.stopping ? LAUNCH_WARM_CANCELLED : LAUNCH_WARM_DONE;
    snprintf(S.detail, sizeof S.detail, "%s", got_error);
    S.running = S.stopping = 0;
#ifdef SR_VULKAN_PROFILE
    S.stale = vk_pipeline_warm_stale();
#endif
    rt_log("launcher: shader build ended: code %#lx, %llu of %llu variants in %llu ms", code, (unsigned long long)S.done,
           (unsigned long long)S.total, (unsigned long long)S.ms);
}

#ifdef SR_VULKAN_PROFILE
// The loop runs on main's thread until Play; closing the window ends the process from the window thread (gfx_window_closed).
// The sleep paces the page: under mailbox or immediate presents nothing else does.
void launcher_run(void) {
    gfx_window_start();
    S.stale = vk_pipeline_warm_stale();
    if (S.stale) rt_log("launcher: the driver cache was not filled for this GPU and driver; precompiling is suggested");
    menu_launcher_begin();
    while (!menu_launcher_done()) {
        launcher_warm_tick();
        vk_present_ui();
        plat_sleep_ms(launcher_warm_state()->running ? 33 : 16);
    }
    vk_present_ui();   // "Loading" stays up until the game's first frame
    menu_launcher_end();
    rt_log("launcher: starting the game");
}
#endif
