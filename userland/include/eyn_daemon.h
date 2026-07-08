#pragma once

/*
 * eyn_daemon.h - generic request/reply IPC between an ordinary program and a
 * long-lived, named "daemon" process.
 *
 * Goal: let anyone add a new background daemon (ftpd, a GUI manager, a
 * network policy service, ...) and let any other program talk to it by name,
 * WITHOUT touching kernel code or adding new syscalls. Everything here is
 * built entirely on top of existing EYN-OS primitives: mkfifo()/open()/
 * read()/write()/close().
 *
 * ---------------------------------------------------------------------
 * Protocol
 * ---------------------------------------------------------------------
 * Every daemon has exactly one well-known, persistent request FIFO:
 *     /tmp/daemons/<name>.req
 * created (idempotently) by whichever side calls mkfifo() first -- usually
 * the daemon at startup via eyn_daemon_listen(), but a client may also race
 * to create it; either way callers just share the same underlying pipe.
 *
 * Every daemon also gets exactly one shared reply FIFO, reused by every
 * client that ever calls it:
 *     /tmp/daemons/<name>.rep
 * Since it is shared, eyn_daemon_call() serializes access to it with a
 * simple mkdir()-based lock (/tmp/daemons/<name>.lock) so only one
 * in-flight call per daemon uses the reply channel at a time. This avoids
 * needing a real per-process identifier -- EYN-OS's getpid() is currently
 * a non-functional stub (always returns 1), so a path keyed by PID would
 * not actually be unique per process.
 *
 * A message on the wire is a fixed-size header (magic + payload length +
 * an optional reply path) immediately followed by the payload bytes:
 *     [eyn_daemon_hdr_t][payload...]
 *
 * eyn_daemon_send()/eyn_daemon_call() write the header+payload with a
 * SINGLE write() syscall. On EYN-OS, syscalls run to completion between
 * voluntary blocking points (see docs/general and repo notes on the
 * syscall trap gate), so as long as the write never needs to block for
 * buffer space it cannot be interleaved with another process's write to
 * the same shared request FIFO. Keep messages small (see
 * EYN_DAEMON_MSG_MAX) and expect the daemon to drain requests promptly --
 * this channel is meant for small control-plane messages, not bulk data.
 * For bulk transfers, negotiate a dedicated channel (a TCP socket, or a
 * second FIFO path) inside your own request/reply payloads.
 *
 * ---------------------------------------------------------------------
 * KNOWN LIMITATIONS
 * ---------------------------------------------------------------------
 * - Total persistent FIFO slots are capped (currently 24, shared with
 *   every other named pipe in the whole system) and unlink() does not
 *   reclaim a slot once allocated. This library keeps its footprint to
 *   exactly two slots per daemon name (one request FIFO, one reply
 *   FIFO) no matter how many clients call it or how many calls are made
 *   -- never one slot per call or per client. Raising the ceiling or
 *   adding real FIFO teardown would require a kernel change and is not
 *   done here by design.
 * - eyn_daemon_call() takes a lock (a directory at /tmp/daemons/<name>.lock)
 *   before using the shared reply FIFO, so concurrent callers to the SAME
 *   daemon are serialized. Calls to DIFFERENT daemons are independent and
 *   run concurrently. If a caller is killed while holding this lock, the
 *   daemon becomes unreachable via eyn_daemon_call() until the lock
 *   directory is removed manually (rmdir "/tmp/daemons/<name>.lock").
 * - timeout_ms, when used, bounds waiting for the lock and waiting for
 *   the reply as two separate windows, so the worst-case total wait for
 *   eyn_daemon_call() is up to roughly 2x timeout_ms under contention.
 * ---------------------------------------------------------------------
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#define EYN_DAEMON_DIR      "/tmp/daemons"
#define EYN_DAEMON_NAME_MAX 32
#define EYN_DAEMON_PATH_MAX 96
#define EYN_DAEMON_MSG_MAX  2048
#define EYN_DAEMON_MAGIC    0x444E4559u /* "EYND" */

/* Error codes returned by the blocking/timeout call & recv functions. */
#define EYN_DAEMON_ERR      (-1) /* generic failure / daemon not reachable */
#define EYN_DAEMON_ERR_SIZE (-2) /* payload larger than EYN_DAEMON_MSG_MAX */
#define EYN_DAEMON_ERR_TIME (-3) /* timed out waiting for a message */

typedef struct {
    uint32_t magic;
    uint32_t payload_len;
    int32_t  wants_reply;
    char     reply_path[EYN_DAEMON_PATH_MAX];
} eyn_daemon_hdr_t;

typedef struct {
    int  req_read_fd;
    int  req_selfwrite_fd;
    char name[EYN_DAEMON_NAME_MAX];
} eyn_daemon_t;

