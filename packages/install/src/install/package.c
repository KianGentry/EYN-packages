#include "package.h"
#include "index.h"

#include <eynos_syscall.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mbedtls_alloc.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ciphersuites.h"
#include "mbedtls/error.h"
#include "mbedtls/bignum.h"

#define PKG_HTTP_MAX_HOST 128
#define PKG_HTTP_MAX_PATH 256
#define PKG_HTTP_MAX_HEADER 4096
#define PKG_HTTP_RECV_BUF 1536
#define PKG_HTTP_CHUNK_BUF 4096
#define PKG_HTTP_IDLE_RECV_LIMIT 1000
#define PKG_HTTP_FIRST_BYTE_RECV_LIMIT 2500
#define PKG_HTTP_HANDSHAKE_POLL_LIMIT 1000
#define PKG_HTTP_MAX_REDIRECTS 5
#define PKG_DOWNLOAD_DEFAULT_MAX (8u * 1024u * 1024u)
#define PKG_INSTALL_PATH_CAP 256
#define PKG_TEMP_PATH_CAP 320
#define PKG_IO_CHUNK 1024
#define PKG_PROGRESS_BAR_WIDTH 24
#define PKG_PROGRESS_UPDATE_MS 120u
#define PKG_PROGRESS_UPDATE_BYTES 32768u
#define PKG_PROGRESS_LABEL_CAP 15
// Default off: private-host fallback causes long stalls/off-LAN failures.
#define PKG_ALLOW_PRIVATE_HOST_FALLBACK 0

#define PKG_LOCAL_CACHE_ROOT "/cache"
#define PKG_LOCAL_PACKAGE_CACHE_DIR "/cache/pkg"
#define PKG_LOCAL_BASE_ARCHIVE "/cache/base.pkg"
#define PKG_INSTALLER_MEDIA_PACKAGE_CACHE_DIR "RAM:/installer/pkg"
#define PKG_ALLOW_INSTALLER_MEDIA_SEED 0
#define PKG_EYNFS_MAX_NAME_CHARS 31

#define PKG_SHA256_BLOCK_SIZE 64
#define PKG_SHA256_DIGEST_SIZE 32

typedef struct {
    char host[PKG_HTTP_MAX_HOST];
    char path[PKG_HTTP_MAX_PATH];
    uint16_t port;
    uint8_t is_https;
} pkg_http_url_t;

typedef struct {
    uint8_t rx_buf[PKG_HTTP_RECV_BUF];
    size_t rx_off;
    size_t rx_len;
} pkg_tls_io_ctx_t;

typedef int (*pkg_body_writer_fn)(const uint8_t* data, size_t len, void* ctx);
typedef void (*pkg_progress_update_fn)(size_t downloaded, long total_bytes, void* ctx);

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buffer[PKG_SHA256_BLOCK_SIZE];
    size_t buffer_len;
} pkg_sha256_ctx_t;

int install_embedded_extract_main(int argc, char** argv);

static int g_package_confirm_mode = PACKAGE_CONFIRM_PROMPT;
static int pkg_path_exists(const char* path);

static const uint32_t pkg_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static int pkg_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

int package_set_confirm_mode(int mode) {
    int prev = g_package_confirm_mode;
    if (mode == PACKAGE_CONFIRM_AUTO_ACCEPT) {
        g_package_confirm_mode = PACKAGE_CONFIRM_AUTO_ACCEPT;
    } else {
        g_package_confirm_mode = PACKAGE_CONFIRM_PROMPT;
    }
    return prev;
}

static int pkg_prompt_confirm(const char* warning) {
    if (!warning || !warning[0]) return 0;

    if (g_package_confirm_mode == PACKAGE_CONFIRM_AUTO_ACCEPT) {
        printf("install: warning: %s (auto-accepted)\n", warning);
        return 1;
    }

    printf("install: warning: %s\n", warning);
    printf("install: continue? [y/N]: ");
    fflush(stdout);

    char answer[8];
    int used = 0;
    while (used + 1 < (int)sizeof(answer)) {
        char c = 0;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) break;
        if (c == '\r' || c == '\n') break;
        answer[used++] = c;
    }
    answer[used] = '\0';

    return answer[0] == 'y' || answer[0] == 'Y';
}

static int pkg_path_is_directory(const char* path) {
    if (!path || !path[0]) return 0;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;

    eyn_dirent_t entries[1];
    int rc = getdents(fd, entries, sizeof(entries));
    close(fd);
    return rc >= 0;
}

static int pkg_is_first_level_dir(const char* path) {
    if (!path || path[0] != '/') return 0;
    if (path[1] == '\0') return 0;

    size_t i = 1;
    while (path[i] && path[i] != '/') i++;
    if (path[i] == '\0') return 1;
    return path[i] == '/' && path[i + 1] == '\0';
}

static int pkg_install_name_is_valid(const char* name) {
    if (!name || !name[0]) return 0;

    for (size_t i = 0; name[i]; i++) {
        if (name[i] == '/' || name[i] == '\\' || name[i] == ':') return 0;
    }
    return 1;
}

static int pkg_install_dir_is_valid(const char* path) {
    if (!path || path[0] != '/') return 0;
    if (path[1] == '\0') return 0;

    size_t seg_len = 0;
    for (size_t i = 1; path[i]; i++) {
        if (path[i] == '\\' || path[i] == ':') return 0;

        if (path[i] == '/') {
            if (seg_len == 0) return 0;

            if (seg_len == 1 && path[i - 1] == '.') return 0;
            if (seg_len == 2 && path[i - 1] == '.' && path[i - 2] == '.') return 0;
            seg_len = 0;
            continue;
        }

        seg_len++;
    }

    if (seg_len == 1 && path[strlen(path) - 1] == '.') return 0;
    if (seg_len == 2 && path[strlen(path) - 1] == '.' && path[strlen(path) - 2] == '.') return 0;
    return 1;
}

static int pkg_mkdir_recursive(const char* path) {
    if (!path || !path[0]) return -1;

    char work[PKG_INSTALL_PATH_CAP];
    int needed = snprintf(work, sizeof(work), "%s", path);
    if (needed <= 0 || needed >= (int)sizeof(work)) return -1;

    size_t len = strlen(work);
    while (len > 1 && work[len - 1] == '/') {
        work[len - 1] = '\0';
        len--;
    }

    for (size_t i = 1; work[i]; i++) {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0) != 0 && !pkg_path_is_directory(work)) {
            work[i] = '/';
            return -1;
        }
        work[i] = '/';
    }

    if (mkdir(work, 0) != 0 && !pkg_path_is_directory(work)) return -1;
    return 0;
}

static int pkg_parse_ipv4_str(const char* s, uint8_t out[4]) {
    if (!s || !out) return -1;

    for (int part = 0; part < 4; part++) {
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

static int pkg_resolve_host_ipv4(const char* host, uint8_t out[4]) {
    if (!host || !out) return -1;

    if (pkg_parse_ipv4_str(host, out) == 0) return 0;
    if (eyn_sys_net_dns_resolve(host, out) == 0) return 0;

    printf("install: DNS failed for %s\n", host);
    return -1;
}

static int pkg_parse_http_url(const char* url, pkg_http_url_t* out) {
    if (!url || !out) return -1;

    const char* p = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        out->is_https = 0;
        out->port = 80;
        p = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        out->is_https = 1;
        out->port = 443;
        p = url + 8;
    } else {
        return -1;
    }

    const char* host_start = p;
    while (*p && *p != '/' && *p != ':') p++;

    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;

    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = (port * 10) + (*p - '0');
            if (port > 65535) return -1;
            p++;
        }
        if (port == 0) return -1;
        out->port = (uint16_t)port;
    }

    if (*p == '\0') {
        strncpy(out->path, "/", sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
        return 0;
    }

    if (*p != '/') return -1;

    size_t path_len = strlen(p);
    if (path_len >= sizeof(out->path)) return -1;
    memcpy(out->path, p, path_len + 1);
    return 0;
}

static int pkg_ipv4_is_private(const uint8_t ip[4]) {
    if (!ip) return 0;

    if (ip[0] == 10) return 1;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return 1;
    if (ip[0] == 192 && ip[1] == 168) return 1;
    if (ip[0] == 127) return 1;
    if (ip[0] == 169 && ip[1] == 254) return 1;

    return 0;
}

static int pkg_host_is_private_ipv4_literal(const char* host) {
    uint8_t ip[4];
    if (pkg_parse_ipv4_str(host, ip) != 0) return 0;
    return pkg_ipv4_is_private(ip);
}

static int pkg_copy_url_literal(const char* in_url, char* out_url, size_t out_cap) {
    if (!in_url || !out_url || out_cap == 0) return -1;
    if (strlen(in_url) >= out_cap) return -1;
    strncpy(out_url, in_url, out_cap - 1);
    out_url[out_cap - 1] = '\0';
    return 0;
}

