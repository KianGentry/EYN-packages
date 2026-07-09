#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>
#include <eyn_daemon.h>
#include <ftpd/protocol.h>

EYN_CMDMETA_V1("FTP client daemon: serves FETCH/LIST/PUT requests from other programs.",
               "ftpd");

/*
 * ftpd - the ONLY place FTP protocol logic lives in this codebase.
 *
 * Foreground programs (e.g. `download`) never link against FTP-specific
 * code; they just send an ftpd_request_t to this daemon via the generic
 * eyn_daemon_call("ftpd", ...) helper (see shared/ftpd/protocol.h) and get
 * back an ftpd_response_t. This file implements the control-connection
 * state machine, PASV parsing, auth, LIST, and RETR/STOR data transfer.
 *
 * IMPORTANT (see /memories/repo notes): TCP pool-socket fds returned by
 * socket() live in a completely different namespace than regular file
 * descriptors -- they must ONLY be used with send()/recv()/close() (or the
 * raw eyn_sys_net_tcp_socket_* syscalls), NEVER with generic read()/
 * write()/fdopen(). This file never mixes the two.
 *
 * The libc recv() wrapper loops internally until data arrives or
 * SO_RCVTIMEO elapses, and can never distinguish "no data yet" from
 * "peer closed the connection" (a kernel limitation - see
 * EYN-packages/userland/libc/unistd.c). That's fine for the control
 * connection (replies always eventually arrive). For the FTP *data*
 * connection, an RETR/LIST completes when the server closes the data
 * socket, which recv() alone cannot detect. So the data transfer loops
 * below call the raw eyn_sys_net_tcp_socket_recv() syscall directly and
 * cross-check the socket's kernel-side TCP state via
 * eyn_sys_net_tcp_get_sockets() to tell a real timeout apart from a
 * normal, successful transfer completion.
 */

#define FTPD_TCP_STATE_ESTABLISHED 3 /* must match kernel's tcp_state TCP_ESTABLISHED (src/network/netstack.c) */
#define FTPD_DATA_IDLE_TIMEOUT_POLLS 6000 /* ~60s at 10ms/poll */
#define FTPD_LINE_MAX 512
#define FTPD_XFER_BUF 1536

typedef struct {
    int  fd;
    char buf[FTPD_LINE_MAX];
    int  len;
    int  pos;
} ftp_conn_t;

static void ftp_conn_init(ftp_conn_t* c, int fd) {
    c->fd = fd;
    c->len = 0;
    c->pos = 0;
}

/* Refill the line buffer from the control socket. Returns bytes available,
   or <0 on error/timeout. Uses the blocking libc recv() (fine for control
   traffic - the server is expected to always eventually reply). */
static int ftp_conn_fill(ftp_conn_t* c) {
    if (c->pos < c->len) return c->len - c->pos;
    ssize_t n = recv(c->fd, c->buf, sizeof(c->buf), 0);
    if (n <= 0) return -1;
    c->len = (int)n;
    c->pos = 0;
    return c->len;
}

/* Read one CRLF/LF-terminated line (stripped) into out[cap]. Returns line
   length (>=0) or -1 on error/EOF. */
static int ftp_conn_read_line(ftp_conn_t* c, char* out, int cap) {
    int outlen = 0;
    for (;;) {
        if (c->pos >= c->len) {
            if (ftp_conn_fill(c) < 0) return (outlen > 0) ? outlen : -1;
        }
        while (c->pos < c->len) {
            char ch = c->buf[c->pos++];
            if (ch == '\n') {
                if (outlen > 0 && out[outlen - 1] == '\r') outlen--;
                out[outlen] = '\0';
                return outlen;
            }
            if (outlen < cap - 1) out[outlen++] = ch;
        }
    }
}

static int ftp_send_line(ftp_conn_t* c, const char* cmd) {
    char line[FTPD_LINE_MAX];
    int n = snprintf(line, sizeof(line), "%s\r\n", cmd);
    if (n <= 0 || n >= (int)sizeof(line)) return -1;
    ssize_t sent = send(c->fd, line, (size_t)n, 0);
    return (sent == n) ? 0 : -1;
}

