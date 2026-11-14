// sr-recomp: the six libzstd calls runtime/core/romfs.c makes, for the null profile, which links no libzstd. romfs.c
// uses them only to rebuild the resource size table for player mods, which this port leaves out (runtime/sr
// stubs_removed.c: no mods), so every call reports an error. The real libzstd replaces this in a full build.
#pragma once
#include <stddef.h>

#define ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define ZSTD_CONTENTSIZE_ERROR (0ULL - 2)

unsigned long long ZSTD_getFrameContentSize(const void *src, size_t srcSize);
size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
size_t ZSTD_compress(void *dst, size_t dstCapacity, const void *src, size_t srcSize, int compressionLevel);
size_t ZSTD_compressBound(size_t srcSize);
unsigned ZSTD_isError(size_t code);