static int pkg_normalize_url_host(const char* in_url, char* out_url, size_t out_cap) {
    if (!in_url || !out_url || out_cap == 0) return -1;

    pkg_http_url_t source;
    if (pkg_parse_http_url(in_url, &source) != 0) {
        return pkg_copy_url_literal(in_url, out_url, out_cap);
    }

    if (!pkg_host_is_private_ipv4_literal(source.host)) {
        return pkg_copy_url_literal(in_url, out_url, out_cap);
    }

    pkg_http_url_t trusted;
    if (pkg_parse_http_url(INSTALL_INDEX_URL, &trusted) != 0 || !trusted.host[0]) {
        return pkg_copy_url_literal(in_url, out_url, out_cap);
    }

    uint16_t default_port = source.is_https ? 443 : 80;
    char port_suffix[16] = {0};
    if (source.port != default_port) {
        snprintf(port_suffix, sizeof(port_suffix), ":%u", (unsigned)source.port);
    }

    const char* scheme = source.is_https ? "https://" : "http://";
    int needed = snprintf(out_url,
                          out_cap,
                          "%s%s%s%s",
                          scheme,
                          trusted.host,
                          port_suffix,
                          source.path);
    if (needed < 0 || (size_t)needed >= out_cap) return -1;

    return 0;
}

static int pkg_path_is_index_json(const char* path) {
    if (!path) return 0;
    if (strcmp(path, "/index.json") == 0) return 1;
    return strncmp(path, "/index.json?", 12) == 0;
}

static int pkg_is_tls_alloc_error(int tls_err) {
    return tls_err == MBEDTLS_ERR_MPI_ALLOC_FAILED
        || tls_err == MBEDTLS_ERR_SSL_ALLOC_FAILED;
}

static int pkg_can_http_fallback(const pkg_http_url_t* parts) {
    if (!parts || !parts->is_https) return 0;
    return !pkg_path_is_index_json(parts->path);
}

static int pkg_build_http_fallback_url(const pkg_http_url_t* https_parts,
                                       char* out_url,
                                       size_t out_cap) {
    if (!https_parts || !out_url || out_cap == 0 || !https_parts->is_https) return -1;

    uint16_t fallback_port = (https_parts->port == 443) ? 80 : https_parts->port;
    char port_suffix[16] = {0};
    if (fallback_port != 80) {
        snprintf(port_suffix, sizeof(port_suffix), ":%u", (unsigned)fallback_port);
    }

    int needed = snprintf(out_url,
                          out_cap,
                          "http://%s%s%s",
                          https_parts->host,
                          port_suffix,
                          https_parts->path);
    return (needed < 0 || (size_t)needed >= out_cap) ? -1 : 0;
}

static int pkg_url_is_absolute(const char* s) {
    if (!s) return 0;
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

static int pkg_make_redirect_url(const pkg_http_url_t* base,
                                 const char* location,
                                 char* out,
                                 size_t out_cap) {
    if (!base || !location || !out || out_cap == 0) return -1;

    if (pkg_url_is_absolute(location)) {
        if (strlen(location) >= out_cap) return -1;
        strncpy(out, location, out_cap - 1);
        out[out_cap - 1] = '\0';
        return 0;
    }

    char path[PKG_HTTP_MAX_PATH];
    if (location[0] == '/') {
        strncpy(path, location, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strncpy(path, base->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';

        char* last = strrchr(path, '/');
        if (!last) {
            strncpy(path, "/", sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            last[1] = '\0';
        }

        if (strlen(path) + strlen(location) + 1 >= sizeof(path)) return -1;
        strcat(path, location);
    }

    uint16_t default_port = base->is_https ? 443 : 80;
    char port_suffix[16] = {0};
    if (base->port != default_port) {
        snprintf(port_suffix, sizeof(port_suffix), ":%u", (unsigned)base->port);
    }

    const char* scheme = base->is_https ? "https://" : "http://";
    int needed = snprintf(out, out_cap, "%s%s%s%s", scheme, base->host, port_suffix, path);
    return (needed < 0 || (size_t)needed >= out_cap) ? -1 : 0;
}

static int pkg_is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static int pkg_header_key_match(const char* line, const char* key) {
    while (*line && *key) {
        if (pkg_ascii_lower(*line) != pkg_ascii_lower(*key)) return 0;
        line++;
        key++;
    }
    return (*key == '\0');
}

static int pkg_header_get_value(const char* headers, const char* key, char* out, size_t out_cap) {
    if (!headers || !key || !out || out_cap == 0) return -1;

    const char* line = headers;
    while (*line) {
        const char* line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (line_end == line) break;

        if (pkg_header_key_match(line, key)) {
            const char* p = line + strlen(key);
            if (*p != ':') {
                line = line_end + 2;
                continue;
            }

            p++;
            while (*p == ' ' || *p == '\t') p++;

            size_t len = (size_t)(line_end - p);
            if (len >= out_cap) len = out_cap - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 0;
        }

        line = line_end + 2;
    }

    return -1;
}

static int pkg_parse_status_code(const char* headers) {
    if (!headers) return -1;

    const char* sp = strchr(headers, ' ');
    if (!sp) return -1;
    sp++;

    int code = 0;
    while (*sp >= '0' && *sp <= '9') {
        code = (code * 10) + (*sp - '0');
        sp++;
    }

    return code;
}

static int pkg_parse_hex_size(const char* s, size_t len, size_t* out) {
    if (!s || !out) return -1;

    size_t v = 0;
    int any = 0;

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int d = -1;

        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else if (c == ';' || c == ' ' || c == '\t') break;
        else return -1;

        any = 1;
        v = (v << 4) | (size_t)d;
    }

    if (!any) return -1;
    *out = v;
    return 0;
}

static int pkg_string_contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !needle[0]) return 0;

    size_t needle_len = strlen(needle);
    size_t hay_len = strlen(haystack);
    if (needle_len > hay_len) return 0;

    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        size_t j = 0;
        while (j < needle_len) {
            if (pkg_ascii_lower(haystack[i + j]) != pkg_ascii_lower(needle[j])) break;
            j++;
        }
        if (j == needle_len) return 1;
    }

    return 0;
}

static int pkg_tcp_send_all(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 512) chunk = 512;

        int rc = eyn_sys_net_tcp_send(p + sent, (uint32_t)chunk);
        if (rc < 0) return -1;
        sent += chunk;
    }

    return 0;
}

static int pkg_tls_rng(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;

    static uint32_t s = 0;
    if (s == 0) {
        s = (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS) ^ 0x7f4a7c15u;
        if (s == 0) s = 1;
    }

    for (size_t i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (unsigned char)(s & 0xffu);
    }

    return 0;
}

