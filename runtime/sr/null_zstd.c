// The null profile's stand-in for libzstd (runtime/compat/null-zstd/zstd.h): every call fails.
#include "../compat/null-zstd/zstd.h"

#define NULL_ZSTD_ERROR ((size_t)-1)

unsigned long long ZSTD_getFrameContentSize(const void *src, size_t n) { (void)src, (void)n; return ZSTD_CONTENTSIZE_ERROR; }
size_t ZSTD_decompress(void *d, size_t dn, const void *s, size_t sn) { (void)d, (void)dn, (void)s, (void)sn; return NULL_ZSTD_ERROR; }
size_t ZSTD_compress(void *d, size_t dn, const void *s, size_t sn, int l) { (void)d, (void)dn, (void)s, (void)sn, (void)l; return NULL_ZSTD_ERROR; }
size_t ZSTD_compressBound(size_t n) { return n + 64; }
unsigned ZSTD_isError(size_t code) { return code == NULL_ZSTD_ERROR; }
