#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   IPC Bridge — connects to forge-net via Unix domain socket

   Messages are length-prefixed: [4 bytes big-endian length][JSON]
   Now includes auto-spawn of forge-net, message extraction, and
   periodic reconnection.
   ══════════════════════════════════════════════════════════════ */

void ipc_init(IPCBridge *ipc) {
    ipc->fd = -1;
    ipc->connected = false;
    ipc->read_len = 0;
    ipc->net_pid = -1;
    ipc->reconnect_timer = 0;
}

bool ipc_connect(IPCBridge *ipc) {
    if (ipc->connected) return true;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ipc->fd = fd;
    ipc->connected = true;
    ipc->reconnect_timer = 0;
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

bool ipc_send(IPCBridge *ipc, const char *json, int len) {
    if (!ipc->connected || ipc->fd < 0) return false;

    /* Length prefix: 4 bytes big-endian */
    unsigned char header[4];
    header[0] = (unsigned char)((len >> 24) & 0xFF);
    header[1] = (unsigned char)((len >> 16) & 0xFF);
    header[2] = (unsigned char)((len >>  8) & 0xFF);
    header[3] = (unsigned char)( len        & 0xFF);

    if (write(ipc->fd, header, 4) != 4) {
        ipc_disconnect(ipc);
        return false;
    }
    if (write(ipc->fd, json, len) != len) {
        ipc_disconnect(ipc);
        return false;
    }
    return true;
}

int ipc_poll(IPCBridge *ipc) {
    if (!ipc->connected || ipc->fd < 0) return 0;

    /* Non-blocking read */
    int avail = IPC_READ_BUF_SZ - ipc->read_len;
    if (avail <= 0) return 0;

    ssize_t n = read(ipc->fd, ipc->read_buf + ipc->read_len, avail);
    if (n > 0) {
        ipc->read_len += (int)n;
    } else if (n == 0) {
        /* Connection closed */
        ipc_disconnect(ipc);
        return 0;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ipc_disconnect(ipc);
        return 0;
    }

    /* Count complete messages */
    int messages = 0;
    int pos = 0;
    while (pos + 4 <= ipc->read_len) {
        int msg_len = ((unsigned char)ipc->read_buf[pos]     << 24) |
                      ((unsigned char)ipc->read_buf[pos + 1] << 16) |
                      ((unsigned char)ipc->read_buf[pos + 2] <<  8) |
                      ((unsigned char)ipc->read_buf[pos + 3]);
        if (msg_len > IPC_MAX_MSG_SZ) {
            /* Corrupted stream — reset */
            ipc->read_len = 0;
            return 0;
        }
        if (pos + 4 + msg_len <= ipc->read_len) {
            messages++;
            pos += 4 + msg_len;
        } else {
            break;
        }
    }

    return messages;
}

/* ══════════════════════════════════════════════════════════════
   Message extraction

   Reads one complete length-prefixed message from the buffer
   and returns it as a malloc'd string. Shifts remaining data
   in the buffer. Returns NULL if no complete message available.
   ══════════════════════════════════════════════════════════════ */

char *ipc_read_message(IPCBridge *ipc) {
    if (ipc->read_len < 4) return NULL;

    int msg_len = ((unsigned char)ipc->read_buf[0]     << 24) |
                  ((unsigned char)ipc->read_buf[1] << 16) |
                  ((unsigned char)ipc->read_buf[2] <<  8) |
                  ((unsigned char)ipc->read_buf[3]);

    if (msg_len <= 0 || msg_len > IPC_MAX_MSG_SZ) {
        /* Corrupted — discard */
        ipc->read_len = 0;
        return NULL;
    }

    if (4 + msg_len > ipc->read_len) return NULL;  /* incomplete */

    /* Extract the message */
    char *msg = malloc(msg_len + 1);
    if (!msg) return NULL;
    memcpy(msg, ipc->read_buf + 4, msg_len);
    msg[msg_len] = '\0';

    /* Shift remaining data */
    int consumed = 4 + msg_len;
    int remaining = ipc->read_len - consumed;
    if (remaining > 0)
        memmove(ipc->read_buf, ipc->read_buf + consumed, remaining);
    ipc->read_len = remaining;

    return msg;
}

/* ══════════════════════════════════════════════════════════════
   Auto-spawn forge-net
   ══════════════════════════════════════════════════════════════ */

bool ipc_spawn_net(IPCBridge *ipc) {
    if (ipc->net_pid > 0) {
        /* Check if still running */
        int status;
        pid_t result = waitpid(ipc->net_pid, &status, WNOHANG);
        if (result == 0) {
            /* Still running */
            return true;
        }
        /* Exited — we'll respawn */
        ipc->net_pid = -1;
    }

    /* Find the forge-net binary */
    char net_path[1024];
    /* Try relative to forge binary first */
    const char *exe_dir = getenv("FORGE_DIR");
    if (exe_dir) {
        snprintf(net_path, sizeof(net_path), "%s/net/target/debug/forge-net", exe_dir);
    } else {
        /* Try common locations */
        snprintf(net_path, sizeof(net_path), "./net/target/debug/forge-net");
    }

    /* Check if the binary exists */
    if (access(net_path, X_OK) != 0) {
        /* Try release build */
        if (exe_dir)
            snprintf(net_path, sizeof(net_path), "%s/net/target/release/forge-net", exe_dir);
        else
            snprintf(net_path, sizeof(net_path), "./net/target/release/forge-net");

        if (access(net_path, X_OK) != 0) {
            /* Try PATH */
            snprintf(net_path, sizeof(net_path), "forge-net");
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        setsid();

        /* Redirect stdout/stderr to log file */
        const char *home = getenv("HOME");
        char log_path[512];
        if (home)
            snprintf(log_path, sizeof(log_path), "%s/.config/forge/forge-net.log", home);
        else
            snprintf(log_path, sizeof(log_path), "/tmp/forge-net.log");

        freopen(log_path, "a", stdout);
        freopen(log_path, "a", stderr);

        execlp(net_path, "forge-net", (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        ipc->net_pid = pid;
        fprintf(stderr, "forge: spawned forge-net (pid %d)\n", pid);
        return true;
    }

    return false;
}

bool ipc_try_connect(IPCBridge *ipc) {
    if (ipc->connected) return true;

    /* Decrement reconnect timer */
    if (ipc->reconnect_timer > 0) {
        ipc->reconnect_timer--;
        return false;
    }

    /* Try spawning forge-net if not already running */
    if (ipc->net_pid <= 0) {
        ipc_spawn_net(ipc);
        ipc->reconnect_timer = 60;  /* wait ~1 second before trying to connect */
        return false;
    }

    /* Try connecting */
    if (ipc_connect(ipc)) {
        fprintf(stderr, "forge: connected to forge-net\n");
        return true;
    }

    /* Back off: wait 3 seconds before next attempt */
    ipc->reconnect_timer = 180;
    return false;
}

void ipc_free(IPCBridge *ipc) {
    ipc_disconnect(ipc);

    /* Kill forge-net if we spawned it */
    if (ipc->net_pid > 0) {
        kill(ipc->net_pid, SIGTERM);
        /* Give it a moment to clean up */
        usleep(100000);  /* 100ms */
        int status;
        pid_t result = waitpid(ipc->net_pid, &status, WNOHANG);
        if (result == 0) {
            /* Still running — force kill */
            kill(ipc->net_pid, SIGKILL);
            waitpid(ipc->net_pid, &status, 0);
        }
        ipc->net_pid = -1;
    }
}