static inline void eyn_daemon__ensure_dir(void) {
    mkdir("/tmp", 0755);
    mkdir(EYN_DAEMON_DIR, 0755);
}

static inline void eyn_daemon__req_path(const char* name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s.req", EYN_DAEMON_DIR, name);
}

static inline void eyn_daemon__rep_path(const char* name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s.rep", EYN_DAEMON_DIR, name);
}

static inline void eyn_daemon__lock_path(const char* name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s.lock", EYN_DAEMON_DIR, name);
}

/*
 * Acquire the per-daemon call lock (a plain directory used as an atomic
 * test-and-set: mkdir() succeeds only if the directory did not already
 * exist). timeout_ms <= 0 waits forever; otherwise gives up after
 * timeout_ms milliseconds. Returns 0 on success, EYN_DAEMON_ERR_TIME on
 * timeout.
 */
static inline int eyn_daemon__lock_acquire(const char* name, int timeout_ms) {
    char lock_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__lock_path(name, lock_path, sizeof(lock_path));

    int waited_ms = 0;
    for (;;) {
        if (mkdir(lock_path, 0755) == 0) return 0;
        if (timeout_ms > 0 && waited_ms >= timeout_ms) return EYN_DAEMON_ERR_TIME;
        usleep(1000);
        waited_ms += 1;
    }
}

static inline void eyn_daemon__lock_release(const char* name) {
    char lock_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__lock_path(name, lock_path, sizeof(lock_path));
    rmdir(lock_path);
}

static inline int eyn_daemon__write_all(int fd, const void* buf, uint32_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t n = 0;
    while (n < len) {
        int w = write(fd, p + n, len - n);
        if (w <= 0) return -1;
        n += (uint32_t)w;
    }
    return 0;
}

/*
 * Reads exactly len bytes from fd, sharing one overall wait budget
 * (*budget_ms_inout, in milliseconds) across repeated calls. If
 * budget_ms_inout is NULL, blocks forever (no timeout).
 * Returns 0 on success, -1 on hard error/EOF, EYN_DAEMON_ERR_TIME on
 * timeout.
 */
static inline int eyn_daemon__read_all_budget(int fd, void* buf, uint32_t len, int* budget_ms_inout) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t n = 0;

    if (!budget_ms_inout) {
        while (n < len) {
            int r = read(fd, p + n, len - n);
            if (r <= 0) return -1;
            n += (uint32_t)r;
        }
        return 0;
    }

    fd_set_nonblock(fd, 1);
    while (n < len) {
        int r = read(fd, p + n, len - n);
        if (r > 0) {
            n += (uint32_t)r;
            continue;
        }
        if (r == 0) {
            fd_set_nonblock(fd, 0);
            return -1; /* real EOF */
        }
        if (*budget_ms_inout <= 0) {
            fd_set_nonblock(fd, 0);
            return EYN_DAEMON_ERR_TIME;
        }
        usleep(1000);
        *budget_ms_inout -= 1;
    }
    fd_set_nonblock(fd, 0);
    return 0;
}

/*
 * Shared receive path used by both the client (reading a reply) and the
 * daemon (reading a request). Drains exactly one framed message from fd,
 * copying up to buf_cap bytes of payload into buf (truncating if the
 * payload is larger). If reply_token is non-NULL, it is filled with the
 * sender's reply path (or an empty string if no reply was requested).
 * budget_ms: 0 blocks forever; >0 caps the total wait across header+payload.
 */
static inline int eyn_daemon__recv_framed(int fd, void* buf, uint32_t buf_cap, uint32_t* out_len,
                                          char reply_token[EYN_DAEMON_PATH_MAX], int budget_ms) {
    int* budget_ptr = (budget_ms > 0) ? &budget_ms : NULL;

    eyn_daemon_hdr_t hdr;
    int hrc = eyn_daemon__read_all_budget(fd, &hdr, sizeof(hdr), budget_ptr);
    if (hrc != 0) return hrc;
    if (hdr.magic != EYN_DAEMON_MAGIC) return EYN_DAEMON_ERR;

    uint32_t remaining = hdr.payload_len;
    uint32_t to_copy = remaining;
    if (to_copy > buf_cap) to_copy = buf_cap;

    uint8_t tmp[256];
    uint32_t copied = 0;
    while (remaining > 0) {
        uint32_t chunk = (remaining < sizeof(tmp)) ? remaining : (uint32_t)sizeof(tmp);
        int rc = eyn_daemon__read_all_budget(fd, tmp, chunk, budget_ptr);
        if (rc != 0) return rc;

        uint32_t take = (copied < to_copy) ? (to_copy - copied) : 0;
        if (take > chunk) take = chunk;
        if (take > 0 && buf) memcpy((uint8_t*)buf + copied, tmp, take);
        copied += take;
        remaining -= chunk;
    }

    if (reply_token) {
        if (hdr.wants_reply) {
            strncpy(reply_token, hdr.reply_path, EYN_DAEMON_PATH_MAX - 1);
            reply_token[EYN_DAEMON_PATH_MAX - 1] = '\0';
        } else {
            reply_token[0] = '\0';
        }
    }

    if (out_len) *out_len = copied;
    return (int)copied;
}

