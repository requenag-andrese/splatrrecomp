// sr-recomp: the zstd decompressor for game_art.c: the official single-file build (third_party/zstd/zstddeclib.c, BSD),
// from vendor/switch2-recomp/runtime/third_party/zstd_zbic. The port's own ZSTD_* names answer "no zstd" for the rest
// of the runtime (sr/null_zstd.c), and those stay as they are: the three names both define are renamed here. The
// decompressor #undefs ZSTD_isError on its way, so that one takes a link name instead of a macro.
#include <stddef.h>
unsigned ZSTD_isError(size_t code) __asm__("art_zstd_is_error");
#define ZSTD_decompress art_zstd_decompress
#define ZSTD_getFrameContentSize art_zstd_frame_size
#include "../third_party/zstd/zstddeclib.c"

// A zstd stream's bytes in a new buffer; free() it. NULL when they are damaged or cut off. These frames need not say
// their full size.
uint8_t *game_art_unzstd(const void *src, size_t n, size_t *out) {
    ZSTD_DStream *z = ZSTD_createDStream();
    size_t cap = n * 4 + (1u << 20), done = 0, left = 1;
    uint8_t *data = z ? malloc(cap) : NULL;
    ZSTD_inBuffer in = {src, n, 0};
    while (data && left && !(ZSTD_isError)(left)) {
        if (done == cap) {
            uint8_t *more = realloc(data, cap *= 2);
            if (!more) {
                free(data);
                data = NULL;
                break;
            }
            data = more;
        }
        size_t was_in = in.pos, was_out = done;
        ZSTD_outBuffer o = {data, cap, done};
        left = ZSTD_decompressStream(z, &o, &in);
        done = o.pos;
        if (left && !(ZSTD_isError)(left) && was_in == in.pos && was_out == done) left = (size_t)-1;   // cut off
    }
    if (data && left) {
        free(data);
        data = NULL;
    }
    if (data) *out = done;
    if (z) ZSTD_freeDStream(z);
    return data;
}
