#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Messages are length-prefixed: [4 bytes big-endian length][JSON] */

void ipc_socket_path(char *out, size_t out_sz) {
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, out_sz, "%s/.config/forge/net/forge-net.sock", home);
    else
        snprintf(out, out_sz, "/tmp/forge-net/forge-net.sock");
}

void ipc_init(IPCBridge *ipc) {
    ipc->fd = -1;
    ipc->connected = false;
    ipc->read_cap = IPC_READ_BUF_INIT;
    ipc->read_buf = malloc((size_t)ipc->read_cap);
    ipc->read_len = 0;
    ipc->net_pid = -1;
    ipc->reconnect_timer = 0;
    ipc->spawn_failures = 0;
}

bool ipc_connect(IPCBridge *ipc) {
    if (ipc->connected) return true;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    char path[sizeof(addr.sun_path) + 64];
    ipc_socket_path(path, sizeof(path));
    if (strlen(path) >= sizeof(addr.sun_path)) { close(fd); return false; }
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ipc->fd = fd;
    ipc->connected = true;
    ipc->read_len = 0;
    ipc->reconnect_timer = 0;
    ipc->spawn_failures = 0;
    return true;
}

void ipc_disconnect(IPCBridge *ipc) {
    if (ipc->fd >= 0) {
        close(ipc->fd);
        ipc->fd = -1;
    }
    ipc->connected = false;
    ipc->read_len = 0;
}

/* Write everything; the fd is non-blocking, so wait for POLLOUT on EAGAIN.
   MSG_NOSIGNAL: a dead peer must not kill the editor with SIGPIPE. */
static bool send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, 2000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool ipc_send(IPCBridge *ipc, const char *json, int len) {
    if (!ipc->connected || ipc->fd < 0) return false;
    if (len <= 0 || len > IPC_MAX_MSG_SZ) return false;   /* forge-net would drop us */

    unsigned char header[4] = {
        (unsigned char)((len >> 24) & 0xFF), (unsigned char)((len >> 16) & 0xFF),
        (unsigned char)((len >>  8) & 0xFF), (unsigned char)( len        & 0xFF)
    };
    if (!send_all(ipc->fd, header, 4) || !send_all(ipc->fd, json, (size_t)len)) {
        ipc_disconnect(ipc);          /* stream is now desynchronised */
        return false;
    }
    return true;
}

static int frame_len(const IPCBridge *ipc, int pos) {
    return ((unsigned char)ipc->read_buf[pos]     << 24) |
           ((unsigned char)ipc->read_buf[pos + 1] << 16) |
           ((unsigned char)ipc->read_buf[pos + 2] <<  8) |
           ((unsigned char)ipc->read_buf[pos + 3]);
}