static int pkg_tls_send_cb(void* ctx, const unsigned char* buf, size_t len) {
    (void)ctx;
    if (!buf || len == 0) return 0;

    size_t chunk = len;
    if (chunk > 512) chunk = 512;

    int rc = eyn_sys_net_tcp_send(buf, (uint32_t)chunk);
    if (rc < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (rc == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return rc;
}

static int pkg_tls_recv_cb(void* ctx, unsigned char* buf, size_t len) {
    pkg_tls_io_ctx_t* io = (pkg_tls_io_ctx_t*)ctx;
    if (!buf || len == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

    if (!io) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;

    if (io->rx_off < io->rx_len) {
        size_t avail = io->rx_len - io->rx_off;
        size_t take = (len < avail) ? len : avail;
        memcpy(buf, io->rx_buf + io->rx_off, take);
        io->rx_off += take;
        if (io->rx_off == io->rx_len) {
            io->rx_off = 0;
            io->rx_len = 0;
        }
        return (int)take;
    }

    int rc = eyn_sys_net_tcp_recv(io->rx_buf, (uint32_t)sizeof(io->rx_buf));
    if (rc > 0) {
        io->rx_off = 0;
        io->rx_len = (size_t)rc;

        size_t take = (len < io->rx_len) ? len : io->rx_len;
        memcpy(buf, io->rx_buf, take);
        io->rx_off = take;
        if (io->rx_off == io->rx_len) {
            io->rx_off = 0;
            io->rx_len = 0;
        }
        return (int)take;
    }
    if (rc == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (rc == -2) return MBEDTLS_ERR_SSL_CONN_EOF;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int pkg_tls_connect_handshake(const pkg_http_url_t* parts,
                                     const uint8_t ip[4],
                                     mbedtls_ssl_context* ssl,
                                     mbedtls_ssl_config* conf,
                                     pkg_tls_io_ctx_t* io_ctx,
                                     int* out_tls_err) {
    if (!parts || !ip || !ssl || !conf || !io_ctx) return -1;
    if (out_tls_err) *out_tls_err = 0;

    // Reset dedicated TLS allocator arena so each connect starts from a clean pool.
    eyn_mbedtls_alloc_reset();

    if (eyn_sys_net_tcp_connect(ip, parts->port, 0) != 0) {
        if (out_tls_err) *out_tls_err = -1;
        printf("install: TCP connect failed for %s:%u via %u.%u.%u.%u\n",
               parts->host,
               (unsigned)parts->port,
               (unsigned)ip[0],
               (unsigned)ip[1],
               (unsigned)ip[2],
               (unsigned)ip[3]);
        return -1;
    }

    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);

    int rc = mbedtls_ssl_config_defaults(conf,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        if (out_tls_err) *out_tls_err = rc;
        printf("install: TLS config failed (code %d)\n", rc);
        mbedtls_ssl_free(ssl);
        mbedtls_ssl_config_free(conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(conf, pkg_tls_rng, NULL);

    rc = mbedtls_ssl_setup(ssl, conf);
    if (rc != 0) {
        if (out_tls_err) *out_tls_err = rc;
        printf("install: TLS setup failed (code %d)\n", rc);
        mbedtls_ssl_free(ssl);
        mbedtls_ssl_config_free(conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    rc = mbedtls_ssl_set_hostname(ssl, parts->host);
    if (rc != 0) {
        if (out_tls_err) *out_tls_err = rc;
        printf("install: TLS SNI set failed for %s (code %d)\n",
               parts->host,
               rc);
        mbedtls_ssl_free(ssl);
        mbedtls_ssl_config_free(conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    io_ctx->rx_off = 0;
    io_ctx->rx_len = 0;
    mbedtls_ssl_set_bio(ssl, io_ctx, pkg_tls_send_cb, pkg_tls_recv_cb, NULL);

    int handshake_polls = 0;
    for (;;) {
        rc = mbedtls_ssl_handshake(ssl);
        if (rc == 0) {
            break;
        }

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            handshake_polls++;
            if (handshake_polls >= PKG_HTTP_HANDSHAKE_POLL_LIMIT) {
                if (out_tls_err) *out_tls_err = MBEDTLS_ERR_SSL_TIMEOUT;
                printf("install: TLS handshake timeout via %u.%u.%u.%u\n",
                       (unsigned)ip[0],
                       (unsigned)ip[1],
                       (unsigned)ip[2],
                       (unsigned)ip[3]);
                mbedtls_ssl_free(ssl);
                mbedtls_ssl_config_free(conf);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }
            usleep(10000);
            continue;
        }

        if (out_tls_err) *out_tls_err = rc;
        printf("install: TLS handshake failed (code %d) via %u.%u.%u.%u\n",
               rc,
               (unsigned)ip[0],
               (unsigned)ip[1],
               (unsigned)ip[2],
               (unsigned)ip[3]);
        mbedtls_ssl_free(ssl);
        mbedtls_ssl_config_free(conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    return 0;
}

static int pkg_https_get_stream_once(const char* url,
                                     const pkg_http_url_t* parts,
                                     pkg_body_writer_fn writer,
                                     void* writer_ctx,
                                     size_t* out_bytes,
                                     char* out_redirect_url,
                                     size_t out_redirect_url_cap,
                                     pkg_progress_update_fn progress_cb,
                                     void* progress_ctx,
                                     int* out_tls_err) {
    if (!url || !parts || !writer || !out_bytes) return -1;
    if (out_tls_err) *out_tls_err = 0;

    uint8_t dst_ip[4];
    if (pkg_resolve_host_ipv4(parts->host, dst_ip) != 0) return -1;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    pkg_tls_io_ctx_t tls_io;
    if (pkg_tls_connect_handshake(parts, dst_ip, &ssl, &conf, &tls_io, out_tls_err) != 0) return -1;

    int rc = 0;

    char req[512];
    int req_len = snprintf(req,
                           sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EYN-OS/install\r\n"
                           "Connection: close\r\n\r\n",
                           parts->path,
                           parts->host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        puts("install: HTTPS request too large");
        (void)mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)req_len) {
        rc = mbedtls_ssl_write(&ssl,
                               (const unsigned char*)req + sent,
                               (size_t)req_len - sent);
        if (rc > 0) {
            sent += (size_t)rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            usleep(1000);
            continue;
        }

        if (out_tls_err) *out_tls_err = rc;
        printf("install: HTTPS send failed (%d)\n", rc);
        (void)mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        (void)eyn_sys_net_tcp_close();
        return -1;
    }

    char header[PKG_HTTP_MAX_HEADER];
    size_t header_len = 0;
    int header_done = 0;
    int status = 0;

    int chunked = 0;
    long content_length = -1;

    uint8_t chunk_buf[PKG_HTTP_CHUNK_BUF];
    size_t chunk_len = 0;
    size_t chunk_need = 0;
    int chunk_have_size = 0;
    int chunk_done = 0;

    size_t total_written = 0;
    int idle_recv_polls = 0;
    int saw_any_rx = 0;

    for (;;) {
        uint8_t rx_buf[PKG_HTTP_RECV_BUF];
        rc = mbedtls_ssl_read(&ssl, rx_buf, sizeof(rx_buf));

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            idle_recv_polls++;
            int idle_limit = saw_any_rx ? PKG_HTTP_IDLE_RECV_LIMIT : PKG_HTTP_FIRST_BYTE_RECV_LIMIT;
            if (idle_recv_polls >= idle_limit) {
                if (out_tls_err) *out_tls_err = MBEDTLS_ERR_SSL_TIMEOUT;
                printf("\ninstall: HTTPS receive timeout\n");
                (void)mbedtls_ssl_close_notify(&ssl);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }
            usleep(10000);
            continue;
        }

        if (rc == 0 || rc == MBEDTLS_ERR_SSL_CONN_EOF || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        }

        if (rc < 0) {
            if (out_tls_err) *out_tls_err = rc;
            printf("install: HTTPS receive failed (%d)\n", rc);
            (void)mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            (void)eyn_sys_net_tcp_close();
            return -1;
        }

        saw_any_rx = 1;
        idle_recv_polls = 0;

        if (!header_done) {
            if (header_len + (size_t)rc >= sizeof(header)) {
                puts("install: response headers too large");
                (void)mbedtls_ssl_close_notify(&ssl);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            memcpy(header + header_len, rx_buf, (size_t)rc);
            header_len += (size_t)rc;
            header[header_len] = '\0';

            char* marker = strstr(header, "\r\n\r\n");
            if (!marker) continue;

            size_t header_end = (size_t)(marker - header) + 4;
            *marker = '\0';
            header_done = 1;

            status = pkg_parse_status_code(header);
            if (pkg_is_redirect_status(status)) {
                char location[MAX_URL];
                location[0] = '\0';
                if (pkg_header_get_value(header,
                                         "Location",
                                         location,
                                         sizeof(location)) != 0
                    || location[0] == '\0') {
                    printf("install: HTTP status %d with missing Location for %s\n", status, url);
                    (void)mbedtls_ssl_close_notify(&ssl);
                    mbedtls_ssl_free(&ssl);
                    mbedtls_ssl_config_free(&conf);
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }

                if (!out_redirect_url || out_redirect_url_cap == 0
                    || pkg_make_redirect_url(parts,
                                             location,
                                             out_redirect_url,
                                             out_redirect_url_cap) != 0) {
                    puts("install: redirect URL is too long");
                    (void)mbedtls_ssl_close_notify(&ssl);
                    mbedtls_ssl_free(&ssl);
                    mbedtls_ssl_config_free(&conf);
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }

                (void)mbedtls_ssl_close_notify(&ssl);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                (void)eyn_sys_net_tcp_close();
                return 1;
            }

            if (status != 200 && status != 206) {
                printf("install: HTTP status %d for %s\n", status, url);
                (void)mbedtls_ssl_close_notify(&ssl);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            char transfer_encoding[64];
            transfer_encoding[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Transfer-Encoding",
                                     transfer_encoding,
                                     sizeof(transfer_encoding)) == 0) {
                if (pkg_string_contains_ci(transfer_encoding, "chunked")) {
                    chunked = 1;
                }
            }

            char content_len_str[32];
            content_len_str[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Content-Length",
                                     content_len_str,
                                     sizeof(content_len_str)) == 0) {
                content_length = strtol(content_len_str, NULL, 10);
                if (content_length < 0) content_length = -1;
            }

            size_t body_len = header_len - header_end;
            if (body_len > 0) {
                if (!chunked) {
                    if (writer((const uint8_t*)(header + header_end), body_len, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)mbedtls_ssl_close_notify(&ssl);
                        mbedtls_ssl_free(&ssl);
                        mbedtls_ssl_config_free(&conf);
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += body_len;
                    if (progress_cb) progress_cb(total_written, content_length, progress_ctx);
                } else {
                    if (body_len > sizeof(chunk_buf)) {
                        puts("install: chunk buffer overflow");
                        (void)mbedtls_ssl_close_notify(&ssl);
                        mbedtls_ssl_free(&ssl);
                        mbedtls_ssl_config_free(&conf);
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    memcpy(chunk_buf, header + header_end, body_len);
                    chunk_len = body_len;
                }
            }
        } else {
            if (!chunked) {
                if (writer(rx_buf, (size_t)rc, writer_ctx) != 0) {
                    puts("install: body write failed");
                    (void)mbedtls_ssl_close_notify(&ssl);
                    mbedtls_ssl_free(&ssl);
                    mbedtls_ssl_config_free(&conf);
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                total_written += (size_t)rc;
                if (progress_cb) progress_cb(total_written, content_length, progress_ctx);
            } else {
                if (chunk_len + (size_t)rc > sizeof(chunk_buf)) {
                    puts("install: chunk buffer overflow");
                    (void)mbedtls_ssl_close_notify(&ssl);
                    mbedtls_ssl_free(&ssl);
                    mbedtls_ssl_config_free(&conf);
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                memcpy(chunk_buf + chunk_len, rx_buf, (size_t)rc);
                chunk_len += (size_t)rc;
            }
        }

        if (chunked) {
            while (!chunk_done) {
                if (!chunk_have_size) {
                    size_t line_end = 0;
                    while (line_end + 1 < chunk_len) {
                        if (chunk_buf[line_end] == '\r' && chunk_buf[line_end + 1] == '\n') break;
                        line_end++;
                    }
                    if (line_end + 1 >= chunk_len) break;

                    if (pkg_parse_hex_size((const char*)chunk_buf, line_end, &chunk_need) != 0) {
                        puts("install: invalid chunk size");
                        (void)mbedtls_ssl_close_notify(&ssl);
                        mbedtls_ssl_free(&ssl);
                        mbedtls_ssl_config_free(&conf);
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }

                    size_t consume = line_end + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 1;

                    if (chunk_need == 0) {
                        chunk_done = 1;
                        break;
                    }
                }

                if (chunk_have_size) {
                    if (chunk_len < chunk_need + 2) break;

                    if (writer(chunk_buf, chunk_need, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)mbedtls_ssl_close_notify(&ssl);
                        mbedtls_ssl_free(&ssl);
                        mbedtls_ssl_config_free(&conf);
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += chunk_need;
                    if (progress_cb) progress_cb(total_written, content_length, progress_ctx);

                    size_t consume = chunk_need + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 0;
                    chunk_need = 0;
                }
            }
        }

        if (!chunked && content_length >= 0 && total_written >= (size_t)content_length) {
            break;
        }
        if (chunked && chunk_done) break;
    }

    (void)mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    (void)eyn_sys_net_tcp_close();

    if (chunked && !chunk_done) {
        puts("install: incomplete chunked transfer");
        return -1;
    }

    *out_bytes = total_written;
    return 0;
}

static int pkg_http_get_stream_once(const char* url,
                                    pkg_body_writer_fn writer,
                                    void* writer_ctx,
                                    size_t* out_bytes,
                                    char* out_redirect_url,
                                    size_t out_redirect_url_cap,
                                    pkg_progress_update_fn progress_cb,
                                    void* progress_ctx) {
    if (!url || !writer || !out_bytes) return -1;

    pkg_http_url_t parts;
    if (pkg_parse_http_url(url, &parts) != 0) {
        printf("install: invalid URL: %s\n", url);
        return -1;
    }

    if (parts.is_https) {
        int last_tls_err = 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            int rc = pkg_https_get_stream_once(url,
                                               &parts,
                                               writer,
                                               writer_ctx,
                                               out_bytes,
                                               out_redirect_url,
                                               out_redirect_url_cap,
                                               progress_cb,
                                               progress_ctx,
                                               &last_tls_err);
            if (rc >= 0) {
                return rc;
            }

            if (attempt == 0) {
                if (last_tls_err == MBEDTLS_ERR_SSL_TIMEOUT) {
                    puts("install: HTTPS timed out; skipping immediate retry");
                    break;
                }
                if (pkg_is_tls_alloc_error(last_tls_err)) {
                    printf("install: TLS allocator pressure (code %d); retrying HTTPS request\n",
                           last_tls_err);
                    (void)eyn_sys_net_tcp_close();
                    usleep(150000);
                } else {
                    puts("install: retrying HTTPS request after timeout/failure");
                    (void)eyn_sys_net_tcp_close();
                    usleep(50000);
                }
            }
        }

        if (pkg_is_tls_alloc_error(last_tls_err) && pkg_can_http_fallback(&parts)) {
            char fallback_url[MAX_URL];
            if (pkg_build_http_fallback_url(&parts, fallback_url, sizeof(fallback_url)) == 0) {
                printf("install: TLS allocator pressure persists; trying HTTP fallback for %s\n",
                       parts.host);
                return pkg_http_get_stream_once(fallback_url,
                                                writer,
                                                writer_ctx,
                                                out_bytes,
                                                out_redirect_url,
                                                out_redirect_url_cap,
                                                progress_cb,
                                                progress_ctx);
            }
        }

        return -1;
    }

    uint8_t dst_ip[4];
    if (pkg_resolve_host_ipv4(parts.host, dst_ip) != 0) return -1;

    uint16_t connect_port = parts.port;

    if (eyn_sys_net_tcp_connect(dst_ip, connect_port, 0) != 0) {
        printf("install: TCP connect failed for %s:%u\n", parts.host, (unsigned)connect_port);
        return -1;
    }

    char req[512];
    int req_len = snprintf(req,
                           sizeof(req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EYN-OS/install\r\n"
                           "Connection: close\r\n\r\n",
                           parts.path,
                           parts.host);
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
                (void)eyn_sys_net_tcp_close();
        puts("install: HTTP request too large");
        return -1;
    }

    if (pkg_tcp_send_all(req, (size_t)req_len) != 0) {
        (void)eyn_sys_net_tcp_close();
        puts("install: failed to send request");
        return -1;
    }

    char header[PKG_HTTP_MAX_HEADER];
    size_t header_len = 0;
    int header_done = 0;
    int status = 0;

    int chunked = 0;
    long content_length = -1;

    uint8_t chunk_buf[PKG_HTTP_CHUNK_BUF];
    size_t chunk_len = 0;
    size_t chunk_need = 0;
    int chunk_have_size = 0;
    int chunk_done = 0;

    size_t total_written = 0;
    int idle_recv_polls = 0;
    int saw_any_rx = 0;

    for (;;) {
        uint8_t rx_buf[PKG_HTTP_RECV_BUF];
        int rc = eyn_sys_net_tcp_recv(rx_buf, sizeof(rx_buf));
        if (rc == -2) break;

        if (rc < 0) {
            puts("install: HTTP receive failed");
            (void)eyn_sys_net_tcp_close();
            return -1;
        }

        if (rc == 0) {
            idle_recv_polls++;
            int idle_limit = saw_any_rx ? PKG_HTTP_IDLE_RECV_LIMIT : PKG_HTTP_FIRST_BYTE_RECV_LIMIT;
            if (idle_recv_polls >= idle_limit) {
                printf("\ninstall: HTTP receive timeout\n");
                (void)eyn_sys_net_tcp_close();
                return -1;
            }
            usleep(10000);
            continue;
        }

        saw_any_rx = 1;
        idle_recv_polls = 0;

        if (!header_done) {
            if (header_len + (size_t)rc >= sizeof(header)) {
                puts("install: response headers too large");
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            memcpy(header + header_len, rx_buf, (size_t)rc);
            header_len += (size_t)rc;
            header[header_len] = '\0';

            char* marker = strstr(header, "\r\n\r\n");
            if (!marker) continue;

            size_t header_end = (size_t)(marker - header) + 4;
            *marker = '\0';
            header_done = 1;

            status = pkg_parse_status_code(header);
            if (pkg_is_redirect_status(status)) {
                char location[MAX_URL];
                location[0] = '\0';
                if (pkg_header_get_value(header,
                                         "Location",
                                         location,
                                         sizeof(location)) != 0
                    || location[0] == '\0') {
                    printf("install: HTTP status %d with missing Location for %s\n", status, url);
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }

                if (!out_redirect_url || out_redirect_url_cap == 0
                    || pkg_make_redirect_url(&parts,
                                             location,
                                             out_redirect_url,
                                             out_redirect_url_cap) != 0) {
                    puts("install: redirect URL is too long");
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }

                (void)eyn_sys_net_tcp_close();
                return 1;
            }

            if (status != 200 && status != 206) {
                printf("install: HTTP status %d for %s\n", status, url);
                (void)eyn_sys_net_tcp_close();
                return -1;
            }

            char transfer_encoding[64];
            transfer_encoding[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Transfer-Encoding",
                                     transfer_encoding,
                                     sizeof(transfer_encoding)) == 0) {
                if (pkg_string_contains_ci(transfer_encoding, "chunked")) {
                    chunked = 1;
                }
            }

            char content_len_str[32];
            content_len_str[0] = '\0';
            if (pkg_header_get_value(header,
                                     "Content-Length",
                                     content_len_str,
                                     sizeof(content_len_str)) == 0) {
                content_length = strtol(content_len_str, NULL, 10);
                if (content_length < 0) content_length = -1;
            }

            size_t body_len = header_len - header_end;
            if (body_len > 0) {
                if (!chunked) {
                    if (writer((const uint8_t*)(header + header_end), body_len, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += body_len;
                    if (progress_cb) progress_cb(total_written, content_length, progress_ctx);
                } else {
                    if (body_len > sizeof(chunk_buf)) {
                        puts("install: chunk buffer overflow");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    memcpy(chunk_buf, header + header_end, body_len);
                    chunk_len = body_len;
                }
            }
        } else {
            if (!chunked) {
                if (writer(rx_buf, (size_t)rc, writer_ctx) != 0) {
                    puts("install: body write failed");
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                total_written += (size_t)rc;
                if (progress_cb) progress_cb(total_written, content_length, progress_ctx);
            } else {
                if (chunk_len + (size_t)rc > sizeof(chunk_buf)) {
                    puts("install: chunk buffer overflow");
                    (void)eyn_sys_net_tcp_close();
                    return -1;
                }
                memcpy(chunk_buf + chunk_len, rx_buf, (size_t)rc);
                chunk_len += (size_t)rc;
            }
        }

        if (chunked) {
            while (!chunk_done) {
                if (!chunk_have_size) {
                    size_t line_end = 0;
                    while (line_end + 1 < chunk_len) {
                        if (chunk_buf[line_end] == '\r' && chunk_buf[line_end + 1] == '\n') break;
                        line_end++;
                    }
                    if (line_end + 1 >= chunk_len) break;

                    if (pkg_parse_hex_size((const char*)chunk_buf, line_end, &chunk_need) != 0) {
                        puts("install: invalid chunk size");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }

                    size_t consume = line_end + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 1;

                    if (chunk_need == 0) {
                        chunk_done = 1;
                        break;
                    }
                }

                if (chunk_have_size) {
                    if (chunk_len < chunk_need + 2) break;

                    if (writer(chunk_buf, chunk_need, writer_ctx) != 0) {
                        puts("install: body write failed");
                        (void)eyn_sys_net_tcp_close();
                        return -1;
                    }
                    total_written += chunk_need;
                    if (progress_cb) progress_cb(total_written, content_length, progress_ctx);

                    size_t consume = chunk_need + 2;
                    memmove(chunk_buf, chunk_buf + consume, chunk_len - consume);
                    chunk_len -= consume;
                    chunk_have_size = 0;
                    chunk_need = 0;
                }
            }
        }

        if (!chunked && content_length >= 0 && total_written >= (size_t)content_length) {
            break;
        }
        if (chunked && chunk_done) break;
    }

    (void)eyn_sys_net_tcp_close();

    if (chunked && !chunk_done) {
        puts("install: incomplete chunked transfer");
        return -1;
    }

    *out_bytes = total_written;
    return 0;
}

static int pkg_http_get_stream(const char* url,
                               pkg_body_writer_fn writer,
                               void* writer_ctx,
                               size_t* out_bytes,
                               pkg_progress_update_fn progress_cb,
                               void* progress_ctx) {
    if (!url || !writer || !out_bytes) return -1;

    enum { PKG_REDIRECT_TRACK_CAP = PKG_HTTP_MAX_REDIRECTS + 2 };
    char redirect_history[PKG_REDIRECT_TRACK_CAP][MAX_URL];
    int redirect_history_count = 0;

    char current_url[MAX_URL];
    if (pkg_normalize_url_host(url, current_url, sizeof(current_url)) != 0) {
        puts("install: URL too long");
        return -1;
    }

    char original_private_http_url[MAX_URL];
    original_private_http_url[0] = '\0';
    char original_private_host[PKG_HTTP_MAX_HOST];
    original_private_host[0] = '\0';
    int tried_original_private_http = 0;
    int preserve_private_redirects = 0;

    pkg_http_url_t original_parts;
    pkg_http_url_t normalized_parts;
    if (PKG_ALLOW_PRIVATE_HOST_FALLBACK
        && pkg_parse_http_url(url, &original_parts) == 0
        && pkg_parse_http_url(current_url, &normalized_parts) == 0
        && original_parts.is_https
        && pkg_host_is_private_ipv4_literal(original_parts.host)
        && strcmp(original_parts.host, normalized_parts.host) != 0
        && pkg_can_http_fallback(&original_parts)
        && pkg_build_http_fallback_url(&original_parts,
                                       original_private_http_url,
                                       sizeof(original_private_http_url)) == 0) {
        strncpy(original_private_host, original_parts.host, sizeof(original_private_host) - 1);
        original_private_host[sizeof(original_private_host) - 1] = '\0';
    }

    for (int redirects = 0; redirects <= PKG_HTTP_MAX_REDIRECTS; redirects++) {
        for (int i = 0; i < redirect_history_count; i++) {
            if (strcmp(redirect_history[i], current_url) == 0) {
                if (!tried_original_private_http
                    && original_private_http_url[0]
                    && strcmp(current_url, original_private_http_url) != 0) {
                    printf("install: redirect loop on rewritten host; trying original private HTTP endpoint %s\n",
                           original_private_host[0] ? original_private_host : "(unknown)");
                    strncpy(current_url, original_private_http_url, sizeof(current_url) - 1);
                    current_url[sizeof(current_url) - 1] = '\0';
                    tried_original_private_http = 1;
                    preserve_private_redirects = 1;
                    redirect_history_count = 0;
                    continue;
                }
                printf("install: redirect loop detected for %s\n", current_url);
                return -1;
            }
        }
        if (redirect_history_count < PKG_REDIRECT_TRACK_CAP) {
            strncpy(redirect_history[redirect_history_count],
                    current_url,
                    sizeof(redirect_history[0]) - 1);
            redirect_history[redirect_history_count][sizeof(redirect_history[0]) - 1] = '\0';
            redirect_history_count++;
        }

        char redirect_url[MAX_URL];
        redirect_url[0] = '\0';

        int rc = pkg_http_get_stream_once(current_url,
                                          writer,
                                          writer_ctx,
                                          out_bytes,
                                          redirect_url,
                                          sizeof(redirect_url),
                                          progress_cb,
                                          progress_ctx);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            if (!tried_original_private_http
                && original_private_http_url[0]
                && strcmp(current_url, original_private_http_url) != 0) {
                printf("install: rewritten-host fetch failed; trying original private HTTP endpoint %s\n",
                       original_private_host[0] ? original_private_host : "(unknown)");
                strncpy(current_url, original_private_http_url, sizeof(current_url) - 1);
                current_url[sizeof(current_url) - 1] = '\0';
                tried_original_private_http = 1;
                preserve_private_redirects = 1;
                redirect_history_count = 0;
                continue;
            }
            return -1;
        }

        int next_url_rc = preserve_private_redirects
            ? pkg_copy_url_literal(redirect_url, current_url, sizeof(current_url))
            : pkg_normalize_url_host(redirect_url, current_url, sizeof(current_url));
        if (next_url_rc != 0) {
            puts("install: redirect URL is too long");
            return -1;
        }
    }

    puts("install: too many redirects");
    return -1;
}

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
    size_t max_bytes;
} pkg_mem_sink_t;

static int pkg_mem_sink_write(const uint8_t* data, size_t len, void* ctx) {
    pkg_mem_sink_t* sink = (pkg_mem_sink_t*)ctx;
    if (!sink || !data) return -1;

    if (len > sink->max_bytes || sink->len > sink->max_bytes - len) {
        puts("install: download exceeded max allowed size");
        return -1;
    }

    size_t needed = sink->len + len;
    while (needed > sink->cap) {
        size_t next = sink->cap * 2;
        if (next < needed) next = needed;
        if (next > sink->max_bytes) next = sink->max_bytes;
        if (next < needed) {
            puts("install: download buffer limit reached");
            return -1;
        }

        uint8_t* bigger = (uint8_t*)realloc(sink->data, next + 1);
        if (!bigger) {
            puts("install: out of memory buffering download");
            return -1;
        }

        sink->data = bigger;
        sink->cap = next;
    }

    memcpy(sink->data + sink->len, data, len);
    sink->len += len;
    return 0;
}

int package_download_url_to_buffer(const char* url,
                                   uint8_t** out_data,
                                   size_t* out_len,
                                   size_t max_bytes) {
    if (!url || !out_data || !out_len) return -1;

    if (max_bytes == 0) {
        max_bytes = PKG_DOWNLOAD_DEFAULT_MAX;
    }

    size_t initial_cap = 32768u;
    if (initial_cap > max_bytes) initial_cap = max_bytes;
    if (initial_cap < 4096u) initial_cap = max_bytes;

    pkg_mem_sink_t sink;
    sink.data = (uint8_t*)malloc(initial_cap + 1u);
    sink.len = 0;
    sink.cap = initial_cap;
    sink.max_bytes = max_bytes;
    if (!sink.data) return -1;

    size_t downloaded = 0;
    if (pkg_http_get_stream(url,
                            pkg_mem_sink_write,
                            &sink,
                            &downloaded,
                            NULL,
                            NULL) != 0) {
        free(sink.data);
        return -1;
    }

    sink.data[sink.len] = '\0';
    *out_data = sink.data;
    *out_len = sink.len;
    (void)downloaded;
    return 0;
}

static uint32_t pkg_sha_rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t pkg_sha_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t pkg_sha_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t pkg_sha_big_sigma0(uint32_t x) {
    return pkg_sha_rotr32(x, 2u) ^ pkg_sha_rotr32(x, 13u) ^ pkg_sha_rotr32(x, 22u);
}

static uint32_t pkg_sha_big_sigma1(uint32_t x) {
    return pkg_sha_rotr32(x, 6u) ^ pkg_sha_rotr32(x, 11u) ^ pkg_sha_rotr32(x, 25u);
}

static uint32_t pkg_sha_small_sigma0(uint32_t x) {
    return pkg_sha_rotr32(x, 7u) ^ pkg_sha_rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t pkg_sha_small_sigma1(uint32_t x) {
    return pkg_sha_rotr32(x, 17u) ^ pkg_sha_rotr32(x, 19u) ^ (x >> 10u);
}

static uint32_t pkg_sha_read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | ((uint32_t)p[3]);
}

static void pkg_sha_write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void pkg_sha256_transform(pkg_sha256_ctx_t* ctx, const uint8_t block[PKG_SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = pkg_sha_read_be32(block + (size_t)i * 4u);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = pkg_sha_small_sigma1(w[i - 2]) + w[i - 7] + pkg_sha_small_sigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + pkg_sha_big_sigma1(e) + pkg_sha_ch(e, f, g) + pkg_sha256_k[i] + w[i];
        uint32_t t2 = pkg_sha_big_sigma0(a) + pkg_sha_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void pkg_sha256_init(pkg_sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buffer_len = 0;
}

static void pkg_sha256_update(pkg_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    ctx->total_len += (uint64_t)len;

    size_t off = 0;
    while (off < len) {
        size_t space = PKG_SHA256_BLOCK_SIZE - ctx->buffer_len;
        size_t take = len - off;
        if (take > space) take = space;

        memcpy(ctx->buffer + ctx->buffer_len, data + off, take);
        ctx->buffer_len += take;
        off += take;

        if (ctx->buffer_len == PKG_SHA256_BLOCK_SIZE) {
            pkg_sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void pkg_sha256_final(pkg_sha256_ctx_t* ctx, uint8_t out_digest[PKG_SHA256_DIGEST_SIZE]) {
    uint64_t bits = ctx->total_len * 8u;

    ctx->buffer[ctx->buffer_len++] = 0x80;

    if (ctx->buffer_len > 56) {
        while (ctx->buffer_len < PKG_SHA256_BLOCK_SIZE) {
            ctx->buffer[ctx->buffer_len++] = 0;
        }
        pkg_sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    while (ctx->buffer_len < 56) {
        ctx->buffer[ctx->buffer_len++] = 0;
    }

    for (int i = 7; i >= 0; --i) {
        ctx->buffer[ctx->buffer_len++] = (uint8_t)(bits >> (i * 8));
    }

    pkg_sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        pkg_sha_write_be32(out_digest + (size_t)i * 4u, ctx->state[i]);
    }
}

static void pkg_sha256_hex(const uint8_t digest[PKG_SHA256_DIGEST_SIZE], char out_hex[MAX_SHA]) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < PKG_SHA256_DIGEST_SIZE; i++) {
        out_hex[(size_t)i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out_hex[(size_t)i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
}

static int pkg_sha256_match_ci(const char* expected, const char* got) {
    if (!expected || !got) return 0;
    if (strlen(expected) != 64 || strlen(got) != 64) return 0;

    for (int i = 0; i < 64; i++) {
        if (pkg_ascii_lower(expected[i]) != pkg_ascii_lower(got[i])) return 0;
    }

    return 1;
}

typedef struct {
    int stream_handle;
    pkg_sha256_ctx_t sha;
    size_t total_written;
} pkg_install_sink_t;

typedef struct {
    const char* label;
    int shown;
    int last_percent;
    uint32_t last_tick_ms;
    size_t last_bytes;
    long total_bytes_hint;
} pkg_progress_bar_t;

static int pkg_install_sink_write(const uint8_t* data, size_t len, void* ctx) {
    pkg_install_sink_t* sink = (pkg_install_sink_t*)ctx;
    if (!sink || !data) return -1;

    if (eynfs_stream_write(sink->stream_handle, data, len) != (ssize_t)len) {
        return -1;
    }

    pkg_sha256_update(&sink->sha, data, len);
    sink->total_written += len;
    return 0;
}

static size_t pkg_progress_kib(size_t bytes) {
    return (bytes + 1023u) / 1024u;
}

static uint32_t pkg_progress_kib_u32(size_t bytes) {
    size_t kib = pkg_progress_kib(bytes);
    if (kib > 0xFFFFFFFFu) kib = 0xFFFFFFFFu;
    return (uint32_t)kib;
}

static void pkg_progress_make_label(const char* in, char out[PKG_PROGRESS_LABEL_CAP]) {
    if (!out) return;

    const char* src = (in && in[0]) ? in : "package";
    int i = 0;
    while (src[i] && i + 1 < PKG_PROGRESS_LABEL_CAP) {
        char c = src[i];
        if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        out[i] = c;
        i++;
    }
    out[i] = '\0';
}

static void pkg_progress_bar_draw(pkg_progress_bar_t* bar,
                                  size_t downloaded,
                                  long total_bytes,
                                  int force) {
    if (!bar) return;
    if (total_bytes >= 0) bar->total_bytes_hint = total_bytes;

    uint32_t now_ms = (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);

    int percent = -1;
    if (total_bytes > 0) {
        size_t total = (size_t)total_bytes;
        if (downloaded >= total) percent = 100;
        else percent = (int)((downloaded * 100u) / total);
    }

    if (!force && bar->shown && percent == bar->last_percent) {
        uint32_t elapsed = (uint32_t)(now_ms - bar->last_tick_ms);
        size_t delta = downloaded - bar->last_bytes;
        if (elapsed < PKG_PROGRESS_UPDATE_MS && delta < PKG_PROGRESS_UPDATE_BYTES) {
            return;
        }
    }

    char gauge[PKG_PROGRESS_BAR_WIDTH + 1];
    int fill = 0;
    if (percent >= 0) {
        fill = (percent * PKG_PROGRESS_BAR_WIDTH) / 100;
    } else {
        fill = (int)((downloaded / 4096u) % (size_t)(PKG_PROGRESS_BAR_WIDTH + 1));
    }
    if (fill > PKG_PROGRESS_BAR_WIDTH) fill = PKG_PROGRESS_BAR_WIDTH;
    for (int i = 0; i < PKG_PROGRESS_BAR_WIDTH; ++i) {
        gauge[i] = (i < fill) ? '#' : '-';
    }
    gauge[PKG_PROGRESS_BAR_WIDTH] = '\0';

    char label[PKG_PROGRESS_LABEL_CAP];
    pkg_progress_make_label(bar->label, label);

    uint32_t dl_kib = pkg_progress_kib_u32(downloaded);
    if (total_bytes > 0) {
        uint32_t total_kib = pkg_progress_kib_u32((size_t)total_bytes);
        printf("\rinstall: %s [%s] %d%% %u/%u KiB      ",
               label,
               gauge,
               percent,
               dl_kib,
               total_kib);
    } else {
        printf("\rinstall: %s [%s] %u KiB      ",
               label,
               gauge,
               dl_kib);
    }
    fflush(stdout);

    bar->shown = 1;
    bar->last_percent = percent;
    bar->last_tick_ms = now_ms;
    bar->last_bytes = downloaded;
}

static void pkg_progress_bar_update(size_t downloaded, long total_bytes, void* ctx) {
    pkg_progress_bar_t* bar = (pkg_progress_bar_t*)ctx;
    if (!bar) return;
    pkg_progress_bar_draw(bar, downloaded, total_bytes, 0);
}

static void pkg_progress_bar_finish(pkg_progress_bar_t* bar, size_t downloaded, int success) {
    if (!bar || !bar->shown) return;

    if (success) {
        long total = bar->total_bytes_hint;
        if (total > 0 && downloaded > (size_t)total) total = (long)downloaded;
        pkg_progress_bar_draw(bar, downloaded, total, 1);
    }

    putchar('\n');
    fflush(stdout);
    bar->shown = 0;
}

static int pkg_get_install_dir(const Package* pkg, char out_dir[PKG_INSTALL_PATH_CAP]) {
    if (!pkg || !out_dir) return -1;

    const char* raw = pkg->install_dir[0] ? pkg->install_dir : "/binaries";
    int needed = snprintf(out_dir, PKG_INSTALL_PATH_CAP, "%s", raw);
    if (needed <= 0 || needed >= PKG_INSTALL_PATH_CAP) return -1;

    size_t len = strlen(out_dir);
    while (len > 1 && out_dir[len - 1] == '/') {
        out_dir[len - 1] = '\0';
        len--;
    }

    if (!pkg_install_dir_is_valid(out_dir)) return -1;
    return 0;
}

static int pkg_get_install_name(const Package* pkg, char out_name[MAX_NAME]) {
    if (!pkg || !out_name) return -1;

    const char* raw = pkg->install_name[0] ? pkg->install_name : pkg->name;
    int needed = snprintf(out_name, MAX_NAME, "%s", raw);
    if (needed <= 0 || needed >= MAX_NAME) return -1;
    if (!pkg_install_name_is_valid(out_name)) return -1;
    return 0;
}

static int pkg_prepare_install_dir(const Package* pkg,
                                   const char* install_dir) {
    if (!pkg || !install_dir || !install_dir[0]) return -1;

    if (!pkg_is_first_level_dir(install_dir)) {
        char warning[200];
        int needed = snprintf(warning,
                              sizeof(warning),
                              "target directory '%s' is not first-level",
                              install_dir);
        if (needed <= 0 || needed >= (int)sizeof(warning)) return -1;
        if (!pkg_prompt_confirm(warning)) {
            puts("install: cancelled by user");
            return -1;
        }
    }

    if (pkg_path_exists(install_dir) && !pkg_path_is_directory(install_dir)) {
        printf("install: target path is not a directory: %s\n", install_dir);
        return -1;
    }

    if (!pkg_path_is_directory(install_dir)) {
        char warning[220];
        int needed = snprintf(warning,
                              sizeof(warning),
                              "target directory '%s' does not exist and will be created",
                              install_dir);
        if (needed <= 0 || needed >= (int)sizeof(warning)) return -1;
        if (!pkg_prompt_confirm(warning)) {
            puts("install: cancelled by user");
            return -1;
        }
        if (pkg_mkdir_recursive(install_dir) != 0) {
            printf("install: failed to create install directory: %s\n", install_dir);
            return -1;
        }
    }

    return 0;
}

static int pkg_build_install_path(const Package* pkg,
                                  char out_path[PKG_INSTALL_PATH_CAP]) {
    if (!pkg || !out_path) return -1;

    char install_dir[PKG_INSTALL_PATH_CAP];
    char install_name[MAX_NAME];
    if (pkg_get_install_dir(pkg, install_dir) != 0) return -1;
    if (pkg_get_install_name(pkg, install_name) != 0) return -1;

    int needed = snprintf(out_path, PKG_INSTALL_PATH_CAP, "%s/%s", install_dir, install_name);
    if (needed <= 0 || needed >= PKG_INSTALL_PATH_CAP) return -1;
    return 0;
}

static int pkg_records_equal(const Package* left, const Package* right) {
    if (!left || !right) return 0;
    if (strcmp(left->name, right->name) != 0) return 0;
    if (strcmp(left->version, right->version) != 0) return 0;
    if (strcmp(left->url, right->url) != 0) return 0;
    if (strcmp(left->sha256, right->sha256) != 0) return 0;
    if (strcmp(left->install_dir, right->install_dir) != 0) return 0;
    if (strcmp(left->install_name, right->install_name) != 0) return 0;
    if (left->system != right->system) return 0;
    return 1;
}

static int pkg_has_suffix_ci(const char* value, const char* suffix) {
    if (!value || !suffix) return 0;

    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > value_len) return 0;

    const char* at = value + value_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        if (pkg_ascii_lower(at[i]) != pkg_ascii_lower(suffix[i])) return 0;
    }
    return 1;
}

static int pkg_url_is_archive(const char* url) {
    if (!url) return 0;
    return pkg_has_suffix_ci(url, ".tar")
        || pkg_has_suffix_ci(url, ".tar.gz")
        || pkg_has_suffix_ci(url, ".tgz");
}

static int pkg_path_exists(const char* path) {
    if (!path || !path[0]) return 0;
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int pkg_build_cache_archive_name(const Package* pkg,
                                        char out_name[PKG_INSTALL_PATH_CAP]) {
    if (!pkg || !out_name || !pkg->name[0]) return -1;

    int needed = snprintf(out_name,
                          PKG_INSTALL_PATH_CAP,
                          "%s.pkg",
                          pkg->name);
    if (needed <= 0 || needed >= PKG_INSTALL_PATH_CAP) return -1;
    if (needed > PKG_EYNFS_MAX_NAME_CHARS) return -1;
    return 0;
}

static int pkg_build_cached_archive_path(const Package* pkg,
                                         char out_path[PKG_TEMP_PATH_CAP]) {
    if (!pkg || !out_path) return -1;

    char archive_name[PKG_INSTALL_PATH_CAP];
    if (pkg_build_cache_archive_name(pkg, archive_name) != 0) {
        return -1;
    }

    int needed = snprintf(out_path,
                          PKG_TEMP_PATH_CAP,
                          "%s/%s",
                          PKG_LOCAL_PACKAGE_CACHE_DIR,
                          archive_name);
    if (needed <= 0 || needed >= PKG_TEMP_PATH_CAP) return -1;
    return 0;
}

static int pkg_ensure_dir_exists(const char* path) {
    if (!path || !path[0]) return -1;

    if (pkg_path_exists(path)) return 0;
    if (mkdir(path, 0) == 0) return 0;
    return pkg_path_exists(path) ? 0 : -1;
}

static int pkg_build_temp_archive_path(const char* name,
                                       char out_path[PKG_TEMP_PATH_CAP]) {
    if (!name || !name[0] || !out_path) return -1;

    int needed = snprintf(out_path,
                          PKG_TEMP_PATH_CAP,
                          "/tmp/install-%s.pkg",
                          name);
    if (needed <= 0 || needed >= PKG_TEMP_PATH_CAP) return -1;
    return 0;
}

static int pkg_measure_file_size(const char* path, size_t* out_size) {
    if (!path || !out_size) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    size_t total = 0;
    uint8_t buf[PKG_IO_CHUNK];
    for (;;) {
        ssize_t rd = read(fd, buf, sizeof(buf));
        if (rd < 0) {
            close(fd);
            return -1;
        }
        if (rd == 0) break;
        total += (size_t)rd;
    }

    close(fd);
    *out_size = total;
    return 0;
}

static int pkg_copy_file_to_path(const char* src_path, const char* dst_path) {
    if (!src_path || !dst_path) return -1;

    int src_fd = open(src_path, O_RDONLY, 0);
    if (src_fd < 0) return -1;

    int stream_handle = eynfs_stream_begin(dst_path);
    if (stream_handle < 0) {
        close(src_fd);
        return -1;
    }

    uint8_t buf[PKG_IO_CHUNK];
    for (;;) {
        ssize_t rd = read(src_fd, buf, sizeof(buf));
        if (rd < 0) {
            (void)close(src_fd);
            (void)eynfs_stream_end(stream_handle);
            (void)unlink(dst_path);
            return -1;
        }
        if (rd == 0) break;

        if (eynfs_stream_write(stream_handle, buf, (size_t)rd) != rd) {
            (void)close(src_fd);
            (void)eynfs_stream_end(stream_handle);
            (void)unlink(dst_path);
            return -1;
        }
    }

    (void)close(src_fd);

    if (eynfs_stream_end(stream_handle) != 0) {
        (void)unlink(dst_path);
        return -1;
    }

    return 0;
}

static int pkg_build_media_archive_path(const Package* pkg,
                                        char out_path[PKG_TEMP_PATH_CAP]) {
    if (!pkg || !out_path) return -1;

    char archive_name[PKG_INSTALL_PATH_CAP];
    if (pkg_build_cache_archive_name(pkg, archive_name) != 0) {
        return -1;
    }

    int needed = snprintf(out_path,
                          PKG_TEMP_PATH_CAP,
                          "%s/%s",
                          PKG_INSTALLER_MEDIA_PACKAGE_CACHE_DIR,
                          archive_name);
    if (needed <= 0 || needed >= PKG_TEMP_PATH_CAP) return -1;
    return 0;
}

static int pkg_extract_archive_embedded(const char* archive_path, const char* dest_dir) {
    if (!archive_path || !dest_dir) return -1;

    char* embedded_argv[3];
    embedded_argv[0] = (char*)"extract";
    embedded_argv[1] = (char*)archive_path;
    embedded_argv[2] = (char*)dest_dir;

    int embedded_rc = install_embedded_extract_main(3, embedded_argv);
    if (embedded_rc != 0) {
        printf("install: embedded extract failed (status=%d)\n", embedded_rc);
        return -1;
    }

    return 0;
}

static int pkg_extract_archive(const char* archive_path, const char* dest_dir) {
    if (!archive_path || !dest_dir) return -1;

#ifdef PKG_EXTRACT_EMBEDDED_ONLY
    return pkg_extract_archive_embedded(archive_path, dest_dir);
#endif

    const char* argv_local[2];
    argv_local[0] = archive_path;
    argv_local[1] = dest_dir;

    int pid = spawn("/binaries/extract", argv_local, 2);
    if (pid <= 0) {
        pid = spawn("extract", argv_local, 2);
    }

    if (pid <= 0) {
        return pkg_extract_archive_embedded(archive_path, dest_dir);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) <= 0) {
        puts("install: failed to wait for extract command");
        return -1;
    }
    if (status != 0) {
        printf("install: extract failed (status=%d)\n", status);
        return -1;
    }

    return 0;
}

static int pkg_download_verified_to_path(const Package* pkg,
                                         const char* out_path,
                                         size_t* out_bytes) {
    if (!pkg || !out_path) return -1;

    int stream_handle = eynfs_stream_begin(out_path);
    if (stream_handle < 0) {
        printf("install: failed to create %s\n", out_path);
        return -1;
    }

    pkg_install_sink_t sink;
    sink.stream_handle = stream_handle;
    sink.total_written = 0;
    pkg_sha256_init(&sink.sha);

    pkg_progress_bar_t progress;
    memset(&progress, 0, sizeof(progress));
    progress.label = pkg->name;
    progress.last_percent = -1;
    progress.total_bytes_hint = -1;

    printf("install: downloading %s\n", pkg->name[0] ? pkg->name : "package");
    fflush(stdout);

    pkg_progress_bar_draw(&progress, 0, -1, 1);

    size_t downloaded = 0;
    if (pkg_http_get_stream(pkg->url,
                            pkg_install_sink_write,
                            &sink,
                            &downloaded,
                            pkg_progress_bar_update,
                            &progress) != 0) {
        pkg_progress_bar_finish(&progress, sink.total_written, 0);
        (void)eynfs_stream_end(stream_handle);
        (void)unlink(out_path);
        return -1;
    }

    pkg_progress_bar_finish(&progress, sink.total_written, 1);

    if (eynfs_stream_end(stream_handle) != 0) {
        (void)unlink(out_path);
        return -1;
    }

    uint8_t digest[PKG_SHA256_DIGEST_SIZE];
    char digest_hex[MAX_SHA];
    pkg_sha256_final(&sink.sha, digest);
    pkg_sha256_hex(digest, digest_hex);

    if (!pkg_sha256_match_ci(pkg->sha256, digest_hex)) {
        printf("install: checksum mismatch for %s\n", pkg->name);
        printf("install: expected %s\n", pkg->sha256);
        printf("install: got      %s\n", digest_hex);
        (void)unlink(out_path);
        return -1;
    }

    if (sink.total_written != downloaded) {
        printf("install: short write for %s\n", pkg->name);
        (void)unlink(out_path);
        return -1;
    }

    if (out_bytes) *out_bytes = sink.total_written;
    return 0;
}

static int pkg_sha256_file_hex(const char* path, char out_hex[MAX_SHA]) {
    if (!path || !out_hex) return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    pkg_sha256_ctx_t sha;
    pkg_sha256_init(&sha);

    uint8_t buf[PKG_IO_CHUNK];
    for (;;) {
        ssize_t rd = read(fd, buf, sizeof(buf));
        if (rd < 0) {
            close(fd);
            return -1;
        }
        if (rd == 0) break;

        pkg_sha256_update(&sha, buf, (size_t)rd);
    }

    close(fd);

    uint8_t digest[PKG_SHA256_DIGEST_SIZE];
    pkg_sha256_final(&sha, digest);
    pkg_sha256_hex(digest, out_hex);
    return 0;
}

static int pkg_try_seed_local_cache_from_base_archive(void) {
    static int seed_attempted = 0;
    if (seed_attempted) return 0;
    seed_attempted = 1;

    if (!pkg_path_exists(PKG_LOCAL_BASE_ARCHIVE)) {
        return 0;
    }

    if (pkg_ensure_dir_exists(PKG_LOCAL_CACHE_ROOT) != 0
        || pkg_ensure_dir_exists(PKG_LOCAL_PACKAGE_CACHE_DIR) != 0) {
        puts("install: failed to prepare local cache directories");
        return 0;
    }

    puts("install: unpacking local base cache ...");
    if (pkg_extract_archive(PKG_LOCAL_BASE_ARCHIVE, PKG_LOCAL_PACKAGE_CACHE_DIR) != 0) {
        puts("install: failed to unpack local base cache; falling back to per-package media cache");
        return 0;
    }

    return 0;
}

static int pkg_try_seed_archive_from_installer_media(const Package* pkg,
                                                     const char* cache_archive_path) {
    if (!pkg || !cache_archive_path) return -1;
    if (!pkg->system) return 1;
    if (!PKG_ALLOW_INSTALLER_MEDIA_SEED) return 1;

    char media_archive_path[PKG_TEMP_PATH_CAP];
    if (pkg_build_media_archive_path(pkg, media_archive_path) != 0) {
        return 1;
    }

    if (!pkg_path_exists(media_archive_path)) {
        return 1;
    }

    if (pkg_ensure_dir_exists(PKG_LOCAL_CACHE_ROOT) != 0
        || pkg_ensure_dir_exists(PKG_LOCAL_PACKAGE_CACHE_DIR) != 0) {
        printf("install: failed to prepare cache for %s\n", pkg->name);
        return 1;
    }

    if (pkg_copy_file_to_path(media_archive_path, cache_archive_path) != 0) {
        printf("install: failed to seed %s from installer media\n", pkg->name);
        return 1;
    }

    printf("install: seeded cached archive for %s from installer media\n", pkg->name);
    return 0;
}

static int pkg_try_install_from_local_cache(const Package* pkg,
                                            const char* out_path,
                                            const char* install_dir,
                                            size_t* out_bytes) {
    if (!pkg || !out_path || !install_dir) return -1;
    if (!pkg->system) return 1;

    int installer_media_pkg_cache_available =
    PKG_ALLOW_INSTALLER_MEDIA_SEED
    && pkg_path_exists(PKG_INSTALLER_MEDIA_PACKAGE_CACHE_DIR);

    char cache_archive_path[PKG_TEMP_PATH_CAP];
    if (pkg_build_cached_archive_path(pkg, cache_archive_path) != 0) {
        return 1;
    }

    if (!pkg_path_exists(cache_archive_path)) {
        (void)pkg_try_seed_archive_from_installer_media(pkg, cache_archive_path);
    }

    if (!pkg_path_exists(cache_archive_path)) {
        if (!installer_media_pkg_cache_available) {
            (void)pkg_try_seed_local_cache_from_base_archive();
        }
    }

    if (!pkg_path_exists(cache_archive_path)) {
        return 1;
    }

    char digest_hex[MAX_SHA];
    if (pkg_sha256_file_hex(cache_archive_path, digest_hex) != 0) {
        printf("install: failed to hash cached archive for %s\n", pkg->name);
        return 1;
    }

    if (!pkg_sha256_match_ci(pkg->sha256, digest_hex)) {
        printf("install: cached archive checksum mismatch for %s; using network\n", pkg->name);
        return 1;
    }

    if (pkg_extract_archive(cache_archive_path, install_dir) != 0) {
        printf("install: cached archive extraction failed for %s; using network\n", pkg->name);
        return 1;
    }

    if (!pkg_path_exists(out_path)) {
        printf("install: cached archive missing payload for %s; using network\n", pkg->name);
        return 1;
    }

    if (pkg_measure_file_size(out_path, out_bytes) != 0) {
        printf("install: failed to inspect cached payload for %s\n", pkg->name);
        return -1;
    }

    printf("install: using cached package for %s\n", pkg->name);
    return 0;
}

int install_package(const struct PackageIndex* index, const Package* pkg) {
    if (!index || !pkg || !pkg->name[0]) return -1;

    const Package* in_index = index_find_package((const PackageIndex*)index, pkg->name);
    if (!in_index) {
        printf("install: package '%s' not found in index\n", pkg->name);
        return -1;
    }

    if (!pkg_records_equal(in_index, pkg)) {
        printf("install: package metadata mismatch for %s\n", pkg->name);
        return -1;
    }

    if (strlen(pkg->sha256) != 64) {
        printf("install: invalid sha256 for %s\n", pkg->name);
        return -1;
    }

    char out_path[PKG_INSTALL_PATH_CAP];
    char install_dir[PKG_INSTALL_PATH_CAP];
    char install_name[MAX_NAME];
    if (pkg_get_install_dir(pkg, install_dir) != 0
        || pkg_get_install_name(pkg, install_name) != 0
        || pkg_build_install_path(pkg, out_path) != 0) {
        printf("install: output path too long for %s\n", pkg->name);
        return -1;
    }

    if (pkg_prepare_install_dir(pkg, install_dir) != 0) {
        return -1;
    }

    size_t installed_bytes = 0;
    if (pkg_url_is_archive(pkg->url)) {
        int cache_rc = pkg_try_install_from_local_cache(pkg, out_path, install_dir, &installed_bytes);
        if (cache_rc == 0) {
            printf("install: installed %s@%s (%lu bytes)\n",
                   pkg->name,
                   pkg->version,
                   (unsigned long)installed_bytes);
            return 0;
        }

        char archive_path[PKG_TEMP_PATH_CAP];

        if (pkg_build_temp_archive_path(pkg->name, archive_path) != 0) {
            printf("install: temporary path too long for %s\n", pkg->name);
            return -1;
        }

        if (pkg_ensure_dir_exists("/tmp") != 0 || pkg_ensure_dir_exists(install_dir) != 0) {
            puts("install: failed to prepare temporary install directory");
            return -1;
        }

        if (pkg_download_verified_to_path(pkg, archive_path, NULL) != 0) {
            printf("install: download failed for %s\n", pkg->name);
            return -1;
        }

        if (pkg_extract_archive(archive_path, install_dir) != 0) {
            (void)unlink(archive_path);
            printf("install: extraction failed for %s\n", pkg->name);
            return -1;
        }

        if (!pkg_path_exists(out_path)) {
            if (strcmp(install_name, pkg->name) != 0) {
                char legacy_path[PKG_INSTALL_PATH_CAP];
                int legacy_needed = snprintf(legacy_path,
                                             PKG_INSTALL_PATH_CAP,
                                             "%s/%s",
                                             install_dir,
                                             pkg->name);
                if (legacy_needed > 0
                    && legacy_needed < PKG_INSTALL_PATH_CAP
                    && pkg_path_exists(legacy_path)
                    && pkg_copy_file_to_path(legacy_path, out_path) == 0) {
                    (void)unlink(legacy_path);
                }
            }
        }

        if (!pkg_path_exists(out_path)) {
            (void)unlink(archive_path);
            printf("install: no extracted payload found for %s\n", pkg->name);
            printf("install: package archive did not provide %s/%s\n", install_dir, install_name);
            return -1;
        }

        if (pkg_measure_file_size(out_path, &installed_bytes) != 0) {
            (void)unlink(archive_path);
            printf("install: failed to inspect extracted payload for %s\n", pkg->name);
            return -1;
        }

        (void)unlink(archive_path);
    } else {
        if (pkg_download_verified_to_path(pkg, out_path, &installed_bytes) != 0) {
            printf("install: download failed for %s\n", pkg->name);
            return -1;
        }
    }

    printf("install: installed %s@%s (%lu bytes)\n",
           pkg->name,
           pkg->version,
           (unsigned long)installed_bytes);
    return 0;
}
