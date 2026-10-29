#ifndef FORGE_IPC_H
#define FORGE_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── IPC Bridge — C ↔ forge-net (Rust) via Unix socket ──────
   Socket lives at $XDG_CONFIG_HOME/forge/net/forge-net.sock when the
   XDG path is absolute, otherwise $HOME/.config/forge/net/forge-net.sock
   (or /tmp/forge-net/forge-net.sock without HOME). forge-net shares it. */

#define IPC_READ_BUF_INIT 65536
#define IPC_MAX_MSG_SZ    (1024 * 1024)

typedef struct {
    int    fd;              /* socket file descriptor (-1 = disconnected) */
    bool   connected;
    char  *read_buf;        /* grows up to IPC_MAX_MSG_SZ + 4 */
    int    read_len;
    int    read_cap;

    /* forge-net process management */
    pid_t  net_pid;         /* PID of forge-net child process (-1 = not started) */
    int    reconnect_timer; /* countdown frames until next reconnect attempt */
    int    spawn_failures;  /* consecutive early exits of the spawned child */
} IPCBridge;

/* Fill `out` with the socket path. */
void ipc_socket_path(char *out, size_t out_sz);

void ipc_init(IPCBridge *ipc);
bool ipc_connect(IPCBridge *ipc);
void ipc_disconnect(IPCBridge *ipc);

/* Send a length-prefixed JSON message (handles partial writes, no SIGPIPE). */
bool ipc_send(IPCBridge *ipc, const char *json, int len);

/* Non-blocking poll: drain the socket. Returns complete messages buffered. */
int ipc_poll(IPCBridge *ipc);

/* Pop one complete message (malloc'd, caller frees) or NULL. */
char *ipc_read_message(IPCBridge *ipc);

/* Periodic reconnect; connects to an already-running forge-net first and
   only spawns one if none answers. Call once per frame. */
bool ipc_try_connect(IPCBridge *ipc);

bool ipc_spawn_net(IPCBridge *ipc);
void ipc_free(IPCBridge *ipc);

#endif