/* ------------------------------------------------------------------- */
/* Client side                                                          */
/* ------------------------------------------------------------------- */

/*
 * Fire-and-forget: send payload to the named daemon, no reply expected.
 * Returns 0 on success, EYN_DAEMON_ERR_SIZE if len > EYN_DAEMON_MSG_MAX,
 * or EYN_DAEMON_ERR if the daemon isn't listening / the write failed.
 */
static inline int eyn_daemon_send(const char* name, const void* payload, uint32_t len) {
    if (!name || !name[0] || len > EYN_DAEMON_MSG_MAX) return EYN_DAEMON_ERR_SIZE;
    eyn_daemon__ensure_dir();

    char req_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__req_path(name, req_path, sizeof(req_path));
    mkfifo(req_path, 0666);

    int fd = open(req_path, O_WRONLY);
    if (fd < 0) return EYN_DAEMON_ERR;

    uint8_t buf[sizeof(eyn_daemon_hdr_t) + EYN_DAEMON_MSG_MAX];
    eyn_daemon_hdr_t* hdr = (eyn_daemon_hdr_t*)(void*)buf;
    hdr->magic = EYN_DAEMON_MAGIC;
    hdr->payload_len = len;
    hdr->wants_reply = 0;
    hdr->reply_path[0] = '\0';
    if (len) memcpy(buf + sizeof(*hdr), payload, len);

    int rc = eyn_daemon__write_all(fd, buf, (uint32_t)sizeof(*hdr) + len);
    close(fd);
    return rc == 0 ? 0 : EYN_DAEMON_ERR;
}

/*
 * Send a request and wait for the daemon's reply.
 * timeout_ms == 0 waits forever; timeout_ms > 0 caps the total wait.
 * On success returns the number of reply bytes copied into resp_buf
 * (>= 0, truncated to resp_cap) and, if out_resp_len is non-NULL, stores
 * the same value there. On failure returns EYN_DAEMON_ERR_SIZE,
 * EYN_DAEMON_ERR, or EYN_DAEMON_ERR_TIME.
 */
static inline int eyn_daemon_call(const char* name,
                                   const void* req, uint32_t req_len,
                                   void* resp_buf, uint32_t resp_cap, uint32_t* out_resp_len,
                                   int timeout_ms) {
    if (!name || !name[0] || req_len > EYN_DAEMON_MSG_MAX) return EYN_DAEMON_ERR_SIZE;
    eyn_daemon__ensure_dir();

    int lrc = eyn_daemon__lock_acquire(name, timeout_ms);
    if (lrc != 0) return lrc;

    char reply_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__rep_path(name, reply_path, sizeof(reply_path));
    mkfifo(reply_path, 0666);

    /*
     * Open both ends of the daemon's shared reply FIFO before sending the
     * request. EYN-OS FIFOs report EOF (read() returning 0) whenever
     * there is no writer, including the "nobody has connected yet" case
     * -- holding our own write end keeps a writer registered so our read
     * below properly waits for the daemon's reply instead of seeing a
     * spurious EOF the instant we start reading. The lock above ensures
     * we are the only client using this shared reply channel right now.
     */
    int reply_rfd = open(reply_path, O_RDONLY);
    int reply_wfd_self = open(reply_path, O_WRONLY);
    if (reply_rfd < 0 || reply_wfd_self < 0) {
        if (reply_rfd >= 0) close(reply_rfd);
        if (reply_wfd_self >= 0) close(reply_wfd_self);
        eyn_daemon__lock_release(name);
        return EYN_DAEMON_ERR;
    }

    char req_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__req_path(name, req_path, sizeof(req_path));
    mkfifo(req_path, 0666);
    int req_fd = open(req_path, O_WRONLY);
    if (req_fd < 0) {
        close(reply_rfd);
        close(reply_wfd_self);
        eyn_daemon__lock_release(name);
        return EYN_DAEMON_ERR;
    }

    uint8_t buf[sizeof(eyn_daemon_hdr_t) + EYN_DAEMON_MSG_MAX];
    eyn_daemon_hdr_t* hdr = (eyn_daemon_hdr_t*)(void*)buf;
    hdr->magic = EYN_DAEMON_MAGIC;
    hdr->payload_len = req_len;
    hdr->wants_reply = 1;
    strncpy(hdr->reply_path, reply_path, sizeof(hdr->reply_path) - 1);
    hdr->reply_path[sizeof(hdr->reply_path) - 1] = '\0';
    if (req_len) memcpy(buf + sizeof(*hdr), req, req_len);

    int wrc = eyn_daemon__write_all(req_fd, buf, (uint32_t)sizeof(*hdr) + req_len);
    close(req_fd);
    if (wrc != 0) {
        close(reply_rfd);
        close(reply_wfd_self);
        eyn_daemon__lock_release(name);
        return EYN_DAEMON_ERR; /* daemon not listening, or write failed */
    }

    int rc = eyn_daemon__recv_framed(reply_rfd, resp_buf, resp_cap, out_resp_len, NULL, timeout_ms);

    close(reply_rfd);
    close(reply_wfd_self);
    eyn_daemon__lock_release(name);
    return rc;
}

