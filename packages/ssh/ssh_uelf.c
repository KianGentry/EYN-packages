#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <mbedtls/aes.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>

#include <gui.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("SSH client with password auth and interactive shell relay.",
               "ssh user@192.168.1.10");

// Compatibility declaration in case an older userland header is picked up.
int spawn_ex(const char* path,
             const char* const* argv,
             int argc,
             int stdin_fd,
             int stdout_fd,
             int stderr_fd,
             int inherit_mode);

#ifndef WNOHANG
#define WNOHANG 1
#endif

#define SSH_MAX_TARGET 192
#define SSH_MAX_USER   64
#define SSH_MAX_HOST   128
#define SSH_MAX_LINE   512
#define SSH_MAX_PACKET 4096
#define SSH_MAX_ROUNDTRIP_PKTS 8
#define SSH_TCP_STREAM_BUF 8192
#define SSH_MAX_IDENT  256
#define SSH_MAX_KEXINIT_PAYLOAD 2048
#define SSH_MAX_HOSTKEY_BLOB 2048
#define SSH_MAX_EPHEMERAL_BLOB 512
#define SSH_SHA256_DIGEST_SIZE 32
#define SSH_AES_BLOCK_SIZE 16
#define SSH_MAX_MAC_LEN 64

static const char* SSH_KEX_ALGS =
    "curve25519-sha256,curve25519-sha256@libssh.org,"
    "diffie-hellman-group14-sha256,diffie-hellman-group14-sha1";
static const char* SSH_HOSTKEY_ALGS =
    "rsa-sha2-512,rsa-sha2-256,ssh-rsa,ssh-ed25519";
static const char* SSH_CIPHER_ALGS = "aes128-ctr,aes256-ctr";
static const char* SSH_MAC_ALGS =
    "hmac-sha2-256,hmac-sha2-512,hmac-sha1";
static const char* SSH_COMP_ALGS = "none";

typedef struct {
    char user[SSH_MAX_USER];
    char host[SSH_MAX_HOST];
    uint16_t port;
} ssh_target_t;

static uint8_t g_tcp_stream_buf[SSH_TCP_STREAM_BUF];
static int g_tcp_stream_off = 0;
static int g_tcp_stream_len = 0;
static char g_negotiated_kex[96];
static char g_negotiated_c2s_cipher[96];
static char g_negotiated_s2c_cipher[96];
static char g_negotiated_c2s_mac[96];
static char g_negotiated_s2c_mac[96];
static char g_client_ident[SSH_MAX_IDENT];
static int g_client_ident_len = 0;
static char g_server_ident[SSH_MAX_IDENT];
static int g_server_ident_len = 0;
static uint8_t g_client_kexinit_payload[SSH_MAX_KEXINIT_PAYLOAD];
static int g_client_kexinit_payload_len = 0;
static uint8_t g_server_kexinit_payload[SSH_MAX_KEXINIT_PAYLOAD];
static int g_server_kexinit_payload_len = 0;
static uint8_t g_hostkey_blob[SSH_MAX_HOSTKEY_BLOB];
static int g_hostkey_blob_len = 0;
static uint8_t g_client_ephemeral[SSH_MAX_EPHEMERAL_BLOB];
static int g_client_ephemeral_len = 0;
static uint8_t g_server_ephemeral[SSH_MAX_EPHEMERAL_BLOB];
static int g_server_ephemeral_len = 0;
static uint8_t g_shared_secret[SSH_MAX_EPHEMERAL_BLOB];
static int g_shared_secret_len = 0;
static uint8_t g_exchange_hash[SSH_SHA256_DIGEST_SIZE];
static int g_exchange_hash_len = 0;
static uint8_t g_session_id[SSH_SHA256_DIGEST_SIZE];
static int g_session_id_len = 0;
static mbedtls_ecdh_context g_curve25519_ctx;
static mbedtls_ecp_keypair g_curve25519_keypair;
static int g_curve25519_ready = 0;
static int g_transport_encrypted = 0;
static uint32_t g_send_seq = 0;
static uint32_t g_recv_seq = 0;
static int g_send_mac_len = 0;
static int g_recv_mac_len = 0;

static mbedtls_aes_context g_send_aes;
static mbedtls_aes_context g_recv_aes;
static size_t g_send_nc_off = 0;
static size_t g_recv_nc_off = 0;
static unsigned char g_send_nonce_counter[SSH_AES_BLOCK_SIZE];
static unsigned char g_recv_nonce_counter[SSH_AES_BLOCK_SIZE];
static unsigned char g_send_stream_block[SSH_AES_BLOCK_SIZE];
static unsigned char g_recv_stream_block[SSH_AES_BLOCK_SIZE];
static uint8_t g_send_mac_key[SSH_MAX_MAC_LEN];
static uint8_t g_recv_mac_key[SSH_MAX_MAC_LEN];

typedef struct {
    int esc;
    int csi;
    int osc;
    int osc_esc;
} ssh_ansi_filter_t;

static ssh_ansi_filter_t g_stdout_ansi;
static ssh_ansi_filter_t g_stderr_ansi;

typedef struct {
    uint64_t bit_len;
    uint32_t state[8];
    uint8_t buffer[64];
    uint32_t buffer_len;
} ssh_sha256_ctx_t;

static void be32_write(uint8_t out[4], uint32_t v);
static void ssh_transport_buffer_compact(void);

