/* Minimal termios.h for EYN-OS userland
 * Provides a small subset of termios needed by nano/toybox
 */
#ifndef TERMIOS_H
#define TERMIOS_H

#include <stdint.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

/* c_iflag bits (subset) */
#define ICRNL   0x00000100
#define IXANY   0x00000800
#define IUTF8   0x00004000

/* c_oflag bits (subset) */
#define OPOST   0x00000001
#define ONLCR   0x00000004

/* c_cflag bits (subset) */
#define CS8     0x00000030
#define CREAD   0x00000080

/* c_lflag bits (subset) */
#define ICANON  0x00000100
#define ECHO    0x00000008
#define ECHOE   0x00000010
#define ECHOK   0x00000020
#define ECHOCTL 0x00000200
#define ECHOKE  0x00000800
#define ISIG    0x00000001
#define IEXTEN  0x00008000

/* indices into c_cc */
#define VMIN  6
#define VTIME 5

/* tcsetattr actions */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* queue selectors for tcflush() */
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
int tcsendbreak(int fd, int duration);
int tcflush(int fd, int queue_selector);
int cfsetspeed(struct termios* termios_p, speed_t speed);
void cfmakeraw(struct termios* termios_p);

#endif
