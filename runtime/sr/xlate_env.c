// The shader translator (tools/sr_xlate, started by runtime/core/gfx_shader.c) is a framework-dependent .NET 10 app:
// it runs on the .NET folder the build names (build_runtime.py --dotnet), given to it as DOTNET_ROOT unless the user
// set one (sr-recomp: or a dotnet folder next to sr.exe, below).
#include <stdlib.h>
#include <string.h>

extern const char sr_dotnet_root[];   // modules.c

#ifdef _WIN32
#include <windows.h>

// sr-recomp: a dotnet folder holding dotnet.exe next to sr.exe (a portable game folder the builder made, builder/)
// wins over the build's: the game then needs no .NET of the machine's or of the workspace's.
__attribute__((constructor)) static void sr_set_dotnet_root(void) {
    if (getenv("DOTNET_ROOT")) return;
    char path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *cut = n && n < MAX_PATH ? strrchr(path, '\\') : NULL;
    if (cut) {
        strcpy(cut + 1, "dotnet\\dotnet.exe");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            cut[1 + strlen("dotnet")] = 0;   // the dotnet folder itself
            _putenv_s("DOTNET_ROOT", path);
            return;
        }
    }
    if (sr_dotnet_root[0]) _putenv_s("DOTNET_ROOT", sr_dotnet_root);
}
#endif