static const uint32_t g_sha256_k[64] = {
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
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(ssh_sha256_ctx_t* ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
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
        uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + g_sha256_k[i] + w[i];
        uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
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

static void sha256_init(ssh_sha256_ctx_t* ctx) {
    if (!ctx) return;
    ctx->bit_len = 0;
    ctx->buffer_len = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(ssh_sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (!ctx || !data || len == 0) return;
    ctx->bit_len += (uint64_t)len * 8u;

    size_t off = 0;
    while (off < len) {
        size_t take = 64u - ctx->buffer_len;
        if (take > (len - off)) take = len - off;
        memcpy(&ctx->buffer[ctx->buffer_len], &data[off], take);
        ctx->buffer_len += (uint32_t)take;
        off += take;

        if (ctx->buffer_len == 64u) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

static void sha256_final(ssh_sha256_ctx_t* ctx, uint8_t out_digest[SSH_SHA256_DIGEST_SIZE]) {
    if (!ctx || !out_digest) return;

    ctx->buffer[ctx->buffer_len++] = 0x80u;
    if (ctx->buffer_len > 56u) {
        while (ctx->buffer_len < 64u) ctx->buffer[ctx->buffer_len++] = 0;
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0;
    }

    while (ctx->buffer_len < 56u) ctx->buffer[ctx->buffer_len++] = 0;
    for (int i = 7; i >= 0; --i) {
        ctx->buffer[ctx->buffer_len++] = (uint8_t)((ctx->bit_len >> (i * 8)) & 0xFFu);
    }
    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        out_digest[i * 4] = (uint8_t)((ctx->state[i] >> 24) & 0xFFu);
        out_digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xFFu);
        out_digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xFFu);
        out_digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFu);
    }
}

static void hash_string_field(ssh_sha256_ctx_t* h, const uint8_t* p, int n) {
    uint8_t len_be[4];
    if (!h || (!p && n > 0) || n < 0) return;
    be32_write(len_be, (uint32_t)n);
    sha256_update(h, len_be, sizeof(len_be));
    if (n > 0) sha256_update(h, p, (size_t)n);
}

static void hash_shared_secret_field(ssh_sha256_ctx_t* h, const uint8_t* secret_octets, int secret_len) {
    uint8_t mpint_buf[SSH_MAX_EPHEMERAL_BLOB + 1];

    if (!h || !secret_octets || secret_len <= 0) return;

    /* RFC8731: reinterpret X25519 octets as network-byte-order integer for mpint K. */
    int used = secret_len;
    memcpy(mpint_buf, secret_octets, (size_t)secret_len);

    int off = 0;
    while (off < used && mpint_buf[off] == 0) off++;
    if (off >= used) {
        hash_string_field(h, NULL, 0);
        return;
    }

    const uint8_t* mpint = &mpint_buf[off];
    int mpint_len = used - off;
    if ((mpint[0] & 0x80u) != 0) {
        uint8_t zero = 0;
        uint8_t len_be[4];
        be32_write(len_be, (uint32_t)(mpint_len + 1));
        sha256_update(h, len_be, sizeof(len_be));
        sha256_update(h, &zero, 1);
        sha256_update(h, mpint, (size_t)mpint_len);
        return;
    }

    hash_string_field(h, mpint, mpint_len);
}

static void ssh_hmac_sha256(const uint8_t* key,
                            int key_len,
                            const uint8_t* data1,
                            int data1_len,
                            const uint8_t* data2,
                            int data2_len,
                            uint8_t out[SSH_SHA256_DIGEST_SIZE]) {
    uint8_t k0[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[SSH_SHA256_DIGEST_SIZE];
    uint8_t khash[SSH_SHA256_DIGEST_SIZE];

    memset(k0, 0, sizeof(k0));
    if (key_len > (int)sizeof(k0)) {
        ssh_sha256_ctx_t kctx;
        sha256_init(&kctx);
        sha256_update(&kctx, key, (size_t)key_len);
        sha256_final(&kctx, khash);
        memcpy(k0, khash, sizeof(khash));
    } else if (key && key_len > 0) {
        memcpy(k0, key, (size_t)key_len);
    }

    for (int i = 0; i < (int)sizeof(k0); ++i) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
        opad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    }

    ssh_sha256_ctx_t hctx;
    sha256_init(&hctx);
    sha256_update(&hctx, ipad, sizeof(ipad));
    if (data1 && data1_len > 0) sha256_update(&hctx, data1, (size_t)data1_len);
    if (data2 && data2_len > 0) sha256_update(&hctx, data2, (size_t)data2_len);
    sha256_final(&hctx, inner);

    sha256_init(&hctx);
    sha256_update(&hctx, opad, sizeof(opad));
    sha256_update(&hctx, inner, sizeof(inner));
    sha256_final(&hctx, out);
}

static int ssh_consttime_equal(const uint8_t* a, const uint8_t* b, int len) {
    if (!a || !b || len < 0) return 0;
    uint8_t diff = 0;
    for (int i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void ssh_transport_reset(void) {
    g_transport_encrypted = 0;
    g_send_seq = 0;
    g_recv_seq = 0;
    g_send_mac_len = 0;
    g_recv_mac_len = 0;
    g_send_nc_off = 0;
    g_recv_nc_off = 0;
    memset(g_send_nonce_counter, 0, sizeof(g_send_nonce_counter));
    memset(g_recv_nonce_counter, 0, sizeof(g_recv_nonce_counter));
    memset(g_send_stream_block, 0, sizeof(g_send_stream_block));
    memset(g_recv_stream_block, 0, sizeof(g_recv_stream_block));
    memset(g_send_mac_key, 0, sizeof(g_send_mac_key));
    memset(g_recv_mac_key, 0, sizeof(g_recv_mac_key));

    mbedtls_aes_free(&g_send_aes);
    mbedtls_aes_free(&g_recv_aes);
    mbedtls_aes_init(&g_send_aes);
    mbedtls_aes_init(&g_recv_aes);
}

static int ssh_transport_install_keys(const uint8_t* iv_c2s,
                                      int iv_c2s_len,
                                      const uint8_t* iv_s2c,
                                      int iv_s2c_len,
                                      const uint8_t* enc_c2s,
                                      int enc_c2s_len,
                                      const uint8_t* enc_s2c,
                                      int enc_s2c_len,
                                      const uint8_t* mac_c2s,
                                      int mac_c2s_len,
                                      const uint8_t* mac_s2c,
                                      int mac_s2c_len) {
    if (!iv_c2s || !iv_s2c || !enc_c2s || !enc_s2c || !mac_c2s || !mac_s2c) return -1;
    if (strcmp(g_negotiated_c2s_mac, "hmac-sha2-256") != 0 ||
        strcmp(g_negotiated_s2c_mac, "hmac-sha2-256") != 0) {
        puts("ssh: only hmac-sha2-256 is implemented for transport MAC");
        return -1;
    }

    if (iv_c2s_len < SSH_AES_BLOCK_SIZE || iv_s2c_len < SSH_AES_BLOCK_SIZE) return -1;
    if (enc_c2s_len != 16 && enc_c2s_len != 32) return -1;
    if (enc_s2c_len != 16 && enc_s2c_len != 32) return -1;
    if (mac_c2s_len < SSH_SHA256_DIGEST_SIZE || mac_s2c_len < SSH_SHA256_DIGEST_SIZE) return -1;

    uint32_t send_seq = g_send_seq;
    uint32_t recv_seq = g_recv_seq;
    ssh_transport_reset();
    g_send_seq = send_seq;
    g_recv_seq = recv_seq;

    memcpy(g_send_nonce_counter, iv_c2s, SSH_AES_BLOCK_SIZE);
    memcpy(g_recv_nonce_counter, iv_s2c, SSH_AES_BLOCK_SIZE);
    memcpy(g_send_mac_key, mac_c2s, SSH_SHA256_DIGEST_SIZE);
    memcpy(g_recv_mac_key, mac_s2c, SSH_SHA256_DIGEST_SIZE);
    g_send_mac_len = SSH_SHA256_DIGEST_SIZE;
    g_recv_mac_len = SSH_SHA256_DIGEST_SIZE;

    int ret = mbedtls_aes_setkey_enc(&g_send_aes, enc_c2s, (unsigned)(enc_c2s_len * 8));
    if (ret != 0) {
        printf("ssh: failed to set c2s AES key (%d)\n", ret);
        return -1;
    }
    ret = mbedtls_aes_setkey_enc(&g_recv_aes, enc_s2c, (unsigned)(enc_s2c_len * 8));
    if (ret != 0) {
        printf("ssh: failed to set s2c AES key (%d)\n", ret);
        return -1;
    }

    g_transport_encrypted = 1;
    printf("ssh: transport seq start send=%u recv=%u\n", g_send_seq, g_recv_seq);
    puts("ssh: encrypted transport enabled (aes-ctr + hmac-sha2-256)");
    return 0;
}

static int ssh_rng(void* ctx, unsigned char* out, size_t len) {
    (void)ctx;

    static uint32_t state = 0;
    if (state == 0) {
        uint32_t seed = (uint32_t)eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
        state = seed ? (seed ^ 0x9E3779B9u) : 0xA341316Cu;
    }

    for (size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] = (uint8_t)(state & 0xFFu);
    }

    return 0;
}

static void ssh_curve25519_reset(void) {
    if (g_curve25519_ready) {
        mbedtls_ecdh_free(&g_curve25519_ctx);
        mbedtls_ecp_keypair_free(&g_curve25519_keypair);
        g_curve25519_ready = 0;
    }

    mbedtls_ecdh_init(&g_curve25519_ctx);
    mbedtls_ecp_keypair_init(&g_curve25519_keypair);
}

static int ssh_curve25519_prepare_keypair(void) {
    int ret = 0;
    size_t public_len = 0;

    if (g_curve25519_ready) return 0;

    ret = mbedtls_ecp_group_load(&g_curve25519_keypair.grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret != 0) {
        printf("ssh: curve25519 group load failed (%d)\n", ret);
        return ret;
    }

    ret = mbedtls_ecp_gen_keypair(&g_curve25519_keypair.grp,
                                  &g_curve25519_keypair.d,
                                  &g_curve25519_keypair.Q,
                                  ssh_rng,
                                  NULL);
    if (ret != 0) {
        printf("ssh: curve25519 key generation failed (%d)\n", ret);
        return ret;
    }

    ret = mbedtls_ecdh_get_params(&g_curve25519_ctx, &g_curve25519_keypair, MBEDTLS_ECDH_OURS);
    if (ret != 0) {
        printf("ssh: curve25519 context setup failed (%d)\n", ret);
        return ret;
    }

    ret = mbedtls_ecp_point_write_binary(&g_curve25519_keypair.grp,
                                         &g_curve25519_keypair.Q,
                                         MBEDTLS_ECP_PF_UNCOMPRESSED,
                                         &public_len,
                                         g_client_ephemeral,
                                         sizeof(g_client_ephemeral));
    if (ret != 0) {
        printf("ssh: curve25519 public export failed (%d)\n", ret);
        return ret;
    }

    if (public_len != 32) {
        printf("ssh: curve25519 public key length was %u\n", (unsigned)public_len);
        return -1;
    }

    g_client_ephemeral_len = (int)public_len;
    g_curve25519_ready = 1;
    return 0;
}

static void print_hex_prefix(const char* label, const uint8_t* p, int n, int prefix_bytes) {
    if (!label || !p || n <= 0 || prefix_bytes <= 0) return;
    if (prefix_bytes > n) prefix_bytes = n;
    printf("ssh: %s=", label);
    for (int i = 0; i < prefix_bytes; ++i) printf("%02x", (unsigned)p[i]);
    if (prefix_bytes < n) puts("..."); else puts("");
}

static int ssh_derive_exchange_hash_and_keys(void) {
    if (g_client_ident_len <= 0 || g_server_ident_len <= 0) return -1;
    if (g_client_kexinit_payload_len <= 0 || g_server_kexinit_payload_len <= 0) return -1;
    if (g_hostkey_blob_len <= 0 || g_client_ephemeral_len <= 0 || g_server_ephemeral_len <= 0) return -1;

    if (strncmp(g_negotiated_kex, "curve25519-", 11) == 0) {
        size_t secret_len = 0;
        int ret = mbedtls_ecdh_calc_secret(&g_curve25519_ctx,
                                           &secret_len,
                                           g_shared_secret,
                                           sizeof(g_shared_secret),
                                           ssh_rng,
                                           NULL);
        if (ret != 0) {
            printf("ssh: curve25519 shared secret failed (%d)\n", ret);
            return -1;
        }
        g_shared_secret_len = (int)secret_len;
    } else {
        puts("ssh: negotiated key exchange is not implemented yet");
        return -1;
    }

    ssh_sha256_ctx_t hctx;
    sha256_init(&hctx);

    hash_string_field(&hctx, (const uint8_t*)g_client_ident, g_client_ident_len);
    hash_string_field(&hctx, (const uint8_t*)g_server_ident, g_server_ident_len);
    hash_string_field(&hctx, g_client_kexinit_payload, g_client_kexinit_payload_len);
    hash_string_field(&hctx, g_server_kexinit_payload, g_server_kexinit_payload_len);
    hash_string_field(&hctx, g_hostkey_blob, g_hostkey_blob_len);
    hash_string_field(&hctx, g_client_ephemeral, g_client_ephemeral_len);
    hash_string_field(&hctx, g_server_ephemeral, g_server_ephemeral_len);
    hash_shared_secret_field(&hctx, g_shared_secret, g_shared_secret_len);

    sha256_final(&hctx, g_exchange_hash);
    g_exchange_hash_len = SSH_SHA256_DIGEST_SIZE;

    if (g_session_id_len == 0) {
        memcpy(g_session_id, g_exchange_hash, SSH_SHA256_DIGEST_SIZE);
        g_session_id_len = SSH_SHA256_DIGEST_SIZE;
    }

    print_hex_prefix("exchange_hash(H)", g_exchange_hash, g_exchange_hash_len, 16);
    print_hex_prefix("session_id", g_session_id, g_session_id_len, 16);

    {
        uint8_t key_iv_c2s[32], key_iv_s2c[32];
        uint8_t key_enc_c2s[32], key_enc_s2c[32];
        uint8_t key_mac_c2s[64], key_mac_s2c[64];
        int iv_c2s_need = SSH_AES_BLOCK_SIZE;
        int iv_s2c_need = SSH_AES_BLOCK_SIZE;
        int enc_c2s_need = (strcmp(g_negotiated_c2s_cipher, "aes256-ctr") == 0) ? 32 : 16;
        int enc_s2c_need = (strcmp(g_negotiated_s2c_cipher, "aes256-ctr") == 0) ? 32 : 16;
        int mac_c2s_need = (strcmp(g_negotiated_c2s_mac, "hmac-sha2-512") == 0) ? 64 : 32;
        int mac_s2c_need = (strcmp(g_negotiated_s2c_mac, "hmac-sha2-512") == 0) ? 64 : 32;

        struct deriv_req {
            uint8_t* out;
            int out_len;
            char ch;
        } reqs[] = {
            { key_iv_c2s,  iv_c2s_need,  'A' },
            { key_iv_s2c,  iv_s2c_need,  'B' },
            { key_enc_c2s, enc_c2s_need, 'C' },
            { key_enc_s2c, enc_s2c_need, 'D' },
            { key_mac_c2s, mac_c2s_need, 'E' },
            { key_mac_s2c, mac_s2c_need, 'F' },
        };

        for (int i = 0; i < (int)(sizeof(reqs) / sizeof(reqs[0])); ++i) {
            int written = 0;
            int round = 0;
            uint8_t last_block[SSH_SHA256_DIGEST_SIZE];
            while (written < reqs[i].out_len) {
                ssh_sha256_ctx_t dctx;
                sha256_init(&dctx);
                hash_shared_secret_field(&dctx, g_shared_secret, g_shared_secret_len);
                sha256_update(&dctx, g_exchange_hash, (size_t)g_exchange_hash_len);

                if (round == 0) {
                    uint8_t c = (uint8_t)reqs[i].ch;
                    sha256_update(&dctx, &c, 1);
                    sha256_update(&dctx, g_session_id, (size_t)g_session_id_len);
                } else {
                    sha256_update(&dctx, reqs[i].out, (size_t)written);
                }

                sha256_final(&dctx, last_block);
                int take = reqs[i].out_len - written;
                if (take > SSH_SHA256_DIGEST_SIZE) take = SSH_SHA256_DIGEST_SIZE;
                memcpy(reqs[i].out + written, last_block, (size_t)take);
                written += take;
                round++;
            }
        }

        print_hex_prefix("kdf iv_c2s", key_iv_c2s, iv_c2s_need, 12);
        print_hex_prefix("kdf iv_s2c", key_iv_s2c, iv_s2c_need, 12);
        print_hex_prefix("kdf enc_c2s", key_enc_c2s, enc_c2s_need, 12);
        print_hex_prefix("kdf enc_s2c", key_enc_s2c, enc_s2c_need, 12);
        print_hex_prefix("kdf mac_c2s", key_mac_c2s, mac_c2s_need, 12);
        print_hex_prefix("kdf mac_s2c", key_mac_s2c, mac_s2c_need, 12);

        if (ssh_transport_install_keys(key_iv_c2s,
                                       iv_c2s_need,
                                       key_iv_s2c,
                                       iv_s2c_need,
                                       key_enc_c2s,
                                       enc_c2s_need,
                                       key_enc_s2c,
                                       enc_s2c_need,
                                       key_mac_c2s,
                                       mac_c2s_need,
                                       key_mac_s2c,
                                       mac_s2c_need) != 0) {
            return -1;
        }
    }

    return 0;
}

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

    if (attempts_max <= 0) attempts_max = 1;

    int used = 0;
    int attempts = 0;
    while (attempts < attempts_max && used < out_cap - 1) {
        if (g_tcp_stream_off >= g_tcp_stream_len) {
            int rc = eyn_sys_net_tcp_recv(g_tcp_stream_buf, (uint32_t)sizeof(g_tcp_stream_buf));
            if (rc > 0) {
                g_tcp_stream_off = 0;
                g_tcp_stream_len = rc;
            } else {
                if (rc == -2) break;
                attempts++;
                (void)usleep(10000);
                continue;
            }
        }

        char ch = 0;
        ch = (char)g_tcp_stream_buf[g_tcp_stream_off++];
        out[used++] = ch;
        if (ch == '\n') break;
    }

    out[used] = '\0';
    return (used > 0) ? used : -1;
}

static int tcp_recv_exact(void* out, int need, int attempts_max) {
    if (!out || need < 0) return -1;
    if (need == 0) return 0;

    if (attempts_max <= 0) attempts_max = 1;

    int got = 0;
    int attempts = 0;
    while (got < need && attempts < attempts_max) {
        if (g_tcp_stream_off >= g_tcp_stream_len) {
            int rc = eyn_sys_net_tcp_recv(g_tcp_stream_buf, (uint32_t)sizeof(g_tcp_stream_buf));
            if (rc > 0) {
                g_tcp_stream_off = 0;
                g_tcp_stream_len = rc;
            } else {
                if (rc == -2) return -1;
                attempts++;
                (void)usleep(10000);
                continue;
            }
        }

        int avail = g_tcp_stream_len - g_tcp_stream_off;
        int take = need - got;
        if (take > avail) take = avail;
        memcpy((uint8_t*)out + got, &g_tcp_stream_buf[g_tcp_stream_off], (size_t)take);
        g_tcp_stream_off += take;
        got += take;
    }
    return (got == need) ? 0 : -1;
}

static void tcp_stream_reset(void) {
    g_tcp_stream_off = 0;
    g_tcp_stream_len = 0;
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

static int be32_read_at(const uint8_t* p, int len, int off, uint32_t* out) {
    if (!p || !out || len < 0 || off < 0 || off + 4 > len) return -1;
    *out = ((uint32_t)p[off] << 24) |
           ((uint32_t)p[off + 1] << 16) |
           ((uint32_t)p[off + 2] << 8) |
           (uint32_t)p[off + 3];
    return 0;
}

static int parse_namelist_field(const uint8_t* p,
                                int len,
                                int* off_io,
                                char* out,
                                int out_cap) {
    if (!p || !off_io || !out || out_cap <= 0) return -1;
    uint32_t n = 0;
    int off = *off_io;
    if (be32_read_at(p, len, off, &n) != 0) return -1;
    off += 4;
    if ((uint32_t)len < (uint32_t)off + n) return -1;
    int copy_n = (int)n;
    if (copy_n >= out_cap) copy_n = out_cap - 1;
    if (copy_n > 0) memcpy(out, &p[off], (size_t)copy_n);
    out[copy_n] = '\0';
    off += (int)n;
    *off_io = off;
    return 0;
}

static int csv_has_token(const char* csv, const char* token, int token_len) {
    if (!csv || !token || token_len <= 0) return 0;
    const char* p = csv;
    while (*p) {
        const char* start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        if (len == token_len && strncmp(start, token, (size_t)len) == 0) return 1;
        if (*p == ',') p++;
    }
    return 0;
}

static int pick_first_common(const char* preferred_csv,
                             const char* offered_csv,
                             char* out,
                             int out_cap) {
    if (!preferred_csv || !offered_csv || !out || out_cap <= 1) return -1;
    const char* p = preferred_csv;
    while (*p) {
        const char* start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        if (len > 0 && len < out_cap && csv_has_token(offered_csv, start, len)) {
            memcpy(out, start, (size_t)len);
            out[len] = '\0';
            return 0;
        }
        if (*p == ',') p++;
    }
    return -1;
}

static void ssh_print_csv_prefix(const char* label, const char* csv, int max_chars) {
    if (!label || !csv || max_chars <= 0) return;
    int n = (int)strlen(csv);
    if (n > max_chars) n = max_chars;
    printf("ssh: %s=%.*s%s\n", label, n, csv, (int)strlen(csv) > n ? "..." : "");
}

static int ssh_send_packet(const uint8_t* payload, int payload_len, uint32_t seed) {
    if (!payload || payload_len <= 0 || payload_len > (SSH_MAX_PACKET - 32)) return -1;

    int block_size = g_transport_encrypted ? SSH_AES_BLOCK_SIZE : 8;
    int padding_len = 4;
    while (((1 + payload_len + padding_len + 4) % block_size) != 0) padding_len++;
    if (padding_len < 4) padding_len = 4;

    int packet_len = 1 + payload_len + padding_len;
    int total_len = 4 + packet_len;
    if (total_len > SSH_MAX_PACKET || total_len < block_size) return -1;

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

    if (!g_transport_encrypted) {
        if (tcp_send_all(packet, total_len) != 0) return -1;
        g_send_seq++;
        return 0;
    }

    uint8_t seq_be[4];
    uint8_t mac[SSH_SHA256_DIGEST_SIZE];
    be32_write(seq_be, g_send_seq);
    ssh_hmac_sha256(g_send_mac_key,
                    g_send_mac_len,
                    seq_be,
                    sizeof(seq_be),
                    packet,
                    total_len,
                    mac);

    uint8_t encrypted[SSH_MAX_PACKET];
    memcpy(encrypted, packet, (size_t)total_len);

    int ret = mbedtls_aes_crypt_ctr(&g_send_aes,
                                    (size_t)total_len,
                                    &g_send_nc_off,
                                    g_send_nonce_counter,
                                    g_send_stream_block,
                                    encrypted,
                                    encrypted);
    if (ret != 0) {
        printf("ssh: packet encrypt failed (%d)\n", ret);
        return -1;
    }

    if (tcp_send_all(encrypted, total_len) != 0) return -1;
    if (tcp_send_all(mac, g_send_mac_len) != 0) return -1;
    g_send_seq++;
    return 0;
}

static int ssh_recv_packet(uint8_t* payload_out, int payload_cap, int* payload_len_out, int* msg_id_out) {
    if (!payload_out || payload_cap <= 0 || !payload_len_out || !msg_id_out) return -1;

    if (!g_transport_encrypted) {
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
        g_recv_seq++;
        return 0;
    }

    uint8_t first_block[SSH_AES_BLOCK_SIZE];
    if (tcp_recv_exact(first_block, sizeof(first_block), 800) != 0) return -1;

    uint8_t packet[SSH_MAX_PACKET];
    int ret = mbedtls_aes_crypt_ctr(&g_recv_aes,
                                    sizeof(first_block),
                                    &g_recv_nc_off,
                                    g_recv_nonce_counter,
                                    g_recv_stream_block,
                                    first_block,
                                    packet);
    if (ret != 0) {
        printf("ssh: packet decrypt(header) failed (%d)\n", ret);
        return -1;
    }

    uint32_t packet_len = be32_read(packet);
    if (packet_len < 2 || packet_len > (uint32_t)(SSH_MAX_PACKET - 4)) return -1;

    int total_len = 4 + (int)packet_len;
    if (total_len < SSH_AES_BLOCK_SIZE || (total_len % SSH_AES_BLOCK_SIZE) != 0) return -1;

    int remain = total_len - SSH_AES_BLOCK_SIZE;
    if (remain > 0) {
        if (tcp_recv_exact(packet + SSH_AES_BLOCK_SIZE, remain, 800) != 0) return -1;
        ret = mbedtls_aes_crypt_ctr(&g_recv_aes,
                                    (size_t)remain,
                                    &g_recv_nc_off,
                                    g_recv_nonce_counter,
                                    g_recv_stream_block,
                                    packet + SSH_AES_BLOCK_SIZE,
                                    packet + SSH_AES_BLOCK_SIZE);
        if (ret != 0) {
            printf("ssh: packet decrypt(body) failed (%d)\n", ret);
            return -1;
        }
    }

    uint8_t mac_wire[SSH_MAX_MAC_LEN];
    if (g_recv_mac_len <= 0 || g_recv_mac_len > (int)sizeof(mac_wire)) return -1;
    if (tcp_recv_exact(mac_wire, g_recv_mac_len, 800) != 0) return -1;

    uint8_t seq_be[4];
    uint8_t mac_calc[SSH_SHA256_DIGEST_SIZE];
    be32_write(seq_be, g_recv_seq);
    ssh_hmac_sha256(g_recv_mac_key,
                    g_recv_mac_len,
                    seq_be,
                    sizeof(seq_be),
                    packet,
                    total_len,
                    mac_calc);
    if (!ssh_consttime_equal(mac_wire, mac_calc, g_recv_mac_len)) {
        puts("ssh: packet MAC verification failed");
        return -1;
    }

    uint8_t padding_len = packet[4];
    int payload_len = (int)packet_len - 1 - (int)padding_len;
    if (payload_len <= 0 || payload_len > payload_cap) return -1;

    memcpy(payload_out, packet + 5, (size_t)payload_len);
    *payload_len_out = payload_len;
    *msg_id_out = payload_out[0];
    g_recv_seq++;
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
        SSH_KEX_ALGS,
        SSH_HOSTKEY_ALGS,
        SSH_CIPHER_ALGS,
        SSH_CIPHER_ALGS,
        SSH_MAC_ALGS,
        SSH_MAC_ALGS,
        SSH_COMP_ALGS,
        SSH_COMP_ALGS,
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

    if (off > (int)sizeof(g_client_kexinit_payload)) return -1;
    memcpy(g_client_kexinit_payload, payload, (size_t)off);
    g_client_kexinit_payload_len = off;

    return ssh_send_packet(payload, off, seed ^ 0x53534821u);
}

static int ssh_parse_server_kexinit(const uint8_t* payload, int payload_len) {
    if (!payload || payload_len < 1 + 16 + 4) return -1;
    if (payload[0] != 20) {
        printf("ssh: expected SSH_MSG_KEXINIT (20), got %u\n", (unsigned)payload[0]);
        return -1;
    }

    int off = 1 + 16; // msg id + cookie
    char server_kex[1024], server_hostkey[1024];
    char server_c2s_cipher[1024], server_s2c_cipher[1024];
    char server_c2s_mac[1024], server_s2c_mac[1024];
    char server_c2s_comp[256], server_s2c_comp[256];
    char lang_c2s[128], lang_s2c[128];

    if (parse_namelist_field(payload, payload_len, &off, server_kex, sizeof(server_kex)) != 0) {
        puts("ssh: parse error at kex_algorithms namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_hostkey, sizeof(server_hostkey)) != 0) {
        puts("ssh: parse error at server_host_key_algorithms namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_c2s_cipher, sizeof(server_c2s_cipher)) != 0) {
        puts("ssh: parse error at encryption_algorithms_client_to_server namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_s2c_cipher, sizeof(server_s2c_cipher)) != 0) {
        puts("ssh: parse error at encryption_algorithms_server_to_client namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_c2s_mac, sizeof(server_c2s_mac)) != 0) {
        puts("ssh: parse error at mac_algorithms_client_to_server namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_s2c_mac, sizeof(server_s2c_mac)) != 0) {
        puts("ssh: parse error at mac_algorithms_server_to_client namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_c2s_comp, sizeof(server_c2s_comp)) != 0) {
        puts("ssh: parse error at compression_algorithms_client_to_server namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, server_s2c_comp, sizeof(server_s2c_comp)) != 0) {
        puts("ssh: parse error at compression_algorithms_server_to_client namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, lang_c2s, sizeof(lang_c2s)) != 0) {
        puts("ssh: parse error at languages_client_to_server namelist");
        return -1;
    }
    if (parse_namelist_field(payload, payload_len, &off, lang_s2c, sizeof(lang_s2c)) != 0) {
        puts("ssh: parse error at languages_server_to_client namelist");
        return -1;
    }

    if (off + 5 > payload_len) return -1;
    uint8_t first_kex_follows = payload[off++];
    uint32_t reserved = 0;
    if (be32_read_at(payload, payload_len, off, &reserved) != 0) return -1;
    (void)reserved;

    ssh_print_csv_prefix("server kex", server_kex, 200);
    ssh_print_csv_prefix("server hostkey", server_hostkey, 200);
    ssh_print_csv_prefix("server c2s cipher", server_c2s_cipher, 200);
    ssh_print_csv_prefix("server s2c cipher", server_s2c_cipher, 200);
    ssh_print_csv_prefix("server c2s mac", server_c2s_mac, 200);
    ssh_print_csv_prefix("server s2c mac", server_s2c_mac, 200);
    ssh_print_csv_prefix("server c2s comp", server_c2s_comp, 80);
    ssh_print_csv_prefix("server s2c comp", server_s2c_comp, 80);
    printf("ssh: server first_kex_packet_follows=%u\n", (unsigned)first_kex_follows);

    char chosen_kex[96], chosen_hostkey[96];
    char chosen_c2s_cipher[96], chosen_s2c_cipher[96];
    char chosen_c2s_mac[96], chosen_s2c_mac[96];
    char chosen_c2s_comp[32], chosen_s2c_comp[32];

    if (pick_first_common(SSH_KEX_ALGS, server_kex, chosen_kex, sizeof(chosen_kex)) != 0) {
        ssh_print_csv_prefix("client kex", SSH_KEX_ALGS, 200);
        puts("ssh: no common key-exchange algorithm");
        return -1;
    }
    if (pick_first_common(SSH_HOSTKEY_ALGS, server_hostkey, chosen_hostkey, sizeof(chosen_hostkey)) != 0) {
        ssh_print_csv_prefix("client hostkey", SSH_HOSTKEY_ALGS, 200);
        puts("ssh: no common hostkey algorithm");
        return -1;
    }
    if (pick_first_common(SSH_CIPHER_ALGS, server_c2s_cipher, chosen_c2s_cipher, sizeof(chosen_c2s_cipher)) != 0) {
        ssh_print_csv_prefix("client c2s cipher", SSH_CIPHER_ALGS, 200);
        puts("ssh: no common c2s cipher algorithm");
        return -1;
    }
    if (pick_first_common(SSH_CIPHER_ALGS, server_s2c_cipher, chosen_s2c_cipher, sizeof(chosen_s2c_cipher)) != 0) {
        ssh_print_csv_prefix("client s2c cipher", SSH_CIPHER_ALGS, 200);
        puts("ssh: no common s2c cipher algorithm");
        return -1;
    }
    if (pick_first_common(SSH_MAC_ALGS, server_c2s_mac, chosen_c2s_mac, sizeof(chosen_c2s_mac)) != 0) {
        ssh_print_csv_prefix("client c2s mac", SSH_MAC_ALGS, 200);
        puts("ssh: no common c2s MAC algorithm");
        return -1;
    }
    if (pick_first_common(SSH_MAC_ALGS, server_s2c_mac, chosen_s2c_mac, sizeof(chosen_s2c_mac)) != 0) {
        ssh_print_csv_prefix("client s2c mac", SSH_MAC_ALGS, 200);
        puts("ssh: no common s2c MAC algorithm");
        return -1;
    }
    if (pick_first_common(SSH_COMP_ALGS, server_c2s_comp, chosen_c2s_comp, sizeof(chosen_c2s_comp)) != 0) {
        ssh_print_csv_prefix("client c2s comp", SSH_COMP_ALGS, 200);
        puts("ssh: no common c2s compression algorithm");
        return -1;
    }
    if (pick_first_common(SSH_COMP_ALGS, server_s2c_comp, chosen_s2c_comp, sizeof(chosen_s2c_comp)) != 0) {
        ssh_print_csv_prefix("client s2c comp", SSH_COMP_ALGS, 200);
        puts("ssh: no common s2c compression algorithm");
        return -1;
    }

    printf("ssh: negotiated kex=%s hostkey=%s\n", chosen_kex, chosen_hostkey);
    printf("ssh: negotiated cipher c2s=%s s2c=%s\n", chosen_c2s_cipher, chosen_s2c_cipher);
    printf("ssh: negotiated mac    c2s=%s s2c=%s\n", chosen_c2s_mac, chosen_s2c_mac);
    printf("ssh: negotiated comp   c2s=%s s2c=%s\n", chosen_c2s_comp, chosen_s2c_comp);

    strncpy(g_negotiated_kex, chosen_kex, sizeof(g_negotiated_kex) - 1);
    g_negotiated_kex[sizeof(g_negotiated_kex) - 1] = '\0';
    strncpy(g_negotiated_c2s_cipher, chosen_c2s_cipher, sizeof(g_negotiated_c2s_cipher) - 1);
    g_negotiated_c2s_cipher[sizeof(g_negotiated_c2s_cipher) - 1] = '\0';
    strncpy(g_negotiated_s2c_cipher, chosen_s2c_cipher, sizeof(g_negotiated_s2c_cipher) - 1);
    g_negotiated_s2c_cipher[sizeof(g_negotiated_s2c_cipher) - 1] = '\0';
    strncpy(g_negotiated_c2s_mac, chosen_c2s_mac, sizeof(g_negotiated_c2s_mac) - 1);
    g_negotiated_c2s_mac[sizeof(g_negotiated_c2s_mac) - 1] = '\0';
    strncpy(g_negotiated_s2c_mac, chosen_s2c_mac, sizeof(g_negotiated_s2c_mac) - 1);
    g_negotiated_s2c_mac[sizeof(g_negotiated_s2c_mac) - 1] = '\0';

    if (payload_len > (int)sizeof(g_server_kexinit_payload)) return -1;
    memcpy(g_server_kexinit_payload, payload, (size_t)payload_len);
    g_server_kexinit_payload_len = payload_len;

    if (first_kex_follows) {
        puts("ssh: server set first_kex_packet_follows=1; ignore handling not implemented yet.");
    }

    return 0;
}

static int ssh_send_kexdh_init(void) {
    uint8_t payload[96];
    int off = 0;
    payload[off++] = 30; // SSH_MSG_KEXDH_INIT

    if (strncmp(g_negotiated_kex, "curve25519-", 11) == 0) {
        size_t public_len = 0;
        int ret = ssh_curve25519_prepare_keypair();
        if (ret != 0) {
            return -1;
        }

        ret = mbedtls_ecp_point_write_binary(&g_curve25519_keypair.grp,
                                             &g_curve25519_keypair.Q,
                                             MBEDTLS_ECP_PF_UNCOMPRESSED,
                                             &public_len,
                                             g_client_ephemeral,
                                             sizeof(g_client_ephemeral));
        if (ret != 0 || public_len != 32) {
            printf("ssh: failed to export curve25519 client public key (%d)\n", ret);
            return -1;
        }

        g_client_ephemeral_len = (int)public_len;
        be32_write(&payload[off], (uint32_t)public_len);
        off += 4;
        memcpy(&payload[off], g_client_ephemeral, public_len);
        off += (int)public_len;
    } else {
        puts("ssh: negotiated key exchange is not implemented yet");
        return -1;
    }

    return ssh_send_packet(payload, off, 0x44483130u);
}

static int ssh_send_newkeys(void) {
    uint8_t payload[1];
    payload[0] = 21; // SSH_MSG_NEWKEYS
    return ssh_send_packet(payload, 1, 0x4E45574Bu);
}

static int ssh_send_request_failure(void) {
    uint8_t payload[1];
    payload[0] = 82; // SSH_MSG_REQUEST_FAILURE
    return ssh_send_packet(payload, 1, 0x52514641u);
}

static void ssh_handle_global_request(const uint8_t* payload, int payload_len) {
    if (!payload || payload_len < 1 + 4 + 1) return;
    if (payload[0] != 80) return; // SSH_MSG_GLOBAL_REQUEST

    int off = 1;
    uint32_t req_len = 0;
    if (be32_read_at(payload, payload_len, off, &req_len) != 0) return;
    off += 4;
    if (off + (int)req_len + 1 > payload_len) return;

    const uint8_t* req_name = &payload[off];
    off += (int)req_len;
    uint8_t want_reply = payload[off];

    if (want_reply) {
        (void)ssh_send_request_failure();
    }

    if (req_len > 0) {
        char name[96];
        int copy_n = (int)req_len;
        if (copy_n >= (int)sizeof(name)) copy_n = (int)sizeof(name) - 1;
        memcpy(name, req_name, (size_t)copy_n);
        name[copy_n] = '\0';
        printf("ssh: global request %s handled (want_reply=%u)\n", name, (unsigned)want_reply);
    }
}

static int ssh_put_string_field(uint8_t* buf,
                                int cap,
                                int* off_io,
                                const uint8_t* data,
                                int data_len) {
    if (!buf || cap <= 0 || !off_io || data_len < 0) return -1;
    int off = *off_io;
    if (off < 0 || off + 4 + data_len > cap) return -1;
    be32_write(&buf[off], (uint32_t)data_len);
    off += 4;
    if (data_len > 0 && data) {
        memcpy(&buf[off], data, (size_t)data_len);
        off += data_len;
    }
    *off_io = off;
    return 0;
}

static int ssh_send_service_request(const char* service_name) {
    if (!service_name || service_name[0] == '\0') return -1;

    uint8_t payload[128];
    int off = 0;
    payload[off++] = 5; // SSH_MSG_SERVICE_REQUEST
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)service_name,
                             (int)strlen(service_name)) != 0) {
        return -1;
    }
    return ssh_send_packet(payload, off, 0x53524356u);
}

static int ssh_read_password(char* out, int out_cap) {
    if (!out || out_cap <= 1) return -1;

    // Drain stale key events (leftover shell command typing / Enter key).
    int quiet_polls = 0;
    for (int i = 0; i < 512 && quiet_polls < 8; ++i) {
        int k = getkey();
        if (k >= 0) {
            quiet_polls = 0;
            continue;
        }
        quiet_polls++;
        (void)usleep(2000);
    }

    printf("ssh: password: ");
    fflush(stdout);

    int len = 0;
    while (len < out_cap - 1) {
        int key = getkey();
        if (key < 0) {
            (void)usleep(10000);
            continue;
        }

        if (key == '\n' || key == '\r') break;
        if (key == 8 || key == 127) {
            if (len > 0) {
                --len;
            }
            continue;
        }
        if (key < 32 || key > 126) continue;

        out[len++] = (char)key;
    }

    out[len] = '\0';
    putchar('\n');
    return (len > 0) ? 0 : -1;
}

static void ssh_write_terminal_filtered(int fd,
                                        const uint8_t* data,
                                        int len,
                                        ssh_ansi_filter_t* st) {
    if (fd < 0 || !data || len <= 0 || !st) return;

    uint8_t plain[256];
    int plain_len = 0;

    for (int i = 0; i < len; ++i) {
        uint8_t ch = data[i];

        if (st->csi) {
            if (ch >= 0x40 && ch <= 0x7e) st->csi = 0;
            continue;
        }
        if (st->osc) {
            if (ch == 0x07) {
                st->osc = 0;
                st->osc_esc = 0;
                continue;
            }
            if (st->osc_esc && ch == '\\') {
                st->osc = 0;
                st->osc_esc = 0;
                continue;
            }
            st->osc_esc = (ch == 0x1b);
            continue;
        }
        if (st->esc) {
            st->esc = 0;
            if (ch == '[') {
                st->csi = 1;
                continue;
            }
            if (ch == ']') {
                st->osc = 1;
                st->osc_esc = 0;
                continue;
            }
            continue;
        }

        if (ch == 0x1b) {
            st->esc = 1;
            continue;
        }

        plain[plain_len++] = ch;
        if (plain_len == (int)sizeof(plain)) {
            (void)write(fd, plain, (size_t)plain_len);
            plain_len = 0;
        }
    }

    if (plain_len > 0) (void)write(fd, plain, (size_t)plain_len);
}

static int ssh_send_userauth_password(const char* user_name, const char* password) {
    if (!user_name || user_name[0] == '\0' || !password) return -1;

    uint8_t payload[320];
    int off = 0;
    payload[off++] = 50; // SSH_MSG_USERAUTH_REQUEST

    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)user_name,
                             (int)strlen(user_name)) != 0) {
        return -1;
    }
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"ssh-connection",
                             (int)strlen("ssh-connection")) != 0) {
        return -1;
    }
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"password",
                             (int)strlen("password")) != 0) {
        return -1;
    }

    payload[off++] = 0; // FALSE: plain password, not password change request
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)password,
                             (int)strlen(password)) != 0) {
        return -1;
    }

    return ssh_send_packet(payload, off, 0x50574431u);
}

