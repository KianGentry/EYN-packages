#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("SSH client scaffold with banner+KEX smoke and PTY spawn smoke.",
               "ssh user@192.168.1.10");

#define SSH_MAX_TARGET 192
#define SSH_MAX_USER   64
#define SSH_MAX_HOST   128
#define SSH_MAX_LINE   256
#define SSH_MAX_PACKET 4096

typedef struct {
    char user[SSH_MAX_USER];
    char host[SSH_MAX_HOST];
    uint16_t port;
} ssh_target_t;

static void usage(void) {
    puts("Usage:\n"
         "  ssh [user@]host[:port]            Banner + KEXINIT smoke\n"
         "  ssh --probe [user@]host[:port]    Same as default mode\n"
         "  ssh --pty-smoke <path> [args...]  Spawn child on PTY and relay output\n"
         "\nExamples:\n"
         "  ssh 192.168.1.20\n"
         "  ssh kian@192.168.1.20:22\n"
         "  ssh --pty-smoke /binaries/echo hello");
}

static int parse_ipv4_str(const char* s, uint8_t out[4]) {
    if (!s || !out) return -1;
    for (int part = 0; part < 4; ++part) {
        if (*s < '0' || *s > '9') return -1;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = (v * 10) + (*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part != 3) {
            if (*s != '.') return -1;
            s++;
        }
    }
    return (*s == '\0') ? 0 : -1;
}

static int resolve_host_ipv4(const char* host, uint8_t out[4]) {
    if (!host || !out) return -1;
    if (parse_ipv4_str(host, out) == 0) return 0;
    if (eyn_sys_net_dns_resolve(host, out) == 0) return 0;
    return -1;
}

static int parse_target(const char* in, ssh_target_t* out) {
    if (!in || !out) return -1;

    char tmp[SSH_MAX_TARGET];
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    strncpy(out->user, "user", sizeof(out->user) - 1);
    out->user[sizeof(out->user) - 1] = '\0';
    out->port = 22;

    char* host_part = tmp;
    char* at = strchr(tmp, '@');
    if (at) {
        *at = '\0';
        if (tmp[0] != '\0') {
            strncpy(out->user, tmp, sizeof(out->user) - 1);
            out->user[sizeof(out->user) - 1] = '\0';
        }
        host_part = at + 1;
    }

    if (host_part[0] == '\0') return -1;

    char* colon = strrchr(host_part, ':');
    if (colon) {
        *colon = '\0';
        colon++;
        if (*colon == '\0') return -1;
        char* end = NULL;
        unsigned long p = strtoul(colon, &end, 10);
        if (!end || *end != '\0' || p == 0 || p > 65535) return -1;
        out->port = (uint16_t)p;
    }

    if (host_part[0] == '\0') return -1;
    strncpy(out->host, host_part, sizeof(out->host) - 1);
    out->host[sizeof(out->host) - 1] = '\0';
    return 0;
}

static int recv_line_tcp(char* out, int out_cap, int attempts_max) {
    if (!out || out_cap <= 1) return -1;

    int used = 0;
    int attempts = 0;
    while (attempts < attempts_max && used < out_cap - 1) {
        char ch = 0;
        int rc = eyn_sys_net_tcp_recv(&ch, 1);
        if (rc > 0) {
            out[used++] = ch;
            if (ch == '\n') break;
            continue;
        }
        if (rc == -2) break;
        attempts++;
        (void)usleep(10000);
    }

    out[used] = '\0';
    return (used > 0) ? used : -1;
}

static int tcp_recv_exact(void* out, int need, int attempts_max) {
    if (!out || need < 0) return -1;
    if (need == 0) return 0;

    int got = 0;
    int attempts = 0;
    while (got < need && attempts < attempts_max) {
        int rc = eyn_sys_net_tcp_recv((uint8_t*)out + got, (uint32_t)(need - got));
        if (rc > 0) {
            got += rc;
            continue;
        }
        if (rc == -2) return -1;
        attempts++;
        (void)usleep(10000);
    }
    return (got == need) ? 0 : -1;
}

