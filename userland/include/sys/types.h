#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int pid_t;
typedef unsigned int id_t;

typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
typedef long time_t;
typedef long suseconds_t;
typedef long clock_t;

typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int useconds_t;
typedef unsigned int dev_t;
typedef unsigned int ino_t;
typedef unsigned int nlink_t;
typedef unsigned int blksize_t;
typedef unsigned int blkcnt_t;

typedef unsigned int nfds_t;
typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

typedef int clockid_t;
typedef int timer_t;
typedef unsigned long long rlim_t;

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

#ifndef le16toh
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define be16toh(x) __eyn_bswap16((uint16_t)(x))
#define be32toh(x) __eyn_bswap32((uint32_t)(x))
#else
#define le16toh(x) __eyn_bswap16((uint16_t)(x))
#define le32toh(x) __eyn_bswap32((uint32_t)(x))
#define be16toh(x) ((uint16_t)(x))
#define be32toh(x) ((uint32_t)(x))
#endif
#endif