/* Read a single (possibly multi-line) FTP reply. Stores the last line's
   text (after the 3-digit code) into msg_out if non-NULL. Returns the
   numeric reply code (>=0) or -1 on error/EOF. */
static int ftp_read_reply(ftp_conn_t* c, char* msg_out, int msg_cap) {
    char line[FTPD_LINE_MAX];
    int code = -1;
    for (;;) {
        int n = ftp_conn_read_line(c, line, sizeof(line));
        if (n < 0) return -1;
        if (n < 4) continue; /* malformed/blank line; ignore */

        int this_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        char sep = line[3];
        if (code < 0) code = this_code;
        if (msg_out && msg_cap > 0) {
            strncpy(msg_out, line + 4, (size_t)msg_cap - 1);
            msg_out[msg_cap - 1] = '\0';
        }
        if (sep == ' ') return code; /* final line of the reply */
        /* sep == '-' (or anything else): multi-line continuation, keep reading. */
    }
}

static int parse_ipv4_str(const char* s, uint8_t out[4]) {
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

static int resolve_host_ipv4(const char* host, uint8_t out[4]) {
    if (!host || !out) return -1;
    if (parse_ipv4_str(host, out) == 0) return 0;
    if (eyn_sys_net_dns_resolve(host, out) == 0) return 0;
    return -1;
}

/* Parse "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)." style replies. */
static int ftp_parse_pasv(const char* msg, uint8_t ip[4], uint16_t* port) {
    const char* p = strchr(msg, '(');
    if (p) p++;
    else p = msg;

    int a, b, c, d, e, f;
    if (sscanf(p, "%d,%d,%d,%d,%d,%d", &a, &b, &c, &d, &e, &f) != 6) return -1;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    *port = (uint16_t)(((e & 0xFF) << 8) | (f & 0xFF));
    return 0;
}

/* Check whether the kernel-side TCP state for a data connection we dialed
   ourselves has left ESTABLISHED (peer sent FIN / connection torn down),
   by matching on the remote endpoint we know we connected to. */
static int ftp_data_finished(const uint8_t remote_ip[4], uint16_t remote_port) {
    eyn_net_tcp_socket_info_t infos[8];
    int n = eyn_sys_net_tcp_get_sockets(infos, 8);
    if (n <= 0) return 1;
    for (int i = 0; i < n; i++) {
        if (!infos[i].in_use) continue;
        if (infos[i].remote_port != remote_port) continue;
        if (memcmp(infos[i].remote_ip, remote_ip, 4) != 0) continue;
        return infos[i].state != FTPD_TCP_STATE_ESTABLISHED;
    }
    return 1; /* not found at all -> already torn down */
}

static int ftp_open_data_connection(ftp_conn_t* control, uint8_t data_ip[4], uint16_t* data_port) {
    if (ftp_send_line(control, "PASV") != 0) return -1;

    char msg[FTPD_LINE_MAX];
    int code = ftp_read_reply(control, msg, sizeof(msg));
    if (code != 227) return -1;
    if (ftp_parse_pasv(msg, data_ip, data_port) != 0) return -1;

    int data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(*data_port);
    memcpy(&addr.sin_addr.s_addr, data_ip, 4);

    if (connect(data_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(data_fd);
        return -1;
    }
    return data_fd;
}

/* Drain a data connection we dialed via PASV, writing every received chunk
   to out_fd (if >= 0) or appending it into a listing buffer (if
   listing_buf != NULL). Returns 0 on a clean, fully-drained completion, or
   -1 on transfer/local-IO error. */
static int ftp_drain_data_connection(int data_fd, const uint8_t remote_ip[4], uint16_t remote_port,
                                     int out_fd, char* listing_buf, size_t listing_cap,
                                     uint64_t* out_bytes) {
    uint8_t buf[FTPD_XFER_BUF];
    uint64_t total = 0;
    size_t listing_len = listing_buf ? strlen(listing_buf) : 0;
    int idle_polls = 0;

    for (;;) {
        int rc = eyn_sys_net_tcp_socket_recv(data_fd, buf, sizeof(buf));
        if (rc > 0) {
            idle_polls = 0;
            total += (uint64_t)rc;
            if (out_fd >= 0) {
                ssize_t w = write(out_fd, buf, (size_t)rc);
                if (w != rc) {
                    if (out_bytes) *out_bytes = total;
                    return -1;
                }
            }
            if (listing_buf && listing_len < listing_cap - 1) {
                size_t take = listing_cap - 1 - listing_len;
                if (take > (size_t)rc) take = (size_t)rc;
                memcpy(listing_buf + listing_len, buf, take);
                listing_len += take;
                listing_buf[listing_len] = '\0';
            }
            continue;
        }
        if (rc < 0) {
            if (out_bytes) *out_bytes = total;
            return -1;
        }
        /* rc == 0: nothing queued this poll. */
        if (ftp_data_finished(remote_ip, remote_port)) break;
        idle_polls++;
        if (idle_polls > FTPD_DATA_IDLE_TIMEOUT_POLLS) {
            if (out_bytes) *out_bytes = total;
            return -1;
        }
        usleep(10000);
    }

    if (out_bytes) *out_bytes = total;
    return 0;
}

static void ftp_reply_error(ftpd_response_t* resp, ftpd_status_t status, int ftp_code, const char* msg) {
    memset(resp, 0, sizeof(*resp));
    resp->status = status;
    resp->ftp_code = ftp_code;
    if (msg) {
        strncpy(resp->message, msg, sizeof(resp->message) - 1);
        resp->message[sizeof(resp->message) - 1] = '\0';
    }
}

/* Connect + log in. On success, control->fd is left open and connected;
   caller is responsible for QUIT + close(). On failure, returns nonzero
   and control->fd has already been closed. */
static int ftp_connect_and_login(const ftpd_request_t* req, ftp_conn_t* control, ftpd_response_t* resp) {
    uint8_t ip[4];
    if (resolve_host_ipv4(req->host, ip) != 0) {
        ftp_reply_error(resp, FTPD_ERR_DNS, 0, "DNS resolution failed");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ftp_reply_error(resp, FTPD_ERR_CONNECT, 0, "socket() failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(req->port ? req->port : 21);
    memcpy(&addr.sin_addr.s_addr, ip, 4);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_CONNECT, 0, "connect() failed");
        return -1;
    }

    ftp_conn_init(control, fd);

    char msg[FTPD_LINE_MAX];
    int code = ftp_read_reply(control, msg, sizeof(msg));
    if (code < 200 || code >= 300) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_CONNECT, code, msg);
        return -1;
    }

    const char* user = req->use_anonymous || req->user[0] == '\0' ? "anonymous" : req->user;
    const char* pass = req->use_anonymous ? "anonymous@" : req->pass;

    char cmd[FTPD_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "USER %s", user);
    if (ftp_send_line(control, cmd) != 0) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_LOGIN, 0, "failed sending USER");
        return -1;
    }
    code = ftp_read_reply(control, msg, sizeof(msg));
    if (code == 331) {
        snprintf(cmd, sizeof(cmd), "PASS %s", pass);
        if (ftp_send_line(control, cmd) != 0) {
            close(fd);
            ftp_reply_error(resp, FTPD_ERR_LOGIN, 0, "failed sending PASS");
            return -1;
        }
        code = ftp_read_reply(control, msg, sizeof(msg));
    }
    if (code < 200 || code >= 300) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_LOGIN, code, msg);
        return -1;
    }

    if (ftp_send_line(control, "TYPE I") != 0) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_PROTOCOL, 0, "failed sending TYPE I");
        return -1;
    }
    code = ftp_read_reply(control, msg, sizeof(msg));
    if (code < 200 || code >= 300) {
        close(fd);
        ftp_reply_error(resp, FTPD_ERR_PROTOCOL, code, msg);
        return -1;
    }

    return 0;
}