static int ssh_handle_userauth_reply(const uint8_t* payload, int payload_len) {
    if (!payload || payload_len <= 0) return -1;
    if (payload[0] == 52) {
        puts("ssh: userauth succeeded");
        return 0;
    }
    if (payload[0] == 60) {
        puts("ssh: server requested password change; not implemented");
        return -1;
    }
    if (payload[0] != 51) {
        printf("ssh: expected USERAUTH_SUCCESS/FAILURE, got %u\n", (unsigned)payload[0]);
        return -1;
    }

    int off = 1;
    uint32_t methods_len = 0;
    if (be32_read_at(payload, payload_len, off, &methods_len) != 0) return -1;
    off += 4;
    if (off + (int)methods_len + 1 > payload_len) return -1;

    char methods[256];
    int copy_n = (int)methods_len;
    if (copy_n >= (int)sizeof(methods)) copy_n = (int)sizeof(methods) - 1;
    memcpy(methods, &payload[off], (size_t)copy_n);
    methods[copy_n] = '\0';
    off += (int)methods_len;

    uint8_t partial = payload[off];
    printf("ssh: userauth rejected; methods=%s partial=%u\n", methods, (unsigned)partial);
    return 0;
}

static int ssh_recv_until_msg(int want_msg,
                              uint8_t* payload_out,
                              int payload_cap,
                              int* payload_len_out) {
    if (!payload_out || payload_cap <= 0 || !payload_len_out) return -1;

    for (int i = 0; i < SSH_MAX_ROUNDTRIP_PKTS; ++i) {
        int msg = -1;
        int len = 0;
        if (ssh_recv_packet(payload_out, payload_cap, &len, &msg) != 0) return -1;
        if (msg == want_msg) {
            *payload_len_out = len;
            return 0;
        }
        if (msg == 80) {
            ssh_handle_global_request(payload_out, len);
            continue;
        }
        if (msg == 53) {
            puts("ssh: received USERAUTH_BANNER");
            continue;
        }
        if (msg == 1) {
            puts("ssh: server sent disconnect during key exchange");
            return -1;
        }
    }

    return -1;
}

