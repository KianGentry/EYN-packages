#include <netinet/in.h>

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) |
           ((x & 0xFF000000u) >> 24);
}

uint16_t htons(uint16_t hostshort) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return bswap16(hostshort);
#else
    return hostshort;
#endif
}

uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);
}

uint32_t htonl(uint32_t hostlong) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return bswap32(hostlong);
#else
    return hostlong;
#endif
}

uint32_t ntohl(uint32_t netlong) {
    return htonl(netlong);
}
