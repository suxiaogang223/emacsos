#ifndef EMACSOS_LITTLEFS_CONFIG_H
#define EMACSOS_LITTLEFS_CONFIG_H
/* Freestanding utilities adapted from littlefs lfs_util.h/lfs_util.c.
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define LFS_NO_MALLOC
#define LFS_TRACE(...)
#define LFS_DEBUG(...)
#define LFS_WARN(...)
#define LFS_ERROR(...)
#define LFS_ASSERT(test) ((void)0)
static inline void *eos_lfs_copy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}
static inline void *eos_lfs_zero(void *d, int c, size_t n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}
static inline int eos_lfs_compare(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        ++x;
        ++y;
    }
    return 0;
}
static inline char *eos_lfs_chr(const char *s, int c)
{
    do {
        if (*s == (char)c)
            return (char *)s;
    } while (*s++);
    return NULL;
}
static inline char *eos_lfs_strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}
static inline size_t eos_lfs_span(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && eos_lfs_chr(set, s[n]))
        ++n;
    return n;
}
static inline size_t eos_lfs_cspan(const char *s, const char *set)
{
    size_t n = 0;
    while (s[n] && !eos_lfs_chr(set, s[n]))
        ++n;
    return n;
}
#define memcpy eos_lfs_copy
#define memset eos_lfs_zero
#define memcmp eos_lfs_compare
#define strchr eos_lfs_chr
#define strcpy eos_lfs_strcpy
#define strspn eos_lfs_span
#define strcspn eos_lfs_cspan
static inline uint32_t lfs_max(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}
static inline uint32_t lfs_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}
static inline uint32_t lfs_aligndown(uint32_t a, uint32_t b)
{
    return a - a % b;
}
static inline uint32_t lfs_alignup(uint32_t a, uint32_t b)
{
    return lfs_aligndown(a + b - 1, b);
}
static inline uint32_t lfs_npw2(uint32_t a)
{
    return a <= 1 ? 0 : 32 - (unsigned)__builtin_clz(a - 1);
}
static inline uint32_t lfs_ctz(uint32_t a)
{
    return (unsigned)__builtin_ctz(a);
}
static inline uint32_t lfs_popc(uint32_t a)
{
    return (unsigned)__builtin_popcount(a);
}
static inline int lfs_scmp(uint32_t a, uint32_t b)
{
    return (int)(a - b);
}
static inline uint32_t lfs_fromle32(uint32_t a)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return a;
#else
    return __builtin_bswap32(a);
#endif
}
static inline uint32_t lfs_tole32(uint32_t a)
{
    return lfs_fromle32(a);
}
static inline uint32_t lfs_frombe32(uint32_t a)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return a;
#else
    return __builtin_bswap32(a);
#endif
}
static inline uint32_t lfs_tobe32(uint32_t a)
{
    return lfs_frombe32(a);
}
static inline uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size)
{
    const uint8_t *p = buffer;
    while (size--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}
static inline void *lfs_malloc(size_t size)
{
    (void)size;
    return NULL;
}
static inline void lfs_free(void *p)
{
    (void)p;
}
#endif
