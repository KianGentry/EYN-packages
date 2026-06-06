#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Send ICMP echo requests (IPv4 or hostname).", "ping g.co");

static int parse_ipv4(const char* s, uint8_t out[4]) {
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

static int resolve_target_ipv4(const char* target, uint8_t out[4]) {
    if (!target || !out) return -1;
    if (parse_ipv4(target, out) == 0) return 0;
    if (eyn_sys_net_dns_resolve(target, out) == 0) return 0;
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0] || strcmp(argv[1], "-h") == 0) {
        puts("Usage: ping <destination> [local_ip]");
        puts("destination can be IPv4 (a.b.c.d) or hostname (for example g.co)");
        return (argc >= 2) ? 0 : 1;
    }

    uint8_t dst_ip[4];
    if (resolve_target_ipv4(argv[1], dst_ip) != 0) {
        puts("ping: could not resolve destination");
        return 1;
    }

    eyn_net_config_t cfg;
    if (eyn_sys_netcfg_get(&cfg) != 0) {
        puts("ping: failed to read net config");
        return 1;
    }

    uint8_t local_ip[4] = {cfg.local_ip[0], cfg.local_ip[1], cfg.local_ip[2], cfg.local_ip[3]};

    if (argc >= 3 && argv[2] && argv[2][0]) {
        if (parse_ipv4(argv[2], local_ip) != 0) {
            puts("ping: invalid local_ip (expected a.b.c.d)");
            return 1;
        }
    }

    if (eyn_sys_net_is_inited() == 0) {
        puts("Note: run 'e1000 init' first if ping fails.");
    }

    printf("PING %s (%d.%d.%d.%d) from %d.%d.%d.%d: 64 bytes of data.\n",
           argv[1],
           (int)dst_ip[0], (int)dst_ip[1], (int)dst_ip[2], (int)dst_ip[3],
           (int)local_ip[0], (int)local_ip[1], (int)local_ip[2], (int)local_ip[3]);

    puts("CTRL + C to stop.");

    int replies = eyn_sys_net_ping(dst_ip, local_ip, INT_MAX);
    if (replies < 0) {
        printf("ping: request failed (%d)\n", replies);
        return 1;
    }

    printf("PING done: %d replies\n", replies);
    return 0;
}
