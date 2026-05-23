#ifndef FORGE_IPC_H
#define FORGE_IPC_H

#include <stdbool.h>
#include <stddef.h>

/* ── IPC Bridge — C ↔ forge-net (Rust) via Unix socket ────── */

#define IPC_SOCKET_PATH "/tmp/forge-net.sock"
#define IPC_READ_BUF_SZ 65536

typedef struct {
    int    fd;              /* socket file descriptor (-1 = disconnected) */
    bool   connected;
    char   read_buf[IPC_READ_BUF_SZ];
    int    read_len;
} IPCBridge;

/* Initialize the bridge (does not connect) */
void ipc_init(IPCBridge *ipc);

/* Try to connect to the forge-net socket. Returns true on success. */
bool ipc_connect(IPCBridge *ipc);

/* Disconnect */
void ipc_disconnect(IPCBridge *ipc);

/* Send a length-prefixed JSON message */
bool ipc_send(IPCBridge *ipc, const char *json, int len);

/* Non-blocking poll: read any available data.
   Returns number of complete messages available. */
int ipc_poll(IPCBridge *ipc);

/* Free resources */
void ipc_free(IPCBridge *ipc);

#endif