static void ftp_quit_and_close(ftp_conn_t* control) {
    char msg[FTPD_LINE_MAX];
    (void)ftp_send_line(control, "QUIT");
    (void)ftp_read_reply(control, msg, sizeof(msg));
    close(control->fd);
}

static void handle_fetch(const ftpd_request_t* req, ftpd_response_t* resp) {
    ftp_conn_t control;
    if (ftp_connect_and_login(req, &control, resp) != 0) return;

    uint8_t data_ip[4];
    uint16_t data_port;
    int data_fd = ftp_open_data_connection(&control, data_ip, &data_port);
    if (data_fd < 0) {
        ftp_reply_error(resp, FTPD_ERR_PASV, 0, "PASV failed");
        ftp_quit_and_close(&control);
        return;
    }

    int out_fd = open(req->local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_LOCAL_IO, 0, "failed to open local_path for writing");
        ftp_quit_and_close(&control);
        return;
    }

    char cmd[FTPD_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "RETR %s", req->remote_path);
    if (ftp_send_line(&control, cmd) != 0) {
        close(out_fd);
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, 0, "failed sending RETR");
        ftp_quit_and_close(&control);
        return;
    }

    char msg[FTPD_LINE_MAX];
    int code = ftp_read_reply(&control, msg, sizeof(msg));
    if (code != 150 && code != 125) {
        close(out_fd);
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, code, msg);
        ftp_quit_and_close(&control);
        return;
    }

    uint64_t bytes = 0;
    int drain_rc = ftp_drain_data_connection(data_fd, data_ip, data_port, out_fd, NULL, 0, &bytes);
    close(out_fd);
    close(data_fd);

    if (drain_rc != 0) {
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, 0, "data transfer stalled or failed");
        resp->bytes_transferred = bytes;
        ftp_quit_and_close(&control);
        return;
    }

    code = ftp_read_reply(&control, msg, sizeof(msg));
    memset(resp, 0, sizeof(*resp));
    if (code < 200 || code >= 300) {
        resp->status = FTPD_ERR_TRANSFER;
        resp->ftp_code = code;
        strncpy(resp->message, msg, sizeof(resp->message) - 1);
    } else {
        resp->status = FTPD_OK;
        resp->ftp_code = code;
        strncpy(resp->message, msg, sizeof(resp->message) - 1);
    }
    resp->bytes_transferred = bytes;

    ftp_quit_and_close(&control);
}