static int tcp_send_all(const void* buf, int len) {
    if (!buf || len < 0) return -1;
    int sent = 0;
    while (sent < len) {
        int rc = eyn_sys_net_tcp_send((const uint8_t*)buf + sent, (uint32_t)(len - sent));
        if (rc <= 0) return -1;
        sent += rc;
    }
    return 0;
}

static void be32_write(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)((v >> 24) & 0xFFu);
    out[1] = (uint8_t)((v >> 16) & 0xFFu);
    out[2] = (uint8_t)((v >> 8) & 0xFFu);
    out[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t be32_read(const uint8_t in[4]) {
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static int ssh_send_packet(const uint8_t* payload, int payload_len, uint32_t seed) {
    if (!payload || payload_len <= 0 || payload_len > (SSH_MAX_PACKET - 32)) return -1;

    int block_size = 8;
    int padding_len = 4;
    while (((1 + payload_len + padding_len + 4) % block_size) != 0) padding_len++;
    if (padding_len < 4) padding_len = 4;

    int packet_len = 1 + payload_len + padding_len;
    int total_len = 4 + packet_len;
    if (total_len > SSH_MAX_PACKET) return -1;

    uint8_t packet[SSH_MAX_PACKET];
    be32_write(packet, (uint32_t)packet_len);
    packet[4] = (uint8_t)padding_len;
    memcpy(packet + 5, payload, (size_t)payload_len);

    uint32_t x = seed ? seed : 0x4E594E31u;
    for (int i = 0; i < padding_len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        packet[5 + payload_len + i] = (uint8_t)(x & 0xFFu);
    }

    return tcp_send_all(packet, total_len);
}

static int ssh_recv_packet(uint8_t* payload_out, int payload_cap, int* payload_len_out, int* msg_id_out) {
    if (!payload_out || payload_cap <= 0 || !payload_len_out || !msg_id_out) return -1;

    uint8_t hdr[5];
    if (tcp_recv_exact(hdr, sizeof(hdr), 800) != 0) return -1;

    uint32_t packet_len = be32_read(hdr);
    uint8_t padding_len = hdr[4];
    if (packet_len < 2 || packet_len > (uint32_t)(SSH_MAX_PACKET - 4)) return -1;

    int remain = (int)packet_len - 1;
    uint8_t body[SSH_MAX_PACKET];
    if (remain <= 0 || remain > (int)sizeof(body)) return -1;
    if (tcp_recv_exact(body, remain, 800) != 0) return -1;

    int payload_len = remain - (int)padding_len;
    if (payload_len <= 0 || payload_len > payload_cap) return -1;

    memcpy(payload_out, body, (size_t)payload_len);
    *payload_len_out = payload_len;
    *msg_id_out = payload_out[0];
    return 0;
}

static int ssh_send_kexinit(void) {
    uint8_t payload[1024];
    int off = 0;
    payload[off++] = 20; // SSH_MSG_KEXINIT

    uint32_t seed = (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    uint32_t x = seed ? seed : 0xA5A5A5A5u;
    for (int i = 0; i < 16; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        payload[off++] = (uint8_t)(x & 0xFFu);
    }

    const char* lists[10] = {
        "diffie-hellman-group14-sha256,diffie-hellman-group14-sha1",
        "ssh-rsa",
        "aes128-ctr,aes256-ctr",
        "aes128-ctr,aes256-ctr",
        "hmac-sha2-256,hmac-sha1",
        "hmac-sha2-256,hmac-sha1",
        "none",
        "none",
        "",
        ""
    };

    for (int i = 0; i < 10; ++i) {
        uint32_t n = (uint32_t)strlen(lists[i]);
        if (off + 4 + (int)n >= (int)sizeof(payload)) return -1;
        be32_write(&payload[off], n);
        off += 4;
        if (n > 0) {
            memcpy(&payload[off], lists[i], n);
            off += (int)n;
        }
    }

    if (off + 5 >= (int)sizeof(payload)) return -1;
    payload[off++] = 0;
    be32_write(&payload[off], 0);
    off += 4;

    return ssh_send_packet(payload, off, seed ^ 0x53534821u);
}

static int ssh_probe_banner(const char* target_str) {
    ssh_target_t t;
    if (parse_target(target_str, &t) != 0) {
        puts("ssh: invalid target format; expected [user@]host[:port]");
        return 1;
    }

    uint8_t ip[4] = {0, 0, 0, 0};
    if (resolve_host_ipv4(t.host, ip) != 0) {
        printf("ssh: DNS/IPv4 resolve failed for %s\n", t.host);
        return 1;
    }

    printf("ssh: connecting to %s@%u.%u.%u.%u:%u\n",
           t.user,
           (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
           (unsigned)t.port);

    if (eyn_sys_net_tcp_connect(ip, t.port, 0) != 0) {
        puts("ssh: tcp connect failed");
        return 1;
    }

    char server_banner[SSH_MAX_LINE];
    int got = recv_line_tcp(server_banner, sizeof(server_banner), 500);
    if (got < 0) {
        puts("ssh: failed to read server identification banner");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    printf("ssh: server banner: %s", server_banner);

    const char* client_banner = "SSH-2.0-EYNOS_0.1\r\n";
    if (tcp_send_all(client_banner, (int)strlen(client_banner)) != 0) {
        puts("ssh: failed to send client identification banner");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: client banner sent");

    if (ssh_send_kexinit() != 0) {
        puts("ssh: failed to send KEXINIT packet");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: sent SSH_MSG_KEXINIT");

    uint8_t rx_payload[SSH_MAX_PACKET];
    int rx_len = 0;
    int rx_msg = -1;
    if (ssh_recv_packet(rx_payload, sizeof(rx_payload), &rx_len, &rx_msg) != 0) {
        puts("ssh: failed to read server KEX packet");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    printf("ssh: received packet msg=%d payload_len=%d\n", rx_msg, rx_len);
    puts("ssh: transport+kex smoke complete; DH key exchange and NEWKEYS not implemented yet.");

    (void)eyn_sys_net_tcp_close();
    return 0;
}

static int run_pty_smoke(int argc, char** argv) {
    if (argc < 1 || !argv || !argv[0]) {
        puts("ssh: --pty-smoke requires a path argument");
        return 1;
    }

    int pty_fds[2] = {-1, -1};
    if (eyn_sys_pty_open(pty_fds) != 0) {
        puts("ssh: pty allocation failed");
        return 1;
    }

    int master_fd = pty_fds[0];
    int slave_fd = pty_fds[1];
    (void)fd_set_nonblock(master_fd, 1);

    int pid = spawn_ex(argv[0], (const char* const*)argv, argc,
                       slave_fd, slave_fd, slave_fd, 1);
    if (pid <= 0) {
        puts("ssh: spawn_ex failed in --pty-smoke mode");
        (void)close(master_fd);
        (void)close(slave_fd);
        return 1;
    }

    (void)close(slave_fd);

    int status = 0;
    int exited = 0;
    int empty_after_exit = 0;

    for (;;) {
        char buf[256];
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if (n > 0) {
            (void)write(1, buf, (size_t)n);
            empty_after_exit = 0;
        }

        if (!exited) {
            int wr = waitpid(pid, &status, WNOHANG);
            if (wr == pid) exited = 1;
        }

        if (exited && n <= 0) {
            empty_after_exit++;
            if (empty_after_exit >= 4) break;
        }

        (void)usleep(10000);
    }

    (void)close(master_fd);
    return status;
}

int main(int argc, char** argv) {
    if (argc < 2 || (argv[1] && strcmp(argv[1], "-h") == 0)) {
        usage();
        return (argc < 2) ? 1 : 0;
    }

    if (strcmp(argv[1], "--pty-smoke") == 0) {
        return run_pty_smoke(argc - 2, &argv[2]);
    }

    if (strcmp(argv[1], "--probe") == 0) {
        if (argc < 3) {
            usage();
            return 1;
        }
        return ssh_probe_banner(argv[2]);
    }

    return ssh_probe_banner(argv[1]);
}