static int ssh_recv_userauth_terminal(uint8_t* payload_out,
                                      int payload_cap,
                                      int* payload_len_out) {
    if (!payload_out || payload_cap <= 0 || !payload_len_out) return -1;

    for (int i = 0; i < SSH_MAX_ROUNDTRIP_PKTS; ++i) {
        int msg = -1;
        int len = 0;
        if (ssh_recv_packet(payload_out, payload_cap, &len, &msg) != 0) return -1;

        if (msg == 51 || msg == 52) {
            *payload_len_out = len;
            return 0;
        }
        if (msg == 80) {
            ssh_handle_global_request(payload_out, len);
            continue;
        }
        if (msg == 53) {
            puts("ssh: received USERAUTH_BANNER");
            continue;
        }
        if (msg == 1) {
            puts("ssh: server sent disconnect during userauth");
            return -1;
        }
    }

    return -1;
}

static int ssh_put_uint32_field(uint8_t* buf, int cap, int* off_io, uint32_t value) {
    if (!buf || cap <= 0 || !off_io) return -1;
    int off = *off_io;
    if (off < 0 || off + 4 > cap) return -1;
    be32_write(&buf[off], value);
    *off_io = off + 4;
    return 0;
}

static int ssh_put_bool_field(uint8_t* buf, int cap, int* off_io, int value) {
    if (!buf || cap <= 0 || !off_io) return -1;
    int off = *off_io;
    if (off < 0 || off + 1 > cap) return -1;
    buf[off] = value ? 1u : 0u;
    *off_io = off + 1;
    return 0;
}

