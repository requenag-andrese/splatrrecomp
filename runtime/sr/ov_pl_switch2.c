// Host overrides for the Switch 2 SDK's built-in font calls (nn::pl, service "pl:u"), which totk-pc-recomp's runtime
// does not have. The fonts are the BFTTF files of the user's own firmware (tools/firmware_fonts.py), loaded from the
// same directory as ov_system.c's shared fonts (ov_shared_font_dir: SR_SHARED_FONTS, else shared-fonts next to the
// executable).
//
// Contract, read from the SDK (21.4.4) bodies of both GetBuiltInFontData variants: the out struct is
//   u8 a; u8 b; u8 pad[6]; u64 address; u64 size;
// where address is the font memory base plus the service's offset, which as for SharedFont points past the 8-byte
// BFTTF header, and size is the decoded font size. The return value is the service's "found" flag. The title's main
// asks for type 5 only (Nintendo extension: controller icons) and reads address and size, nothing else.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/host.h"
#include "../core/ov.h"
#include "../core/platform/platform.h"

// Index = file number in the shared-fonts directory.
#define FONT_FILES 7
static const char *const font_files[FONT_FILES] = {
    "0-standard.bfttf", "1-chinese-simplified.bfttf", "2-chinese-simplified-extension.bfttf",
    "3-chinese-traditional.bfttf", "4-korean.bfttf", "5-nintendo-extension.bfttf", "6-nintendo-extension2.bfttf"};
static struct { PlatMutex lock; const uint8_t *file[FONT_FILES]; uint64_t size[FONT_FILES]; } fonts = {PLAT_MUTEX_INIT};

// The file for a font type. Nx types 0..5 are SharedFontType's. For the Ounce types the order is assumed to be the same
// except that type 5 is the firmware's newer Nintendo-extension font (nintendo_ext2, which firmware 20+ adds next to
// nintendo_ext); the older file stands in when the newer one is absent.
static int font_file(int ounce, int64_t type) {
    if (type < 0 || type > 5) return -1;
    return ounce && type == 5 ? 6 : (int)type;
}

static const uint8_t *load(int i) {
    plat_mutex_lock(&fonts.lock);
    if (!fonts.file[i]) {
        char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
        snprintf(path, sizeof path, "%s/%s", ov_shared_font_dir(dir, sizeof dir), font_files[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            plat_fseek64(f, 0, SEEK_END);
            int64_t len = plat_ftell64(f);
            plat_fseek64(f, 0, SEEK_SET);
            if (len < 12 || len > (64 << 20)) rt_fatal("nn::pl: built-in font %s: size %lld", path, (long long)len);
            uint8_t *p = mem_kernel_alloc((uint64_t)len, 16);
            size_t got = fread(p, 1, (size_t)len, f);
            fclose(f);
            uint64_t size = got == (size_t)len ? ov_bfttf_size(p, (uint64_t)len) : 0;
            if (!size) rt_fatal("nn::pl: built-in font %s is not a BFTTF font", path);
            fonts.size[i] = size;
            __atomic_store_n(&fonts.file[i], p, __ATOMIC_RELEASE);
            rt_log("pl: built-in font %s: %llu bytes", font_files[i], (unsigned long long)size);
        }
    }
    const uint8_t *p = fonts.file[i];
    plat_mutex_unlock(&fonts.lock);
    return p;
}

typedef struct {
    uint8_t a, b, pad[6];
    uint64_t address, size;
} BuiltInFontData;

static void get_font(Cpu *c, int ounce) {
    BuiltInFontData *out = arg_p(c, 0);
    int64_t type = (int32_t)arg_x(c, 1);
    int i = font_file(ounce, type);
    if (!out) rt_fatal("nn::pl::GetBuiltInFontData: null output");
    if (i < 0) rt_fatal("nn::pl::GetBuiltInFontData(%s): unknown font type %lld", ounce ? "Ounce" : "Nx", (long long)type);
    const uint8_t *p = load(i);
    if (!p && i == 6) p = load(i = 5);
    if (!p) {
        char dir[PLAT_PATH_MAX];
        rt_fatal("nn::pl::GetBuiltInFontData(%s, %lld): no %s in %s; run tools/firmware_fonts.py and build with --fonts",
                 ounce ? "Ounce" : "Nx", (long long)type, font_files[i], ov_shared_font_dir(dir, sizeof dir));
    }
    memset(out, 0, sizeof *out);
    out->a = 1;
    out->address = (uint64_t)(uintptr_t)(p + 8);
    out->size = fonts.size[i];
    ret_x(c, 1);
}

// bool GetBuiltInFontData(BuiltInFontDataOunce *out, BuiltInFontTypeOunce type, bool)
static void ov_builtin_font_ounce(Cpu *c) { get_font(c, 1); }
// bool GetBuiltInFontData(BuiltInFontDataNx *out, BuiltInFontTypeNx type, bool)
static void ov_builtin_font_nx(Cpu *c) { get_font(c, 0); }

// void RequestLoadNxFont(void *buffer, size_t size): the SDK hands the service a 19 MiB transfer memory to load the
// Switch 1 fonts into. Here they are read from files on first use, so the buffer is not needed.
static void ov_request_nx_font(Cpu *c) {
    rt_log("pl: RequestLoadNxFont(%#llx bytes): fonts load from files on first use", (unsigned long long)arg_x(c, 1));
}

HOST_OVERRIDE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_20BuiltInFontDataOunceENS0_20BuiltInFontTypeOunceEb",
              ov_builtin_font_ounce);
HOST_OVERRIDE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_17BuiltInFontDataNxENS0_17BuiltInFontTypeNxEb", ov_builtin_font_nx);
HOST_OVERRIDE("_ZN2nn2pl17RequestLoadNxFontEPvm", ov_request_nx_font);
