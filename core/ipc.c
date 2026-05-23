#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   IPC Bridge — connects to forge-net via Unix domain socket

   Messages are length-prefixed: [4 bytes big-endian length][JSON]
   This is a stub that gracefully handles forge-net not running.
   ══════════════════════════════════════════════════════════════ */

void ipc_init(IPCBridge *ipc) {
    ipc->fd = -1;
    ipc->connected = false;
    ipc->read_len = 0;
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
        if (pos + 4 + msg_len <= ipc->read_len) {
            messages++;
            pos += 4 + msg_len;
        } else {
            break;
        }
    }

    return messages;
}

void ipc_free(IPCBridge *ipc) {
    ipc_disconnect(ipc);
}