static int ssh_send_channel_open_session(uint32_t* remote_channel_out) {
    uint8_t payload[128];
    int off = 0;

    payload[off++] = 90; // SSH_MSG_CHANNEL_OPEN
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"session",
                             (int)strlen("session")) != 0) {
        return -1;
    }
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 0) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 0x00100000u) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 0x00008000u) != 0) return -1;

    if (ssh_send_packet(payload, off, 0x4F504E53u) != 0) return -1;

    uint8_t rx_payload[SSH_MAX_PACKET];
    for (int i = 0; i < SSH_MAX_ROUNDTRIP_PKTS * 8; ++i) {
        int msg = -1;
        int rx_len = 0;
        if (ssh_recv_packet(rx_payload, sizeof(rx_payload), &rx_len, &msg) != 0) {
            puts("ssh: failed to read SSH channel-open reply packet");
            return -1;
        }

        if (msg == 80) {
            ssh_handle_global_request(rx_payload, rx_len);
            continue;
        }

        if (msg == 91) {
            int rx_off = 1;
            uint32_t recipient = 0;
            uint32_t sender = 0;
            uint32_t window = 0;
            uint32_t max_packet = 0;
            if (be32_read_at(rx_payload, rx_len, rx_off, &recipient) != 0) return -1;
            rx_off += 4;
            if (be32_read_at(rx_payload, rx_len, rx_off, &sender) != 0) return -1;
            rx_off += 4;
            if (be32_read_at(rx_payload, rx_len, rx_off, &window) != 0) return -1;
            rx_off += 4;
            if (be32_read_at(rx_payload, rx_len, rx_off, &max_packet) != 0) return -1;

            printf("ssh: channel open confirmed recipient=%u sender=%u window=%u max_packet=%u\n",
                   (unsigned)recipient,
                   (unsigned)sender,
                   (unsigned)window,
                   (unsigned)max_packet);

            if (remote_channel_out) *remote_channel_out = sender;
            return 0;
        }

        if (msg == 92) {
            int off = 1;
            uint32_t recipient = 0;
            uint32_t reason_code = 0;
            uint32_t desc_len = 0;
            if (be32_read_at(rx_payload, rx_len, off, &recipient) != 0) return -1;
            off += 4;
            if (be32_read_at(rx_payload, rx_len, off, &reason_code) != 0) return -1;
            off += 4;
            if (be32_read_at(rx_payload, rx_len, off, &desc_len) != 0) return -1;
            off += 4;
            if (off + (int)desc_len > rx_len) return -1;

            char desc[128];
            int copy_n = (int)desc_len;
            if (copy_n >= (int)sizeof(desc)) copy_n = (int)sizeof(desc) - 1;
            if (copy_n > 0) memcpy(desc, &rx_payload[off], (size_t)copy_n);
            desc[copy_n] = '\0';

            printf("ssh: channel open failed recipient=%u reason=%u desc=%s\n",
                   (unsigned)recipient,
                   (unsigned)reason_code,
                   desc);
            return -1;
        }

        if (msg == 1) {
            puts("ssh: server disconnected during channel open");
            return -1;
        }
    }

    puts("ssh: failed to read SSH_MSG_CHANNEL_OPEN_CONFIRMATION");
    return -1;
}

