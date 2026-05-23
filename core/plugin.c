#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ══════════════════════════════════════════════════════════════
   Shell hooks — execute shell commands on editor events

   The command string can contain $FILE which is replaced with
   the actual file path. Commands run in the background (forked)
   so they don't block the editor.
   ══════════════════════════════════════════════════════════════ */

void plugin_run_hook(const char *command, const char *filepath) {
    if (!command || command[0] == '\0') return;

    /* Expand $FILE in the command */
    char expanded[1024];
    int  elen = 0;
    int  clen = (int)strlen(command);

    for (int i = 0; i < clen && elen < (int)sizeof(expanded) - 1; i++) {
        if (command[i] == '$' && i + 4 < clen &&
            memcmp(command + i + 1, "FILE", 4) == 0) {
            /* Replace $FILE with filepath */
            if (filepath) {
                int flen = (int)strlen(filepath);
                if (elen + flen < (int)sizeof(expanded) - 1) {
                    memcpy(expanded + elen, filepath, flen);
                    elen += flen;
                }
            }
            i += 4;  /* skip FILE */
        } else {
            expanded[elen++] = command[i];
        }
    }
    expanded[elen] = '\0';

    /* Fork and exec in background */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        setsid();  /* detach from terminal */

        /* Redirect stdout/stderr to /dev/null */
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    }

    /* Parent: don't wait (fire and forget) */
    /* Reap zombie later */
    if (pid > 0) {
        /* Non-blocking wait to reap any previous zombies */
        waitpid(-1, NULL, WNOHANG);
    }
}

/* ══════════════════════════════════════════════════════════════
   Plugin host (stub for future dlopen-based loading)
   ══════════════════════════════════════════════════════════════ */

void plugin_host_init(PluginHost *ph) {
    ph->initialized = true;
}

void plugin_host_free(PluginHost *ph) {
    ph->initialized = false;
}

void plugin_on_save(PluginHost *ph, const char *filepath) {
    (void)ph;
    (void)filepath;
    /* Future: iterate loaded plugins and fire on_save callbacks */
}

void plugin_on_open(PluginHost *ph, const char *filepath) {
    (void)ph;
    (void)filepath;
}

void plugin_on_close(PluginHost *ph, const char *filepath) {
    (void)ph;
    (void)filepath;
}
