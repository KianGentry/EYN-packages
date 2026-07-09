#pragma once

/*
 * ftpd protocol.h - wire-format contract for talking to the "ftpd" daemon.
 *
 * ARCHITECTURE: foreground programs (e.g. `download`) never call FTP-
 * specific functions directly. All FTP protocol knowledge (control
 * connection, PASV, auth, LIST/NLST, RETR/STOR) lives inside the ftpd
 * daemon (packages/ftpd). A caller just fills in an ftpd_request_t and
 * goes through the generic daemon IPC helper (eyn_daemon.h):
 *
 *     #include <eyn_daemon.h>
 *     #include <ftpd/protocol.h>
 *
 *     ftpd_request_t req; memset(&req, 0, sizeof(req));
 *     req.op = FTPD_OP_FETCH;
 *     strncpy(req.host, "ftp.example.com", sizeof(req.host) - 1);
 *     req.port = 21;
 *     req.use_anonymous = 1;
 *     strncpy(req.remote_path, "/pub/file.txt", sizeof(req.remote_path) - 1);
 *     strncpy(req.local_path, "/file.txt", sizeof(req.local_path) - 1);
 *
 *     ftpd_response_t resp;
 *     int rc = eyn_daemon_call("ftpd", &req, sizeof(req), &resp, sizeof(resp), NULL, 0);
 *
 * This header defines only the data contract, never behavior. It is
 * included by BOTH the ftpd daemon and any client package, via the
 * shared include path (EYN-packages/shared, added by devtools/
 * build_user_c.sh and devtools/build_package.sh for every package).
 *
 * The daemon itself is spawned on demand: a client that fails to reach
 * "ftpd" via eyn_daemon_call() (e.g. FTPD_OP_PING times out) should
 * posix_spawn()/eyn_sys_spawn_ex() "/bin/ftpd" and retry. See ftpd_uelf.c
 * for the daemon-side listen loop.
 */

#include <stdint.h>

#define FTPD_HOST_MAX    128
#define FTPD_USER_MAX    64
#define FTPD_PASS_MAX    64
#define FTPD_PATH_MAX    256
#define FTPD_MSG_MAX     160
#define FTPD_LISTING_MAX 1024

typedef enum {
    FTPD_OP_PING  = 0, /* health check; used to detect a live daemon before spawning a new one */
    FTPD_OP_FETCH = 1, /* RETR: download remote_path from the server to local_path */
    FTPD_OP_LIST  = 2, /* LIST: list remote_path (or server cwd if empty), text returned in resp.listing */
    FTPD_OP_PUT   = 3  /* STOR: upload local_path to remote_path on the server */
} ftpd_op_t;

typedef struct {
    uint32_t op;                        /* ftpd_op_t */
    char     host[FTPD_HOST_MAX];       /* hostname or dotted-quad IPv4; ignored for FTPD_OP_PING */
    uint16_t port;                      /* 0 => default 21 */
    uint8_t  use_anonymous;             /* nonzero => login as "anonymous", ignore user/pass */
    char     user[FTPD_USER_MAX];
    char     pass[FTPD_PASS_MAX];
    char     remote_path[FTPD_PATH_MAX];
    char     local_path[FTPD_PATH_MAX]; /* unused for FTPD_OP_LIST */
} ftpd_request_t;

typedef enum {
    FTPD_OK              = 0,
    FTPD_ERR_DNS         = -1,
    FTPD_ERR_CONNECT     = -2,
    FTPD_ERR_LOGIN       = -3,
    FTPD_ERR_PASV        = -4,
    FTPD_ERR_TRANSFER    = -5,
    FTPD_ERR_LOCAL_IO    = -6,
    FTPD_ERR_PROTOCOL    = -7,
    FTPD_ERR_BAD_REQUEST = -8
} ftpd_status_t;

typedef struct {
    int32_t  status;                    /* 0 on success, ftpd_status_t (negative) on failure */
    int32_t  ftp_code;                  /* last numeric FTP reply code seen (e.g. 550), 0 if none */
    uint64_t bytes_transferred;         /* for FETCH/PUT */
    char     message[FTPD_MSG_MAX];     /* human-readable detail: error text or last server reply line */
    char     listing[FTPD_LISTING_MAX]; /* for FTPD_OP_LIST: raw LIST text, truncated if longer */
} ftpd_response_t;