int ipc_poll(IPCBridge *ipc) {
    if (!ipc->connected || ipc->fd < 0) return 0;

    /* Drain the socket (previously one 64 KB read per frame, and a message
       larger than the buffer could never complete → permanent stall). */
    for (;;) {
        if (ipc->read_len == ipc->read_cap) {
            int max_cap = IPC_MAX_MSG_SZ + 4;
            if (ipc->read_cap >= max_cap) break;           /* full: parse first */
            int ncap = ipc->read_cap * 2 > max_cap ? max_cap : ipc->read_cap * 2;
            char *nb = realloc(ipc->read_buf, (size_t)ncap);
            if (!nb) { ipc_disconnect(ipc); return 0; }
            ipc->read_buf = nb;
            ipc->read_cap = ncap;
        }
        ssize_t n = read(ipc->fd, ipc->read_buf + ipc->read_len,
                         (size_t)(ipc->read_cap - ipc->read_len));
        if (n > 0) { ipc->read_len += (int)n; continue; }
        if (n == 0) { ipc_disconnect(ipc); return 0; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        ipc_disconnect(ipc);
        return 0;
    }

    int messages = 0, pos = 0;
    while (pos + 4 <= ipc->read_len) {
        int msg_len = frame_len(ipc, pos);
        if (msg_len <= 0 || msg_len > IPC_MAX_MSG_SZ) {
            ipc_disconnect(ipc);        /* corrupt stream: reconnect to resync */
            return 0;
        }
        if (pos + 4 + msg_len > ipc->read_len) break;
        messages++;
        pos += 4 + msg_len;
    }
    return messages;
}

char *ipc_read_message(IPCBridge *ipc) {
    if (ipc->read_len < 4) return NULL;

    int msg_len = frame_len(ipc, 0);
    if (msg_len <= 0 || msg_len > IPC_MAX_MSG_SZ) {
        ipc_disconnect(ipc);
        return NULL;
    }
    if (4 + msg_len > ipc->read_len) return NULL;   /* incomplete */

    char *msg = malloc((size_t)msg_len + 1);
    if (!msg) return NULL;
    memcpy(msg, ipc->read_buf + 4, (size_t)msg_len);
    msg[msg_len] = '\0';

    int consumed = 4 + msg_len;
    int remaining = ipc->read_len - consumed;
    if (remaining > 0)
        memmove(ipc->read_buf, ipc->read_buf + consumed, (size_t)remaining);
    ipc->read_len = remaining;
    return msg;
}

/* ══════════════════════════════════════════════════════════════
   Spawn forge-net
   ══════════════════════════════════════════════════════════════ */

static bool exe_exists(const char *p) { return access(p, X_OK) == 0; }

static void find_net_binary(char *out, size_t sz) {
    const char *env = getenv("FORGE_DIR");
    char exe[1024] = "", dir[1024] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = '\0'; snprintf(dir, sizeof(dir), "%s", exe); }
    }
    const char *roots[] = { env, dir[0] ? dir : NULL, "." };
    const char *rels[]  = { "forge-net", "net/target/release/forge-net",
                            "net/target/debug/forge-net" };
    for (size_t r = 0; r < 3; r++) {
        if (!roots[r]) continue;
        for (size_t i = 0; i < 3; i++) {
            snprintf(out, sz, "%s/%s", roots[r], rels[i]);
            if (exe_exists(out)) return;
        }
    }
    snprintf(out, sz, "forge-net");           /* fall back to $PATH */
}

bool ipc_spawn_net(IPCBridge *ipc) {
    if (ipc->net_pid > 0) {
        int status;
        pid_t r = waitpid(ipc->net_pid, &status, WNOHANG);
        if (r == 0) return true;               /* still running */
        ipc->net_pid = -1;                     /* exited: reaped */
        ipc->spawn_failures++;
    }
    if (ipc->spawn_failures >= 3) return false; /* not installed / crash-looping */

    char net_path[1100];
    find_net_binary(net_path, sizeof(net_path));

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const char *home = getenv("HOME");
        char log_path[512];
        if (home) snprintf(log_path, sizeof(log_path), "%s/.config/forge/forge-net.log", home);
        else      snprintf(log_path, sizeof(log_path), "/tmp/forge-net.log");
        if (!freopen(log_path, "a", stdout)) freopen("/dev/null", "w", stdout);
        if (!freopen(log_path, "a", stderr)) freopen("/dev/null", "w", stderr);
        execlp(net_path, "forge-net", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        ipc->net_pid = pid;
        return true;
    }
    return false;
}

bool ipc_try_connect(IPCBridge *ipc) {
    if (ipc->connected) return true;

    if (ipc->reconnect_timer > 0) { ipc->reconnect_timer--; return false; }

    /* 1. Someone (a previous run, or the user) may already have forge-net up:
          never spawn a second instance over a live socket. */
    if (ipc_connect(ipc)) return true;

    /* 2. Nobody home: (re)spawn, then retry shortly.  A frame is ~100 ms
          (input timeout), so 5 frames ≈ 0.5 s, 20 frames ≈ 2 s. */
    if (ipc_spawn_net(ipc)) {
        ipc->reconnect_timer = 5;
        return false;
    }
    ipc->reconnect_timer = 100;                /* spawn disabled: poll rarely */
    return false;
}

void ipc_free(IPCBridge *ipc) {
    ipc_disconnect(ipc);
    free(ipc->read_buf);
    ipc->read_buf = NULL;
    ipc->read_cap = 0;

    if (ipc->net_pid > 0) {
        kill(ipc->net_pid, SIGTERM);
        usleep(100000);
        int status;
        if (waitpid(ipc->net_pid, &status, WNOHANG) == 0) {
            kill(ipc->net_pid, SIGKILL);
            waitpid(ipc->net_pid, &status, 0);
        }
        ipc->net_pid = -1;
    }
}
