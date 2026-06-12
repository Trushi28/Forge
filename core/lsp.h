#ifndef FORGE_LSP_H
#define FORGE_LSP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── Completion item ────────────────────────────────────────── */

#define LSP_MAX_COMPLETIONS   128
#define LSP_MAX_DIAGNOSTICS   256
#define LSP_MAX_LABEL         128
#define LSP_MAX_DETAIL        256
#define LSP_MAX_URI           512

typedef enum {
    LSP_KIND_TEXT         = 1,
    LSP_KIND_METHOD       = 2,
    LSP_KIND_FUNCTION     = 3,
    LSP_KIND_CONSTRUCTOR  = 4,
    LSP_KIND_FIELD        = 5,
    LSP_KIND_VARIABLE     = 6,
    LSP_KIND_CLASS        = 7,
    LSP_KIND_INTERFACE    = 8,
    LSP_KIND_MODULE       = 9,
    LSP_KIND_PROPERTY     = 10,
    LSP_KIND_KEYWORD      = 14,
    LSP_KIND_SNIPPET      = 15,
    LSP_KIND_STRUCT       = 22,
    LSP_KIND_ENUM         = 13,
    LSP_KIND_CONSTANT     = 21,
    LSP_KIND_TYPE_PARAM   = 25
} LSPCompletionKind;

typedef struct {
    char  label[LSP_MAX_LABEL];
    char  insert_text[LSP_MAX_LABEL];
    char  detail[LSP_MAX_DETAIL];
    int   kind;
} LSPCompletionItem;

/* ── Diagnostic ─────────────────────────────────────────────── */

typedef enum {
    LSP_DIAG_ERROR   = 1,
    LSP_DIAG_WARNING = 2,
    LSP_DIAG_INFO    = 3,
    LSP_DIAG_HINT    = 4
} LSPDiagSeverity;

typedef struct {
    int   start_line, start_col;
    int   end_line,   end_col;
    int   severity;
    char  message[LSP_MAX_DETAIL];
} LSPDiagnostic;

/* ── Hover result ───────────────────────────────────────────── */

typedef struct {
    char contents[4096];
    bool valid;
} LSPHoverResult;

/* ── Definition result ──────────────────────────────────────── */

typedef struct {
    char uri[LSP_MAX_URI];
    int  line, col;
    bool valid;
} LSPDefinitionResult;

/* ── References result ──────────────────────────────────────── */

#define LSP_MAX_REFERENCES 128

typedef struct {
    char uri[LSP_MAX_URI];
    int  line, col;
} LSPReferenceLocation;

typedef struct {
    LSPReferenceLocation locations[LSP_MAX_REFERENCES];
    int count;
    bool valid;
} LSPReferencesResult;

/* ── Pending response types ─────────────────────────────────── */

typedef enum {
    LSP_RESP_NONE,
    LSP_RESP_COMPLETION,
    LSP_RESP_HOVER,
    LSP_RESP_DEFINITION,
    LSP_RESP_REFERENCES,
    LSP_RESP_INITIALIZE
} LSPResponseType;

typedef struct {
    int             id;
    LSPResponseType type;
} LSPPendingRequest;

#define LSP_MAX_PENDING 32

/* ── LSP Client ─────────────────────────────────────────────── */

typedef struct {
    pid_t  child_pid;
    int    stdin_fd;     /* write requests to server */
    int    stdout_fd;    /* read responses from server */

    int    next_id;
    bool   initialized;
    bool   running;

    char   server_name[64];
    char   root_uri[LSP_MAX_URI];

    /* Pending requests */
    LSPPendingRequest pending[LSP_MAX_PENDING];
    int               pending_count;

    /* Read buffer for partial messages */
    char  *read_buf;
    int    read_len;
    int    read_cap;

    /* Last results (polled by main loop) */
    LSPCompletionItem  completions[LSP_MAX_COMPLETIONS];
    int                completion_count;
    bool               completion_ready;

    LSPDiagnostic      diagnostics[LSP_MAX_DIAGNOSTICS];
    int                diagnostic_count;
    bool               diagnostics_ready;

    LSPHoverResult     hover;
    bool               hover_ready;

    LSPDefinitionResult definition;
    bool                definition_ready;

    LSPReferencesResult references;
    bool                references_ready;
} LSPClient;

/* ── Public API ─────────────────────────────────────────────── */

/* Start an LSP server process. Returns NULL on failure. */
LSPClient *lsp_start(const char *server_cmd, const char *root_path);

/* Stop the LSP server */
void lsp_stop(LSPClient *c);

/* Send notifications */
void lsp_send_did_open(LSPClient *c, const char *uri,
                       const char *language_id, const char *text);
void lsp_send_did_change(LSPClient *c, const char *uri,
                         const char *full_text);
void lsp_send_did_close(LSPClient *c, const char *uri);

/* Send requests (responses come via lsp_poll) */
void lsp_request_completion(LSPClient *c, const char *uri,
                            int line, int col);
void lsp_request_hover(LSPClient *c, const char *uri,
                       int line, int col);
void lsp_request_definition(LSPClient *c, const char *uri,
                            int line, int col);
void lsp_request_references(LSPClient *c, const char *uri,
                            int line, int col);

/* Non-blocking poll: read and parse any available responses.
   Sets completion_ready / hover_ready / etc. flags. */
void lsp_poll(LSPClient *c);

/* Auto-detect a language server for a file extension.
   Returns server command or NULL. */
const char *lsp_detect_server(const char *filepath);

/* Convert a file path to a file:// URI */
void lsp_path_to_uri(const char *path, char *uri, size_t uri_sz);

/* Get language ID from file extension */
const char *lsp_language_id(const char *filepath);

#endif