static int ssh_send_channel_request_pty(uint32_t remote_channel) {
    uint8_t payload[256];
    int off = 0;

    payload[off++] = 98; // SSH_MSG_CHANNEL_REQUEST
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"pty-req",
                             (int)strlen("pty-req")) != 0) {
        return -1;
    }
    if (ssh_put_bool_field(payload, (int)sizeof(payload), &off, 0) != 0) return -1;
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"xterm",
                             (int)strlen("xterm")) != 0) {
        return -1;
    }
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 80) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 24) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 640) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, 480) != 0) return -1;
    if (ssh_put_string_field(payload, (int)sizeof(payload), &off, NULL, 0) != 0) return -1;

    return ssh_send_packet(payload, off, 0x50545930u);
}

static int ssh_send_channel_request_shell(uint32_t remote_channel) {
    uint8_t payload[64];
    int off = 0;

    payload[off++] = 98; // SSH_MSG_CHANNEL_REQUEST
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
    if (ssh_put_string_field(payload,
                             (int)sizeof(payload),
                             &off,
                             (const uint8_t*)"shell",
                             (int)strlen("shell")) != 0) {
        return -1;
    }
    if (ssh_put_bool_field(payload, (int)sizeof(payload), &off, 0) != 0) return -1;

    return ssh_send_packet(payload, off, 0x5348454Cu);
}

static int ssh_send_channel_window_adjust(uint32_t remote_channel, uint32_t bytes) {
    uint8_t payload[16];
    int off = 0;

    payload[off++] = 93; // SSH_MSG_CHANNEL_WINDOW_ADJUST
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, bytes) != 0) return -1;
    return ssh_send_packet(payload, off, 0x5741444Au);
}

static int ssh_send_channel_eof(uint32_t remote_channel) {
    uint8_t payload[8];
    int off = 0;

    payload[off++] = 96; // SSH_MSG_CHANNEL_EOF
    if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
    return ssh_send_packet(payload, off, 0x454f4630u);
}

static int ssh_key_to_ssh_bytes(int key, uint8_t* out, int out_cap) {
    if (!out || out_cap <= 0) return 0;

    if (key == '\r' || key == '\n') {
        out[0] = '\r';
        return 1;
    }
    if (key == 8 || key == 127) {
        out[0] = 127;
        return 1;
    }
    if (key == GUI_KEY_CTRL_C) {
        out[0] = 3;
        return 1;
    }
    if (key == GUI_KEY_CTRL_D) {
        out[0] = 4;
        return 1;
    }
    if (key == GUI_KEY_CTRL_H) {
        out[0] = 8;
        return 1;
    }
    if (key == GUI_KEY_CTRL_L) {
        out[0] = 12;
        return 1;
    }
    if (key == GUI_KEY_UP && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'A';
        return 3;
    }
    if (key == GUI_KEY_DOWN && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'B';
        return 3;
    }
    if (key == GUI_KEY_RIGHT && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'C';
        return 3;
    }
    if (key == GUI_KEY_LEFT && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'D';
        return 3;
    }
    if (key == GUI_KEY_HOME && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'H';
        return 3;
    }
    if (key == GUI_KEY_END && out_cap >= 3) {
        out[0] = 0x1b; out[1] = '['; out[2] = 'F';
        return 3;
    }
    if (key == GUI_KEY_DELETE && out_cap >= 4) {
        out[0] = 0x1b; out[1] = '['; out[2] = '3'; out[3] = '~';
        return 4;
    }
    if (key == GUI_KEY_PGUP && out_cap >= 4) {
        out[0] = 0x1b; out[1] = '['; out[2] = '5'; out[3] = '~';
        return 4;
    }
    if (key == GUI_KEY_PGDN && out_cap >= 4) {
        out[0] = 0x1b; out[1] = '['; out[2] = '6'; out[3] = '~';
        return 4;
    }

    if (key >= 32 && key <= 126) {
        out[0] = (uint8_t)key;
        return 1;
    }

    return 0;
}

static void ssh_transport_buffer_compact(void) {
    if (g_tcp_stream_off <= 0) return;
    if (g_tcp_stream_off >= g_tcp_stream_len) {
        g_tcp_stream_off = 0;
        g_tcp_stream_len = 0;
        return;
    }

    int remain = g_tcp_stream_len - g_tcp_stream_off;
    memmove(g_tcp_stream_buf, &g_tcp_stream_buf[g_tcp_stream_off], (size_t)remain);
    g_tcp_stream_off = 0;
    g_tcp_stream_len = remain;
}

static int ssh_transport_buffer_fill_once(void) {
    if (g_tcp_stream_off > 0) {
        ssh_transport_buffer_compact();
    }

    if (g_tcp_stream_len >= SSH_TCP_STREAM_BUF) return 0;

    int rc = eyn_sys_net_tcp_recv(g_tcp_stream_buf + g_tcp_stream_len,
                                  (uint32_t)(SSH_TCP_STREAM_BUF - g_tcp_stream_len));
    if (rc > 0) {
        g_tcp_stream_len += rc;
        return 1;
    }
    if (rc == 0) return 0;
    return rc;
}

