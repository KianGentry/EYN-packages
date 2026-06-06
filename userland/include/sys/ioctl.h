#pragma once

#include <stddef.h>

#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE      0U
#define _IOC_WRITE     1U
#define _IOC_READ      2U

#define _IOC(dir, type, nr, size) \
	(((dir)  << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr)   << _IOC_NRSHIFT)  | ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr)          _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)   _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)   _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size)  _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#ifndef TIOCGWINSZ
#define TIOCGWINSZ _IOR('t', 104, struct winsize)
#endif

#ifndef TIOCNOTTY
#define TIOCNOTTY _IO('t', 34)
#endif

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

#ifndef FIONREAD
#define FIONREAD 0x541B
#endif

#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS 0x8913
#endif
#ifndef SIOCSIFFLAGS
#define SIOCSIFFLAGS 0x8914
#endif
#ifndef SIOCGIFADDR
#define SIOCGIFADDR 0x8915
#endif
#ifndef SIOCSIFADDR
#define SIOCSIFADDR 0x8916
#endif
#ifndef SIOCGIFDSTADDR
#define SIOCGIFDSTADDR 0x8917
#endif
#ifndef SIOCSIFDSTADDR
#define SIOCSIFDSTADDR 0x8918
#endif
#ifndef SIOCGIFBRDADDR
#define SIOCGIFBRDADDR 0x8919
#endif
#ifndef SIOCSIFBRDADDR
#define SIOCSIFBRDADDR 0x891a
#endif
#ifndef SIOCGIFNETMASK
#define SIOCGIFNETMASK 0x891b
#endif
#ifndef SIOCSIFNETMASK
#define SIOCSIFNETMASK 0x891c
#endif
#ifndef SIOCGIFMETRIC
#define SIOCGIFMETRIC 0x891d
#endif
#ifndef SIOCSIFMETRIC
#define SIOCSIFMETRIC 0x891e
#endif
#ifndef SIOCGIFMTU
#define SIOCGIFMTU 0x8921
#endif
#ifndef SIOCSIFMTU
#define SIOCSIFMTU 0x8922
#endif
#ifndef SIOCSIFNAME
#define SIOCSIFNAME 0x8923
#endif
#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR 0x8927
#endif
#ifndef SIOCSIFHWADDR
#define SIOCSIFHWADDR 0x8924
#endif
#ifndef SIOCGIFMAP
#define SIOCGIFMAP 0x8970
#endif
#ifndef SIOCSIFMAP
#define SIOCSIFMAP 0x8971
#endif
#ifndef SIOCGIFTXQLEN
#define SIOCGIFTXQLEN 0x8942
#endif
#ifndef SIOCSIFTXQLEN
#define SIOCSIFTXQLEN 0x8943
#endif
#ifndef SIOCGIFCONF
#define SIOCGIFCONF 0x8912
#endif
#ifndef SIOCGIFINDEX
#define SIOCGIFINDEX 0x8933
#endif
#ifndef SIOCDIFADDR
#define SIOCDIFADDR 0x8936
#endif
#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE 0x89F0
#endif

int ioctl(int fd, unsigned long request, ...);