static void handle_list(const ftpd_request_t* req, ftpd_response_t* resp) {
    ftp_conn_t control;
    if (ftp_connect_and_login(req, &control, resp) != 0) return;

    uint8_t data_ip[4];
    uint16_t data_port;
    int data_fd = ftp_open_data_connection(&control, data_ip, &data_port);
    if (data_fd < 0) {
        ftp_reply_error(resp, FTPD_ERR_PASV, 0, "PASV failed");
        ftp_quit_and_close(&control);
        return;
    }

    char cmd[FTPD_LINE_MAX];
    if (req->remote_path[0]) {
        snprintf(cmd, sizeof(cmd), "LIST %s", req->remote_path);
    } else {
        snprintf(cmd, sizeof(cmd), "LIST");
    }
    if (ftp_send_line(&control, cmd) != 0) {
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, 0, "failed sending LIST");
        ftp_quit_and_close(&control);
        return;
    }

    char msg[FTPD_LINE_MAX];
    int code = ftp_read_reply(&control, msg, sizeof(msg));
    if (code != 150 && code != 125) {
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, code, msg);
        ftp_quit_and_close(&control);
        return;
    }

    memset(resp, 0, sizeof(*resp));
    uint64_t bytes = 0;
    int drain_rc = ftp_drain_data_connection(data_fd, data_ip, data_port, -1,
                                             resp->listing, sizeof(resp->listing), &bytes);
    close(data_fd);

    if (drain_rc != 0) {
        resp->status = FTPD_ERR_TRANSFER;
        strncpy(resp->message, "data transfer stalled or failed", sizeof(resp->message) - 1);
        resp->bytes_transferred = bytes;
        ftp_quit_and_close(&control);
        return;
    }

    code = ftp_read_reply(&control, msg, sizeof(msg));
    resp->status = (code >= 200 && code < 300) ? FTPD_OK : FTPD_ERR_TRANSFER;
    resp->ftp_code = code;
    strncpy(resp->message, msg, sizeof(resp->message) - 1);
    resp->bytes_transferred = bytes;

    ftp_quit_and_close(&control);
}

