#pragma once

#include <stdint.h>

#ifndef __EYN_BSWAP_DEFINED
#define __EYN_BSWAP_DEFINED 1
static inline uint16_t __eyn_bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t __eyn_bswap32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define htole16(x) ((uint16_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define htobe16(x) __eyn_bswap16((uint16_t)(x))
#define be16toh(x) __eyn_bswap16((uint16_t)(x))
#define htobe32(x) __eyn_bswap32((uint32_t)(x))
#define be32toh(x) __eyn_bswap32((uint32_t)(x))
#else
#define htole16(x) __eyn_bswap16((uint16_t)(x))
#define le16toh(x) __eyn_bswap16((uint16_t)(x))
#define htole32(x) __eyn_bswap32((uint32_t)(x))
#define le32toh(x) __eyn_bswap32((uint32_t)(x))
#define htobe16(x) ((uint16_t)(x))
#define be16toh(x) ((uint16_t)(x))
#define htobe32(x) ((uint32_t)(x))
#define be32toh(x) ((uint32_t)(x))
#endif