/* ------------------------------------------------------------------- */
/* Daemon side                                                          */
/* ------------------------------------------------------------------- */

/*
 * Create (idempotent) and open the daemon's well-known request FIFO.
 * Call once at startup. Returns a handle, or NULL on failure.
 */
static inline eyn_daemon_t* eyn_daemon_listen(const char* name) {
    if (!name || !name[0]) return NULL;
    eyn_daemon__ensure_dir();

    char req_path[EYN_DAEMON_PATH_MAX];
    eyn_daemon__req_path(name, req_path, sizeof(req_path));
    mkfifo(req_path, 0666);

    int rfd = open(req_path, O_RDONLY);
    /* Keep-alive self-writer: see the comment in eyn_daemon_call() -- the
     * same "no writer yet == spurious EOF" issue applies while we wait
     * for the first real client to connect. */
    int wfd_self = open(req_path, O_WRONLY);
    if (rfd < 0 || wfd_self < 0) {
        if (rfd >= 0) close(rfd);
        if (wfd_self >= 0) close(wfd_self);
        return NULL;
    }

    eyn_daemon_t* d = (eyn_daemon_t*)malloc(sizeof(eyn_daemon_t));
    if (!d) {
        close(rfd);
        close(wfd_self);
        return NULL;
    }
    d->req_read_fd = rfd;
    d->req_selfwrite_fd = wfd_self;
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = '\0';
    return d;
}

/*
 * Block (or time out) waiting for the next incoming message. Copies up to
 * buf_cap payload bytes into buf. If the sender expects a reply,
 * reply_token is filled with an opaque token to pass to eyn_daemon_reply();
 * otherwise reply_token[0] is set to '\0'. reply_token must point to a
 * buffer of at least EYN_DAEMON_PATH_MAX bytes.
 * timeout_ms == 0 waits forever. Returns payload length (>= 0) on success,
 * EYN_DAEMON_ERR or EYN_DAEMON_ERR_TIME on failure.
 */
static inline int eyn_daemon_recv(eyn_daemon_t* d, void* buf, uint32_t buf_cap, uint32_t* out_len,
                                  char reply_token[EYN_DAEMON_PATH_MAX], int timeout_ms) {
    if (!d) return EYN_DAEMON_ERR;
    return eyn_daemon__recv_framed(d->req_read_fd, buf, buf_cap, out_len, reply_token, timeout_ms);
}

/*
 * Send a reply to a specific caller using the token from eyn_daemon_recv().
 * Returns 0 on success, EYN_DAEMON_ERR if reply_token is empty/invalid or
 * the caller has already given up.
 */
static inline int eyn_daemon_reply(const char* reply_token, const void* resp, uint32_t resp_len) {
    if (!reply_token || !reply_token[0]) return EYN_DAEMON_ERR;

    int fd = open(reply_token, O_WRONLY);
    if (fd < 0) return EYN_DAEMON_ERR;

    eyn_daemon_hdr_t hdr;
    hdr.magic = EYN_DAEMON_MAGIC;
    hdr.payload_len = resp_len;
    hdr.wants_reply = 0;
    hdr.reply_path[0] = '\0';

    int rc = eyn_daemon__write_all(fd, &hdr, sizeof(hdr));
    if (rc == 0 && resp_len) rc = eyn_daemon__write_all(fd, resp, resp_len);
    close(fd);
    return rc == 0 ? 0 : EYN_DAEMON_ERR;
}

/* Close the daemon's request FIFO handle. Typically only used at shutdown. */
static inline void eyn_daemon_close(eyn_daemon_t* d) {
    if (!d) return;
    if (d->req_read_fd >= 0) close(d->req_read_fd);
    if (d->req_selfwrite_fd >= 0) close(d->req_selfwrite_fd);
    free(d);
}