static int ssh_try_recv_packet(uint8_t* payload_out, int payload_cap, int* payload_len_out, int* msg_id_out) {
    if (!payload_out || payload_cap <= 0 || !payload_len_out || !msg_id_out) return -1;

    int fill_rc = ssh_transport_buffer_fill_once();
    if (fill_rc == -2) return -2;
    if (fill_rc < 0) return -1;

    int avail = g_tcp_stream_len - g_tcp_stream_off;
    if (avail <= 0) return 0;

    if (!g_transport_encrypted) {
        if (avail < 5) return 0;

        uint32_t packet_len = be32_read(&g_tcp_stream_buf[g_tcp_stream_off]);
        uint8_t padding_len = g_tcp_stream_buf[g_tcp_stream_off + 4];
        if (packet_len < 2 || packet_len > (uint32_t)(SSH_MAX_PACKET - 4)) return -1;

        int total_len = 4 + (int)packet_len;
        if (avail < total_len) return 0;

        int payload_len = (int)packet_len - 1 - (int)padding_len;
        if (payload_len <= 0 || payload_len > payload_cap) return -1;

        memcpy(payload_out, &g_tcp_stream_buf[g_tcp_stream_off + 5], (size_t)payload_len);
        *payload_len_out = payload_len;
        *msg_id_out = payload_out[0];

        g_recv_seq++;
        g_tcp_stream_off += total_len;
        ssh_transport_buffer_compact();
        return 1;
    }

    if (avail < SSH_AES_BLOCK_SIZE) return 0;

    uint8_t first_block[SSH_AES_BLOCK_SIZE];
    unsigned char nonce_counter_hdr[SSH_AES_BLOCK_SIZE];
    unsigned char stream_block_hdr[SSH_AES_BLOCK_SIZE];
    size_t nc_off_hdr = g_recv_nc_off;
    memcpy(nonce_counter_hdr, g_recv_nonce_counter, sizeof(nonce_counter_hdr));
    memcpy(stream_block_hdr, g_recv_stream_block, sizeof(stream_block_hdr));

    int ret = mbedtls_aes_crypt_ctr(&g_recv_aes,
                                    SSH_AES_BLOCK_SIZE,
                                    &nc_off_hdr,
                                    nonce_counter_hdr,
                                    stream_block_hdr,
                                    &g_tcp_stream_buf[g_tcp_stream_off],
                                    first_block);
    if (ret != 0) {
        printf("ssh: packet decrypt(header) failed (%d)\n", ret);
        return -1;
    }

    uint32_t packet_len = be32_read(first_block);
    if (packet_len < 2 || packet_len > (uint32_t)(SSH_MAX_PACKET - 4)) return -1;

    int total_len = 4 + (int)packet_len;
    if (total_len < SSH_AES_BLOCK_SIZE || (total_len % SSH_AES_BLOCK_SIZE) != 0) return -1;

    int total_with_mac = total_len + g_recv_mac_len;
    if (avail < total_with_mac) return 0;

    if (total_len > SSH_MAX_PACKET) return -1;

    uint8_t packet[SSH_MAX_PACKET];
    memcpy(packet, &g_tcp_stream_buf[g_tcp_stream_off], (size_t)total_len);

    unsigned char nonce_counter_full[SSH_AES_BLOCK_SIZE];
    unsigned char stream_block_full[SSH_AES_BLOCK_SIZE];
    size_t nc_off_full = g_recv_nc_off;
    memcpy(nonce_counter_full, g_recv_nonce_counter, sizeof(nonce_counter_full));
    memcpy(stream_block_full, g_recv_stream_block, sizeof(stream_block_full));

    ret = mbedtls_aes_crypt_ctr(&g_recv_aes,
                                (size_t)total_len,
                                &nc_off_full,
                                nonce_counter_full,
                                stream_block_full,
                                packet,
                                packet);
    if (ret != 0) {
        printf("ssh: packet decrypt(full) failed (%d)\n", ret);
        return -1;
    }

    uint8_t mac_wire[SSH_MAX_MAC_LEN];
    if (g_recv_mac_len <= 0 || g_recv_mac_len > (int)sizeof(mac_wire)) return -1;
    memcpy(mac_wire, &g_tcp_stream_buf[g_tcp_stream_off + total_len], (size_t)g_recv_mac_len);

    uint8_t seq_be[4];
    uint8_t mac_calc[SSH_SHA256_DIGEST_SIZE];
    be32_write(seq_be, g_recv_seq);
    ssh_hmac_sha256(g_recv_mac_key,
                    g_recv_mac_len,
                    seq_be,
                    sizeof(seq_be),
                    packet,
                    total_len,
                    mac_calc);
    if (!ssh_consttime_equal(mac_wire, mac_calc, g_recv_mac_len)) {
        puts("ssh: packet MAC verification failed");
        return -1;
    }

    uint8_t padding_len = packet[4];
    int payload_len = (int)packet_len - 1 - (int)padding_len;
    if (payload_len <= 0 || payload_len > payload_cap) return -1;

    memcpy(payload_out, packet + 5, (size_t)payload_len);
    *payload_len_out = payload_len;
    *msg_id_out = payload_out[0];

    g_recv_seq++;
    g_recv_nc_off = nc_off_full;
    memcpy(g_recv_nonce_counter, nonce_counter_full, sizeof(g_recv_nonce_counter));
    memcpy(g_recv_stream_block, stream_block_full, sizeof(g_recv_stream_block));
    g_tcp_stream_off += total_with_mac;
    ssh_transport_buffer_compact();
    return 1;
}

static int ssh_run_session_shell(uint32_t remote_channel) {
    memset(&g_stdout_ansi, 0, sizeof(g_stdout_ansi));
    memset(&g_stderr_ansi, 0, sizeof(g_stderr_ansi));

    if (ssh_send_channel_window_adjust(remote_channel, 0x00100000u) != 0) {
        puts("ssh: failed to send initial channel window adjust");
        return -1;
    }
    puts("ssh: sent initial channel window adjust");

    if (ssh_send_channel_request_pty(remote_channel) != 0) {
        puts("ssh: failed to send pty-req");
        return -1;
    }
    puts("ssh: sent pty-req (no-reply)");

    if (ssh_send_channel_request_shell(remote_channel) != 0) {
        puts("ssh: failed to send shell request");
        return -1;
    }
    puts("ssh: sent shell request (no-reply)");

    // Nudge interactive shells to emit the first prompt consistently.
    {
        uint8_t payload[16];
        int off = 0;
        uint8_t nl = '\n';
        payload[off++] = 94; // SSH_MSG_CHANNEL_DATA
        if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
        if (ssh_put_string_field(payload,
                                 (int)sizeof(payload),
                                 &off,
                                 &nl,
                                 1) != 0) {
            return -1;
        }
        if (ssh_send_packet(payload, off, 0x50524d50u) != 0) return -1;
    }

    int stdin_closed = 0;
    while (1) {
        int did_work = 0;

        if (!stdin_closed) {
            for (int key_batch = 0; key_batch < 64; ++key_batch) {
                int key = getkey();
                if (key < 0) break;

                uint8_t key_buf[8];
                int key_len = ssh_key_to_ssh_bytes(key, key_buf, (int)sizeof(key_buf));
                if (key_len <= 0) continue;

                if (key == GUI_KEY_CTRL_D) {
                    stdin_closed = 1;
                    if (ssh_send_channel_eof(remote_channel) != 0) return -1;
                    did_work = 1;
                    break;
                }

                uint8_t payload[64];
                int off = 0;
                payload[off++] = 94; // SSH_MSG_CHANNEL_DATA
                if (ssh_put_uint32_field(payload, (int)sizeof(payload), &off, remote_channel) != 0) return -1;
                if (ssh_put_string_field(payload,
                                         (int)sizeof(payload),
                                         &off,
                                         key_buf,
                                         key_len) != 0) {
                    return -1;
                }
                if (ssh_send_packet(payload, off, 0x43484154u) != 0) return -1;
                did_work = 1;
            }
        }

        for (;;) {
            uint8_t pkt[SSH_MAX_PACKET];
            int pkt_len = 0;
            int msg = -1;
            int rc = ssh_try_recv_packet(pkt, sizeof(pkt), &pkt_len, &msg);
            if (rc == 0) break;
            if (rc < 0) return -1;

            did_work = 1;

            if (msg == 94) {
                int off = 1;
                uint32_t recipient = 0;
                uint32_t data_len = 0;
                if (be32_read_at(pkt, pkt_len, off, &recipient) != 0) return -1;
                off += 4;
                if (be32_read_at(pkt, pkt_len, off, &data_len) != 0) return -1;
                off += 4;
                if (recipient != remote_channel || off + (int)data_len > pkt_len) continue;
                if (data_len > 0) {
                    ssh_write_terminal_filtered(1, &pkt[off], (int)data_len, &g_stdout_ansi);
                }
                if (ssh_send_channel_window_adjust(remote_channel, data_len) != 0) return -1;
                continue;
            }

            if (msg == 95) {
                int off = 1;
                uint32_t recipient = 0;
                uint32_t data_len = 0;
                uint32_t code = 0;
                if (be32_read_at(pkt, pkt_len, off, &recipient) != 0) return -1;
                off += 4;
                if (be32_read_at(pkt, pkt_len, off, &code) != 0) return -1;
                off += 4;
                if (be32_read_at(pkt, pkt_len, off, &data_len) != 0) return -1;
                off += 4;
                if (recipient != remote_channel || off + (int)data_len > pkt_len) continue;
                if (data_len > 0) {
                    ssh_write_terminal_filtered(2, &pkt[off], (int)data_len, &g_stderr_ansi);
                }
                if (ssh_send_channel_window_adjust(remote_channel, data_len) != 0) return -1;
                continue;
            }

            if (msg == 98) {
                int off = 1;
                uint32_t recipient = 0;
                uint32_t req_len = 0;
                if (be32_read_at(pkt, pkt_len, off, &recipient) != 0) return -1;
                off += 4;
                if (be32_read_at(pkt, pkt_len, off, &req_len) != 0) return -1;
                off += 4;
                if (off + (int)req_len + 1 > pkt_len) return -1;

                char req_name[64];
                int copy_n = (int)req_len;
                if (copy_n >= (int)sizeof(req_name)) copy_n = (int)sizeof(req_name) - 1;
                if (copy_n > 0) memcpy(req_name, &pkt[off], (size_t)copy_n);
                req_name[copy_n] = '\0';
                  (void)recipient;
                  (void)req_name;
                continue;
            }

            if (msg == 96 || msg == 97) {
                puts("ssh: channel closed by server");
                return 0;
            }

            if (msg == 1) {
                puts("ssh: disconnect received during shell session");
                return -1;
            }
        }

        if (!did_work) {
            (void)usleep(10000);
        } else {
            (void)usleep(1000);
        }
    }
}

