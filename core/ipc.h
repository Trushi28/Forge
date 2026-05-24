#ifndef FORGE_IPC_H
#define FORGE_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── IPC Bridge — C ↔ forge-net (Rust) via Unix socket ────── */

#define IPC_SOCKET_PATH "/tmp/forge-net.sock"
#define IPC_READ_BUF_SZ 65536
#define IPC_MAX_MSG_SZ  (1024 * 1024)

typedef struct {
    int    fd;              /* socket file descriptor (-1 = disconnected) */
    bool   connected;
    char   read_buf[IPC_READ_BUF_SZ];
    int    read_len;

    /* forge-net process management */
    pid_t  net_pid;         /* PID of forge-net child process (-1 = not started) */
    int    reconnect_timer; /* countdown frames until next reconnect attempt */
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

/* Extract and return a complete JSON message from the read buffer.
   Caller must free() the returned string.
   Returns NULL if no complete message is available. */
char *ipc_read_message(IPCBridge *ipc);

/* Attempt periodic reconnection (call every N frames).
   Also auto-spawns forge-net if not running. */
bool ipc_try_connect(IPCBridge *ipc);

/* Spawn the forge-net binary as a child process.
   Returns true if spawned successfully. */
bool ipc_spawn_net(IPCBridge *ipc);

/* Free resources and kill forge-net if we spawned it */
void ipc_free(IPCBridge *ipc);

#endif