static void handle_put(const ftpd_request_t* req, ftpd_response_t* resp) {
    ftp_conn_t control;
    if (ftp_connect_and_login(req, &control, resp) != 0) return;

    int in_fd = open(req->local_path, O_RDONLY);
    if (in_fd < 0) {
        ftp_reply_error(resp, FTPD_ERR_LOCAL_IO, 0, "failed to open local_path for reading");
        ftp_quit_and_close(&control);
        return;
    }

    uint8_t data_ip[4];
    uint16_t data_port;
    int data_fd = ftp_open_data_connection(&control, data_ip, &data_port);
    if (data_fd < 0) {
        close(in_fd);
        ftp_reply_error(resp, FTPD_ERR_PASV, 0, "PASV failed");
        ftp_quit_and_close(&control);
        return;
    }

    char cmd[FTPD_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "STOR %s", req->remote_path);
    if (ftp_send_line(&control, cmd) != 0) {
        close(in_fd);
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, 0, "failed sending STOR");
        ftp_quit_and_close(&control);
        return;
    }

    char msg[FTPD_LINE_MAX];
    int code = ftp_read_reply(&control, msg, sizeof(msg));
    if (code != 150 && code != 125) {
        close(in_fd);
        close(data_fd);
        ftp_reply_error(resp, FTPD_ERR_TRANSFER, code, msg);
        ftp_quit_and_close(&control);
        return;
    }

    uint64_t total = 0;
    uint8_t buf[FTPD_XFER_BUF];
    int io_err = 0;
    for (;;) {
        ssize_t n = read(in_fd, buf, sizeof(buf));
        if (n < 0) { io_err = 1; break; }
        if (n == 0) break;
        ssize_t s = send(data_fd, buf, (size_t)n, 0);
        if (s != n) { io_err = 1; break; }
        total += (uint64_t)n;
    }
    close(in_fd);
    close(data_fd); /* triggers FIN so the server knows the upload is complete */

    memset(resp, 0, sizeof(*resp));
    if (io_err) {
        resp->status = FTPD_ERR_TRANSFER;
        strncpy(resp->message, "local read or data send failed", sizeof(resp->message) - 1);
        resp->bytes_transferred = total;
        ftp_quit_and_close(&control);
        return;
    }

    code = ftp_read_reply(&control, msg, sizeof(msg));
    resp->status = (code >= 200 && code < 300) ? FTPD_OK : FTPD_ERR_TRANSFER;
    resp->ftp_code = code;
    strncpy(resp->message, msg, sizeof(resp->message) - 1);
    resp->bytes_transferred = total;

    ftp_quit_and_close(&control);
}

int main(void) {
    eyn_daemon_t* d = eyn_daemon_listen("ftpd");
    if (!d) {
        puts("ftpd: failed to start listening (eyn_daemon_listen failed)");
        return 1;
    }

    for (;;) {
        ftpd_request_t req;
        uint32_t got_len = 0;
        char reply_token[EYN_DAEMON_PATH_MAX];

        int rc = eyn_daemon_recv(d, &req, sizeof(req), &got_len, reply_token, 0);
        if (rc < 0) continue; /* EYN_DAEMON_ERR / EYN_DAEMON_ERR_TIME: keep serving */
        if (got_len != sizeof(req)) continue; /* malformed request; drop silently */

        ftpd_response_t resp;
        memset(&resp, 0, sizeof(resp));

        switch (req.op) {
            case FTPD_OP_PING:
                resp.status = FTPD_OK;
                strncpy(resp.message, "pong", sizeof(resp.message) - 1);
                break;
            case FTPD_OP_FETCH:
                handle_fetch(&req, &resp);
                break;
            case FTPD_OP_LIST:
                handle_list(&req, &resp);
                break;
            case FTPD_OP_PUT:
                handle_put(&req, &resp);
                break;
            default:
                ftp_reply_error(&resp, FTPD_ERR_BAD_REQUEST, 0, "unknown op");
                break;
        }

        if (reply_token[0]) {
            (void)eyn_daemon_reply(reply_token, &resp, sizeof(resp));
        }
    }

    eyn_daemon_close(d);
    return 0;
}