static int ssh_parse_kexdh_reply(const uint8_t* payload, int payload_len) {
    if (!payload || payload_len < 1 + 4 + 4 + 4) return -1;
    if (payload[0] != 31) {
        printf("ssh: expected SSH_MSG_KEXDH_REPLY (31), got %u\n", (unsigned)payload[0]);
        return -1;
    }

    int off = 1;
    uint32_t hostkey_len = 0;
    uint32_t f_len = 0;
    uint32_t sig_len = 0;

    if (be32_read_at(payload, payload_len, off, &hostkey_len) != 0) return -1;
    off += 4;
    if ((uint32_t)payload_len < (uint32_t)off + hostkey_len) return -1;

    const uint8_t* hostkey_blob = &payload[off];
    off += (int)hostkey_len;

    if ((int)hostkey_len > (int)sizeof(g_hostkey_blob)) return -1;
    memcpy(g_hostkey_blob, hostkey_blob, (size_t)hostkey_len);
    g_hostkey_blob_len = (int)hostkey_len;

    if (be32_read_at(payload, payload_len, off, &f_len) != 0) return -1;
    off += 4;
    if ((uint32_t)payload_len < (uint32_t)off + f_len) return -1;

    if ((int)f_len > (int)sizeof(g_server_ephemeral)) return -1;
    memcpy(g_server_ephemeral, &payload[off], (size_t)f_len);
    g_server_ephemeral_len = (int)f_len;

    if (strncmp(g_negotiated_kex, "curve25519-", 11) == 0) {
        if (g_server_ephemeral_len != 32) {
            printf("ssh: curve25519 server public key length was %d\n", g_server_ephemeral_len);
            return -1;
        }
        if (!g_curve25519_ready) {
            puts("ssh: curve25519 state not initialized");
            return -1;
        }
        /* mbedtls_ecdh_read_public() expects a TLS ECPoint (len byte + point). */
        uint8_t tls_point[1 + 32];
        tls_point[0] = (uint8_t)g_server_ephemeral_len;
        memcpy(&tls_point[1], g_server_ephemeral, (size_t)g_server_ephemeral_len);

        int ret = mbedtls_ecdh_read_public(&g_curve25519_ctx,
                                           tls_point,
                                           sizeof(tls_point));
        if (ret != 0) {
            printf("ssh: curve25519 peer import failed (%d)\n", ret);
            return -1;
        }
    }

    off += (int)f_len;

    if (be32_read_at(payload, payload_len, off, &sig_len) != 0) return -1;
    off += 4;
    if ((uint32_t)payload_len < (uint32_t)off + sig_len) return -1;

    char hostkey_alg[64] = {0};
    if (hostkey_len >= 4) {
        uint32_t alg_len = be32_read(hostkey_blob);
        if (alg_len > 0 && alg_len < (uint32_t)sizeof(hostkey_alg) && 4u + alg_len <= hostkey_len) {
            memcpy(hostkey_alg, hostkey_blob + 4, alg_len);
            hostkey_alg[alg_len] = '\0';
        }
    }

    printf("ssh: KEXDH_REPLY hostkey_alg=%s hostkey_len=%u f_len=%u sig_len=%u\n",
           hostkey_alg[0] ? hostkey_alg : "(unknown)",
           (unsigned)hostkey_len,
           (unsigned)f_len,
           (unsigned)sig_len);
    return 0;
}

static int ssh_read_server_identification(char* out, int out_cap) {
    if (!out || out_cap <= 1) return -1;

    for (int i = 0; i < 16; ++i) {
        char line[SSH_MAX_LINE];
        int got = recv_line_tcp(line, sizeof(line), 1500);
        if (got < 0) return -1;
        if (strncmp(line, "SSH-", 4) == 0) {
            strncpy(out, line, (size_t)(out_cap - 1));
            out[out_cap - 1] = '\0';
            return 0;
        }
        printf("ssh: ignored pre-banner line: %s", line);
    }

    return -1;
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

    tcp_stream_reset();
    g_client_kexinit_payload_len = 0;
    g_server_kexinit_payload_len = 0;
    g_hostkey_blob_len = 0;
    g_client_ephemeral_len = 0;
    g_server_ephemeral_len = 0;
    g_shared_secret_len = 0;
    g_exchange_hash_len = 0;
    g_session_id_len = 0;
    g_negotiated_kex[0] = '\0';
    g_negotiated_c2s_cipher[0] = '\0';
    g_negotiated_s2c_cipher[0] = '\0';
    g_negotiated_c2s_mac[0] = '\0';
    g_negotiated_s2c_mac[0] = '\0';
    ssh_curve25519_reset();
    ssh_transport_reset();

    char server_banner[SSH_MAX_LINE];
    if (ssh_read_server_identification(server_banner, sizeof(server_banner)) != 0) {
        puts("ssh: failed to read server identification banner");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    printf("ssh: server banner: %s", server_banner);
    strncpy(g_server_ident, server_banner, sizeof(g_server_ident) - 1);
    g_server_ident[sizeof(g_server_ident) - 1] = '\0';
    g_server_ident_len = (int)strlen(g_server_ident);
    while (g_server_ident_len > 0 &&
           (g_server_ident[g_server_ident_len - 1] == '\n' || g_server_ident[g_server_ident_len - 1] == '\r')) {
        g_server_ident[--g_server_ident_len] = '\0';
    }

    const char* client_banner = "SSH-2.0-EYNOS_0.1\r\n";
    if (tcp_send_all(client_banner, (int)strlen(client_banner)) != 0) {
        puts("ssh: failed to send client identification banner");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: client banner sent");
    strncpy(g_client_ident, client_banner, sizeof(g_client_ident) - 1);
    g_client_ident[sizeof(g_client_ident) - 1] = '\0';
    g_client_ident_len = (int)strlen(g_client_ident);
    while (g_client_ident_len > 0 &&
           (g_client_ident[g_client_ident_len - 1] == '\n' || g_client_ident[g_client_ident_len - 1] == '\r')) {
        g_client_ident[--g_client_ident_len] = '\0';
    }

    if (ssh_send_kexinit() != 0) {
        puts("ssh: failed to send KEXINIT packet");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: sent SSH_MSG_KEXINIT");

    uint8_t rx_payload[SSH_MAX_PACKET];
    int rx_len = 0;
    if (ssh_recv_until_msg(20, rx_payload, sizeof(rx_payload), &rx_len) != 0) {
        puts("ssh: failed to read server KEX packet");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    if (ssh_parse_server_kexinit(rx_payload, rx_len) != 0) {
        puts("ssh: failed to parse/negotiate server KEXINIT");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    if (ssh_send_kexdh_init() != 0) {
        puts("ssh: failed to send KEXDH_INIT");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: sent SSH_MSG_KEXDH_INIT");

    if (ssh_recv_until_msg(31, rx_payload, sizeof(rx_payload), &rx_len) != 0) {
        puts("ssh: failed to read SSH_MSG_KEXDH_REPLY");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    if (ssh_parse_kexdh_reply(rx_payload, rx_len) != 0) {
        puts("ssh: failed to parse SSH_MSG_KEXDH_REPLY");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    if (ssh_send_newkeys() != 0) {
        puts("ssh: failed to send SSH_MSG_NEWKEYS");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: sent SSH_MSG_NEWKEYS");

    if (ssh_recv_until_msg(21, rx_payload, sizeof(rx_payload), &rx_len) != 0) {
        puts("ssh: failed to read server SSH_MSG_NEWKEYS");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    if (ssh_derive_exchange_hash_and_keys() != 0) {
        puts("ssh: failed to derive exchange hash/session keys");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    if (ssh_send_service_request("ssh-userauth") != 0) {
        puts("ssh: failed to send encrypted SSH_MSG_SERVICE_REQUEST");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: sent encrypted SSH_MSG_SERVICE_REQUEST(ssh-userauth)");

    if (ssh_recv_until_msg(6, rx_payload, sizeof(rx_payload), &rx_len) != 0) {
        puts("ssh: failed to read SSH_MSG_SERVICE_ACCEPT");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }
    puts("ssh: received encrypted SSH_MSG_SERVICE_ACCEPT");

    int auth_ok = 0;
    for (int auth_attempt = 0; auth_attempt < 5; ++auth_attempt) {
        char password[128];
        if (ssh_read_password(password, sizeof(password)) != 0) {
            puts("ssh: failed to read password");
            (void)eyn_sys_net_tcp_close();
            return 1;
        }

        if (ssh_send_userauth_password(t.user, password) != 0) {
            puts("ssh: failed to send encrypted SSH_MSG_USERAUTH_REQUEST(password)");
            (void)eyn_sys_net_tcp_close();
            return 1;
        }
        puts("ssh: sent encrypted SSH_MSG_USERAUTH_REQUEST(password)");

        if (ssh_recv_userauth_terminal(rx_payload, sizeof(rx_payload), &rx_len) != 0) {
            puts("ssh: failed to read SSH_MSG_USERAUTH_(SUCCESS|FAILURE)");
            (void)eyn_sys_net_tcp_close();
            return 1;
        }
        if (ssh_handle_userauth_reply(rx_payload, rx_len) != 0) {
            puts("ssh: failed to parse SSH_MSG_USERAUTH reply");
            (void)eyn_sys_net_tcp_close();
            return 1;
        }

        if (rx_len > 0 && rx_payload[0] == 52) {
            auth_ok = 1;
            break;
        }

        if (auth_attempt < 4) {
            puts("ssh: password was rejected; try again");
        }
    }

    if (!auth_ok) {
        puts("ssh: authentication failed after multiple attempts");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    uint32_t remote_channel = 0;
    if (ssh_send_channel_open_session(&remote_channel) != 0) {
        puts("ssh: failed to open session channel");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    puts("ssh: session channel open; starting interactive shell relay");
    if (ssh_run_session_shell(remote_channel) != 0) {
        puts("ssh: interactive shell relay failed");
        (void)eyn_sys_net_tcp_close();
        return 1;
    }

    puts("ssh: encrypted transport is active; interactive shell relay ended.");

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
