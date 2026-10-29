#include "lsp.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <strings.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════
   Minimal JSON writer — emits LSP JSON-RPC requests directly
   into a growable buffer, then wraps with Content-Length header.
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    char *buf;
    int   len, cap;
    int   depth;
} JsonWriter;

static void jw_init(JsonWriter *w) {
    w->cap  = 4096;
    w->len  = 0;
    w->buf  = malloc(w->cap);
    w->depth = 0;
}

static void jw_free(JsonWriter *w) { free(w->buf); }

static void jw_grow(JsonWriter *w, int need) {
    if (w->len + need <= w->cap) return;
    w->cap = (w->len + need) * 2;
    w->buf = realloc(w->buf, w->cap);
}

static void jw_raw(JsonWriter *w, const char *s, int n) {
    jw_grow(w, n);
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void jw_str_raw(JsonWriter *w, const char *s) {
    jw_raw(w, s, (int)strlen(s));
}

static void jw_obj_start(JsonWriter *w) {
    jw_str_raw(w, "{");
    w->depth++;
}

static void jw_obj_end(JsonWriter *w) {
    /* Remove trailing comma if present */
    if (w->len > 0 && w->buf[w->len - 1] == ',')
        w->len--;
    jw_str_raw(w, "}");
    w->depth--;
}

static void jw_arr_start(JsonWriter *w) { jw_str_raw(w, "["); }

static void jw_arr_end(JsonWriter *w) {
    if (w->len > 0 && w->buf[w->len - 1] == ',')
        w->len--;
    jw_str_raw(w, "]");
}

static void jw_key(JsonWriter *w, const char *key) {
    jw_str_raw(w, "\"");
    jw_str_raw(w, key);
    jw_str_raw(w, "\":");
}

static void jw_string(JsonWriter *w, const char *val) {
    jw_str_raw(w, "\"");
    /* Escape special characters */
    for (const char *p = val; *p; p++) {
        switch (*p) {
            case '"':  jw_str_raw(w, "\\\""); break;
            case '\\': jw_str_raw(w, "\\\\"); break;
            case '\n': jw_str_raw(w, "\\n");  break;
            case '\r': jw_str_raw(w, "\\r");  break;
            case '\t': jw_str_raw(w, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof esc, "\\u%04x", (unsigned char)*p);
                    jw_raw(w, esc, n);
                } else {
                    jw_raw(w, p, 1);
                }
                break;
        }
    }
    jw_str_raw(w, "\",");
}

static void jw_int(JsonWriter *w, int val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%d,", val);
    jw_raw(w, tmp, n);
}

/* jw_null available if needed:
static void jw_null(JsonWriter *w) { jw_str_raw(w, "null,"); }
*/

/* ══════════════════════════════════════════════════════════════
   Minimal JSON reader — recursive descent parser for responses

   Parses into a simple tree of JValue nodes allocated from a
   bump allocator.  Good enough for LSP response payloads.
   ══════════════════════════════════════════════════════════════ */

typedef enum {
    JV_NULL, JV_BOOL, JV_INT, JV_STRING, JV_ARRAY, JV_OBJECT
} JValueType;

typedef struct JValue JValue;

typedef struct JKV {
    char    *key;
    JValue  *val;
    struct JKV *next;
} JKV;

struct JValue {
    JValueType type;
    union {
        bool    b;
        int     i;
        char   *s;
        struct {            /* array */
            JValue **items;
            int      count;
        } arr;
        JKV *obj;           /* linked list of key-value pairs */
    };
};

/* Chunked bump arena for JSON values.  Chunks never move, so pointers
   handed out earlier stay valid (the old single realloc'd buffer left every
   already-parsed JValue* dangling on large responses). */
typedef struct JChunk {
    struct JChunk *next;
    int  len, cap;
    char data[];
} JChunk;

typedef struct { JChunk *head; } JsonArena;

static JsonArena *ja_new(int cap) {
    (void)cap;
    return calloc(1, sizeof(JsonArena));
}

static void *ja_alloc(JsonArena *a, int size) {
    int aligned = (size + 15) & ~15;
    if (!a->head || a->head->len + aligned > a->head->cap) {
        int cap = aligned > 65536 ? aligned : 65536;
        JChunk *c = malloc(sizeof(JChunk) + (size_t)cap);
        c->next = a->head;
        c->len  = 0;
        c->cap  = cap;
        a->head = c;
    }
    void *p = a->head->data + a->head->len;
    a->head->len += aligned;
    return p;
}

static void ja_free(JsonArena *a) {
    for (JChunk *c = a->head; c;) {
        JChunk *n = c->next;
        free(c);
        c = n;
    }
    free(a);
}

/* ja_strdup available if needed:
static char *ja_strdup(JsonArena *a, const char *s, int len) {
    char *d = ja_alloc(a, len + 1); memcpy(d, s, len); d[len] = '\0'; return d;
}
*/

/* Parser state */
typedef struct {
    const char *src;
    int         pos;
    int         len;
    JsonArena  *arena;
} JParser;

static void jp_skip_ws(JParser *p) {
    while (p->pos < p->len && (p->src[p->pos] == ' ' || p->src[p->pos] == '\n' ||
           p->src[p->pos] == '\r' || p->src[p->pos] == '\t'))
        p->pos++;
}

static JValue *jp_parse(JParser *p);

static int jp_hex4(const char *s) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static char *jp_parse_string_raw(JParser *p, int *out_len) {
    if (p->pos >= p->len || p->src[p->pos] != '"') return NULL;
    p->pos++;  /* skip opening quote */

    int start = p->pos;
    /* Find end, handling escapes */
    while (p->pos < p->len && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') p->pos++;
        p->pos++;
    }
    int slen = p->pos - start;
    p->pos++;  /* skip closing quote */

    /* Unescape */
    char *result = ja_alloc(p->arena, slen + 1);
    int ri = 0;
    for (int i = start; i < start + slen; i++) {
        if (p->src[i] == '\\' && i + 1 < start + slen) {
            i++;
            switch (p->src[i]) {
                case '"':  result[ri++] = '"';  break;
                case '\\': result[ri++] = '\\'; break;
                case 'n':  result[ri++] = '\n'; break;
                case 'r':  result[ri++] = '\r'; break;
                case 't':  result[ri++] = '\t'; break;
                case '/':  result[ri++] = '/';  break;
                case 'b':  result[ri++] = '\b'; break;
                case 'f':  result[ri++] = '\f'; break;
                case 'u': {
                    int end = start + slen, cp;
                    if (i + 4 < end && (cp = jp_hex4(p->src + i + 1)) >= 0) {
                        i += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < end &&
                            p->src[i + 1] == '\\' && p->src[i + 2] == 'u') {
                            int lo = jp_hex4(p->src + i + 3);
                            if (lo >= 0xDC00 && lo < 0xE000) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i += 6;
                            }
                        }
                        if (cp < 0x80) result[ri++] = (char)cp;
                        else if (cp < 0x800) {
                            result[ri++] = (char)(0xC0 | (cp >> 6));
                            result[ri++] = (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            result[ri++] = (char)(0xE0 | (cp >> 12));
                            result[ri++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            result[ri++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            result[ri++] = (char)(0xF0 | (cp >> 18));
                            result[ri++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            result[ri++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            result[ri++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        result[ri++] = 'u';
                    }
                    break;
                }
                default:   result[ri++] = p->src[i]; break;
            }
        } else {
            result[ri++] = p->src[i];
        }
    }
    result[ri] = '\0';
    if (out_len) *out_len = ri;
    return result;
}

static JValue *jp_parse_string(JParser *p) {
    JValue *v = ja_alloc(p->arena, sizeof(JValue));
    v->type = JV_STRING;
    v->s = jp_parse_string_raw(p, NULL);
    return v;
}

static JValue *jp_parse_number(JParser *p) {
    JValue *v = ja_alloc(p->arena, sizeof(JValue));
    v->type = JV_INT;
    int sign = 1;
    if (p->src[p->pos] == '-') { sign = -1; p->pos++; }
    int n = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) {
        n = n * 10 + (p->src[p->pos] - '0');
        p->pos++;
    }
    /* Skip decimal and fractional part if present */
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos]))
            p->pos++;
    }
    v->i = sign * n;
    return v;
}

static JValue *jp_parse_array(JParser *p) {
    p->pos++;  /* skip [ */
    JValue *v = ja_alloc(p->arena, sizeof(JValue));
    v->type = JV_ARRAY;

    /* Temp storage */
    int cap = 16;
    JValue **items = ja_alloc(p->arena, cap * (int)sizeof(JValue*));
    int count = 0;

    jp_skip_ws(p);
    while (p->pos < p->len && p->src[p->pos] != ']') {
        JValue *item = jp_parse(p);
        if (!item) break;
        if (count >= cap) {
            cap *= 2;
            JValue **new_items = ja_alloc(p->arena, cap * (int)sizeof(JValue*));
            memcpy(new_items, items, count * sizeof(JValue*));
            items = new_items;
        }
        items[count++] = item;
        jp_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        jp_skip_ws(p);
    }
    if (p->pos < p->len) p->pos++;  /* skip ] */
    v->arr.items = items;
    v->arr.count = count;
    return v;
}

static JValue *jp_parse_object(JParser *p) {
    p->pos++;  /* skip { */
    JValue *v = ja_alloc(p->arena, sizeof(JValue));
    v->type = JV_OBJECT;
    v->obj  = NULL;

    JKV *tail = NULL;
    jp_skip_ws(p);
    while (p->pos < p->len && p->src[p->pos] != '}') {
        jp_skip_ws(p);
        if (p->src[p->pos] != '"') break;
        char *key = jp_parse_string_raw(p, NULL);
        jp_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ':') p->pos++;
        jp_skip_ws(p);
        JValue *val = jp_parse(p);

        JKV *kv = ja_alloc(p->arena, sizeof(JKV));
        kv->key  = key;
        kv->val  = val;
        kv->next = NULL;
        if (!v->obj) { v->obj = kv; tail = kv; }
        else         { tail->next = kv; tail = kv; }

        jp_skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') p->pos++;
        jp_skip_ws(p);
    }
    if (p->pos < p->len) p->pos++;  /* skip } */
    return v;
}

static JValue *jp_parse(JParser *p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) return NULL;

    char c = p->src[p->pos];
    if (c == '"')  return jp_parse_string(p);
    if (c == '{')  return jp_parse_object(p);
    if (c == '[')  return jp_parse_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return jp_parse_number(p);
    if (c == 't') { p->pos += 4; JValue *v = ja_alloc(p->arena, sizeof(JValue)); v->type = JV_BOOL; v->b = true; return v; }
    if (c == 'f') { p->pos += 5; JValue *v = ja_alloc(p->arena, sizeof(JValue)); v->type = JV_BOOL; v->b = false; return v; }
    if (c == 'n') { p->pos += 4; JValue *v = ja_alloc(p->arena, sizeof(JValue)); v->type = JV_NULL; return v; }

    /* Unknown char — skip */
    p->pos++;
    return NULL;
}

/* JSON value lookup helpers */
static JValue *jv_get(JValue *obj, const char *key) {
    if (!obj || obj->type != JV_OBJECT) return NULL;
    for (JKV *kv = obj->obj; kv; kv = kv->next) {
        if (kv->key && strcmp(kv->key, key) == 0) return kv->val;
    }
    return NULL;
}

static const char *jv_str(JValue *v) {
    return (v && v->type == JV_STRING) ? v->s : "";
}

static int jv_int(JValue *v) {
    return (v && v->type == JV_INT) ? v->i : 0;
}

/* ══════════════════════════════════════════════════════════════
   LSP transport — Content-Length framing
   ══════════════════════════════════════════════════════════════ */

static void lsp_send_raw(LSPClient *c, const char *json, int json_len) {
    char header[128];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %d\r\n\r\n", json_len);
    const char *parts[] = { header, json };
    size_t sizes[] = { (size_t)hlen, (size_t)json_len };
    for (int part = 0; part < 2; part++) {
        size_t sent = 0;
        while (sent < sizes[part]) {
            ssize_t n = write(c->stdin_fd, parts[part] + sent, sizes[part] - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { c->running = false; return; }
            sent += (size_t)n;
        }
    }
}

static void lsp_send_request(LSPClient *c, const char *method,
                             const char *params_json, int params_len,
                             LSPResponseType resp_type) {
    int id = c->next_id++;

    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "jsonrpc"); jw_string(&w, "2.0");
    jw_key(&w, "id");      jw_int(&w, id);
    jw_key(&w, "method");  jw_string(&w, method);
    jw_key(&w, "params");
    jw_raw(&w, params_json, params_len);
    jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_raw(c, w.buf, w.len);
    jw_free(&w);

    /* Track pending */
    if (c->pending_count < LSP_MAX_PENDING) {
        c->pending[c->pending_count].id   = id;
        c->pending[c->pending_count].type = resp_type;
        c->pending_count++;
    }
}

static void lsp_send_notification(LSPClient *c, const char *method,
                                  const char *params_json, int params_len) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "jsonrpc"); jw_string(&w, "2.0");
    jw_key(&w, "method");  jw_string(&w, method);
    jw_key(&w, "params");
    jw_raw(&w, params_json, params_len);
    jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_raw(c, w.buf, w.len);
    jw_free(&w);
}

/* ══════════════════════════════════════════════════════════════
   Server auto-detection
   ══════════════════════════════════════════════════════════════ */

static bool cmd_exists(const char *cmd) {
    if (!cmd || !cmd[0]) return false;
    if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    const char *part = path;
    while (1) {
        const char *end = strchr(part, ':');
        size_t dir_len = end ? (size_t)(end - part) : strlen(part);
        char candidate[PATH_MAX];
        int n;
        if (dir_len == 0)
            n = snprintf(candidate, sizeof(candidate), "%s", cmd);
        else
            n = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                         (int)dir_len, part, cmd);
        if (n > 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0)
            return true;
        if (!end) break;
        part = end + 1;
    }
    return false;
}

const char *lsp_detect_server(const char *filepath) {
    if (!filepath) return NULL;

    const char *ext = strrchr(filepath, '.');
    if (!ext) return NULL;

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
        strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
        strcmp(ext, ".cxx") == 0 || strcmp(ext, ".hpp") == 0) {
        if (cmd_exists("clangd")) return "clangd";
    }
    else if (strcmp(ext, ".py") == 0) {
        if (cmd_exists("pyright-langserver")) return "pyright-langserver --stdio";
        if (cmd_exists("pylsp")) return "pylsp";
    }
    else if (strcmp(ext, ".rs") == 0) {
        if (cmd_exists("rust-analyzer")) return "rust-analyzer";
    }
    else if (strcmp(ext, ".js") == 0 || strcmp(ext, ".ts") == 0 ||
             strcmp(ext, ".jsx") == 0 || strcmp(ext, ".tsx") == 0) {
        if (cmd_exists("typescript-language-server"))
            return "typescript-language-server --stdio";
    }
    else if (strcmp(ext, ".go") == 0) {
        if (cmd_exists("gopls")) return "gopls";
    }
    else if (strcmp(ext, ".lua") == 0) {
        if (cmd_exists("lua-language-server")) return "lua-language-server";
    }

    return NULL;
}

const char *lsp_language_id(const char *filepath) {
    if (!filepath) return "plaintext";
    const char *ext = strrchr(filepath, '.');
    if (!ext) return "plaintext";

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0)
        return "c";
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 ||
        strcmp(ext, ".cxx") == 0 || strcmp(ext, ".hpp") == 0)
        return "cpp";
    if (strcmp(ext, ".py") == 0)   return "python";
    if (strcmp(ext, ".rs") == 0)   return "rust";
    if (strcmp(ext, ".js") == 0)   return "javascript";
    if (strcmp(ext, ".ts") == 0)   return "typescript";
    if (strcmp(ext, ".jsx") == 0)  return "javascriptreact";
    if (strcmp(ext, ".tsx") == 0)  return "typescriptreact";
    if (strcmp(ext, ".go") == 0)   return "go";
    if (strcmp(ext, ".lua") == 0)  return "lua";
    if (strcmp(ext, ".json") == 0) return "json";
    if (strcmp(ext, ".md") == 0)   return "markdown";
    if (strcmp(ext, ".toml") == 0) return "toml";
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return "yaml";

    return "plaintext";
}

void lsp_path_to_uri(const char *path, char *uri, size_t uri_sz) {
    if (!uri || uri_sz == 0) return;
    uri[0] = '\0';
    if (!path) return;
    char abs[PATH_MAX];
    const char *source = path;
    if (path[0] != '/') source = realpath(path, abs) ? abs : path;
    size_t used = 0;
    const char prefix[] = "file://";
    if (sizeof(prefix) - 1 >= uri_sz) return;
    memcpy(uri, prefix, sizeof(prefix) - 1);
    used = sizeof(prefix) - 1;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)source; *p; p++) {
        bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '/' || *p == '-' ||
                    *p == '_' || *p == '.' || *p == '~';
        size_t need = safe ? 1 : 3;
        if (used + need >= uri_sz) { uri[0] = '\0'; return; }
        if (safe) uri[used++] = (char)*p;
        else {
            uri[used++] = '%';
            uri[used++] = hex[*p >> 4];
            uri[used++] = hex[*p & 15];
        }
    }
    uri[used] = '\0';
}

/* ══════════════════════════════════════════════════════════════
   Start / Stop
   ══════════════════════════════════════════════════════════════ */

/* Split a configured executable and arguments without invoking a shell.
   Whitespace separates arguments; single/double quotes and backslash escapes
   are supported. Shell expansion, pipes, and command substitution are not. */
static char **parse_command_argv(const char *command, char **storage_out) {
    enum { MAX_ARGS = 32 };
    if (!command || !command[0]) return NULL;
    char *storage = strdup(command);
    char **argv = calloc(MAX_ARGS + 1, sizeof(*argv));
    if (!storage || !argv) { free(storage); free(argv); return NULL; }

    char *readp = storage, *writep = storage;
    int argc = 0;
    while (*readp) {
        while (*readp == ' ' || *readp == '\t' || *readp == '\r' || *readp == '\n')
            readp++;
        if (!*readp) break;
        if (argc == MAX_ARGS) { free(storage); free(argv); return NULL; }

        argv[argc++] = writep;
        char quote = 0;
        bool started = false;
        while (*readp) {
            char ch = *readp++;
            if (quote) {
                if (ch == quote) { quote = 0; started = true; continue; }
                if (ch == '\\' && quote == '"' && *readp) ch = *readp++;
                *writep++ = ch;
                started = true;
                continue;
            }
            if (ch == '\'' || ch == '"') { quote = ch; started = true; continue; }
            if (ch == '\\' && *readp) { *writep++ = *readp++; started = true; continue; }
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
            *writep++ = ch;
            started = true;
        }
        if (quote || !started) { free(storage); free(argv); return NULL; }
        *writep++ = '\0';
    }
    if (argc == 0) { free(storage); free(argv); return NULL; }
    argv[argc] = NULL;
    *storage_out = storage;
    return argv;
}

LSPClient *lsp_start(const char *server_cmd, const char *root_path) {
    if (!server_cmd) return NULL;

    char *argv_storage = NULL;
    char **argv = parse_command_argv(server_cmd, &argv_storage);
    if (!argv) return NULL;

    int pipe_stdin[2], pipe_stdout[2];
    if (pipe(pipe_stdin) < 0) { free(argv); free(argv_storage); return NULL; }
    if (pipe(pipe_stdout) < 0) {
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        free(argv); free(argv_storage); return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        free(argv); free(argv_storage); return NULL;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        /* Child: redirect stdin/stdout */
        dup2(pipe_stdin[0],  STDIN_FILENO);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        close(pipe_stdin[1]);
        close(pipe_stdout[0]);
        close(pipe_stdin[0]);
        close(pipe_stdout[1]);

        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        execvp(argv[0], argv);
        _exit(127);
    }

    (void)setpgid(pid, pid);
    free(argv);
    free(argv_storage);

    /* Parent */
    close(pipe_stdin[0]);
    close(pipe_stdout[1]);

    LSPClient *c = calloc(1, sizeof(LSPClient));
    if (!c) {
        close(pipe_stdin[1]); close(pipe_stdout[0]);
        kill(-pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    c->child_pid = pid;
    c->stdin_fd  = pipe_stdin[1];
    c->stdout_fd = pipe_stdout[0];
    c->next_id   = 1;
    c->running   = true;

    /* Make stdout non-blocking */
    int flags = fcntl(c->stdout_fd, F_GETFL, 0);
    fcntl(c->stdout_fd, F_SETFL, flags | O_NONBLOCK);

    /* Read buffer */
    c->read_cap = 65536;
    c->read_buf = malloc(c->read_cap);
    if (!c->read_buf) {
        close(c->stdin_fd); close(c->stdout_fd);
        kill(-pid, SIGTERM); waitpid(pid, NULL, 0);
        free(c);
        return NULL;
    }
    c->read_len = 0;

    /* Store server name */
    const char *slash = strrchr(server_cmd, '/');
    const char *name = slash ? slash + 1 : server_cmd;
    /* Take first word only */
    int i = 0;
    while (name[i] && name[i] != ' ' && i < 63) {
        c->server_name[i] = name[i];
        i++;
    }
    c->server_name[i] = '\0';

    /* Build root URI */
    if (root_path)
        lsp_path_to_uri(root_path, c->root_uri, sizeof(c->root_uri));
    else {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
            lsp_path_to_uri(cwd, c->root_uri, sizeof(c->root_uri));
    }

    /* Send initialize request */
    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "processId"); jw_int(&w, (int)getpid());
    jw_key(&w, "rootUri");   jw_string(&w, c->root_uri);
    jw_key(&w, "capabilities");
    jw_obj_start(&w);
      jw_key(&w, "textDocument");
      jw_obj_start(&w);
        jw_key(&w, "completion");
        jw_obj_start(&w);
          jw_key(&w, "completionItem");
          jw_obj_start(&w);
            jw_key(&w, "snippetSupport"); jw_str_raw(&w, "false,");
          jw_obj_end(&w); jw_str_raw(&w, ",");
        jw_obj_end(&w); jw_str_raw(&w, ",");
        jw_key(&w, "hover");
        jw_obj_start(&w);
          jw_key(&w, "contentFormat");
          jw_arr_start(&w); jw_string(&w, "plaintext"); jw_arr_end(&w);
          jw_str_raw(&w, ",");
        jw_obj_end(&w); jw_str_raw(&w, ",");
        jw_key(&w, "publishDiagnostics");
        jw_obj_start(&w);
        jw_obj_end(&w); jw_str_raw(&w, ",");
        jw_key(&w, "definition");
        jw_obj_start(&w);
        jw_obj_end(&w); jw_str_raw(&w, ",");
        jw_key(&w, "synchronization");
        jw_obj_start(&w);
          jw_key(&w, "didSave"); jw_str_raw(&w, "true,");
        jw_obj_end(&w); jw_str_raw(&w, ",");
      jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_request(c, "initialize", w.buf, w.len, LSP_RESP_INITIALIZE);
    jw_free(&w);

    return c;
}

void lsp_stop(LSPClient *c) {
    if (!c) return;
    if (c->running) {
        /* Send shutdown request */
        lsp_send_request(c, "shutdown", "null", 4, LSP_RESP_NONE);
        /* Give it a moment */
        usleep(100000);
        /* Send exit notification */
        lsp_send_notification(c, "exit", "null", 4);
        usleep(50000);

        kill(-c->child_pid, SIGTERM);
    }
    if (c->stdin_fd >= 0) close(c->stdin_fd);
    if (c->stdout_fd >= 0) close(c->stdout_fd);
    int status;
    pid_t waited = waitpid(c->child_pid, &status, WNOHANG);
    if (waited == 0) {
        kill(-c->child_pid, SIGKILL);
        (void)waitpid(c->child_pid, &status, 0);
    }
    free(c->read_buf);
    free(c);
}

/* ══════════════════════════════════════════════════════════════
   Notifications
   ══════════════════════════════════════════════════════════════ */

void lsp_send_did_open(LSPClient *c, const char *uri,
                       const char *language_id, const char *text) {
    if (!c || !c->running) return;

    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "textDocument");
    jw_obj_start(&w);
      jw_key(&w, "uri");        jw_string(&w, uri);
      jw_key(&w, "languageId"); jw_string(&w, language_id);
      jw_key(&w, "version");    jw_int(&w, 1);
      jw_key(&w, "text");       jw_string(&w, text);
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_notification(c, "textDocument/didOpen", w.buf, w.len);
    jw_free(&w);
}

void lsp_send_did_change(LSPClient *c, const char *uri,
                         const char *full_text) {
    if (!c || !c->running) return;

    static int version = 2;

    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "textDocument");
    jw_obj_start(&w);
      jw_key(&w, "uri");     jw_string(&w, uri);
      jw_key(&w, "version"); jw_int(&w, version++);
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_key(&w, "contentChanges");
    jw_arr_start(&w);
    jw_obj_start(&w);
      jw_key(&w, "text"); jw_string(&w, full_text);
    jw_obj_end(&w);
    jw_arr_end(&w); jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_notification(c, "textDocument/didChange", w.buf, w.len);
    jw_free(&w);
}

void lsp_send_did_close(LSPClient *c, const char *uri) {
    if (!c || !c->running) return;

    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "textDocument");
    jw_obj_start(&w);
      jw_key(&w, "uri"); jw_string(&w, uri);
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_obj_end(&w);

    lsp_send_notification(c, "textDocument/didClose", w.buf, w.len);
    jw_free(&w);
}

/* ══════════════════════════════════════════════════════════════
   Requests
   ══════════════════════════════════════════════════════════════ */

static void build_text_document_position(JsonWriter *w, const char *uri,
                                         int line, int col) {
    jw_obj_start(w);
    jw_key(w, "textDocument");
    jw_obj_start(w);
      jw_key(w, "uri"); jw_string(w, uri);
    jw_obj_end(w); jw_str_raw(w, ",");
    jw_key(w, "position");
    jw_obj_start(w);
      jw_key(w, "line");      jw_int(w, line);
      jw_key(w, "character"); jw_int(w, col);
    jw_obj_end(w); jw_str_raw(w, ",");
    jw_obj_end(w);
}

void lsp_request_completion(LSPClient *c, const char *uri,
                            int line, int col) {
    if (!c || !c->running || !c->initialized) return;

    JsonWriter w;
    jw_init(&w);
    build_text_document_position(&w, uri, line, col);
    lsp_send_request(c, "textDocument/completion", w.buf, w.len,
                     LSP_RESP_COMPLETION);
    jw_free(&w);
}

void lsp_request_hover(LSPClient *c, const char *uri,
                       int line, int col) {
    if (!c || !c->running || !c->initialized) return;

    JsonWriter w;
    jw_init(&w);
    build_text_document_position(&w, uri, line, col);
    lsp_send_request(c, "textDocument/hover", w.buf, w.len,
                     LSP_RESP_HOVER);
    jw_free(&w);
}

void lsp_request_definition(LSPClient *c, const char *uri,
                            int line, int col) {
    if (!c || !c->running || !c->initialized) return;

    JsonWriter w;
    jw_init(&w);
    build_text_document_position(&w, uri, line, col);
    lsp_send_request(c, "textDocument/definition", w.buf, w.len,
                     LSP_RESP_DEFINITION);
    jw_free(&w);
}

void lsp_request_references(LSPClient *c, const char *uri,
                            int line, int col) {
    if (!c || !c->running || !c->initialized) return;

    JsonWriter w;
    jw_init(&w);
    jw_obj_start(&w);
    jw_key(&w, "textDocument");
    jw_obj_start(&w);
      jw_key(&w, "uri"); jw_string(&w, uri);
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_key(&w, "position");
    jw_obj_start(&w);
      jw_key(&w, "line");      jw_int(&w, line);
      jw_key(&w, "character"); jw_int(&w, col);
    jw_obj_end(&w); jw_str_raw(&w, ",");
    jw_key(&w, "context");
    jw_obj_start(&w);
      jw_key(&w, "includeDeclaration"); jw_str_raw(&w, "true");
    jw_obj_end(&w);
    jw_obj_end(&w);

    lsp_send_request(c, "textDocument/references", w.buf, w.len,
                     LSP_RESP_REFERENCES);
    jw_free(&w);
}

/* ══════════════════════════════════════════════════════════════
   Response handling
   ══════════════════════════════════════════════════════════════ */

static LSPResponseType find_pending_type(LSPClient *c, int id) {
    for (int i = 0; i < c->pending_count; i++) {
        if (c->pending[i].id == id) {
            LSPResponseType t = c->pending[i].type;
            /* Remove from pending */
            memmove(&c->pending[i], &c->pending[i + 1],
                    (c->pending_count - i - 1) * sizeof(LSPPendingRequest));
            c->pending_count--;
            return t;
        }
    }
    return LSP_RESP_NONE;
}

static void handle_completion_response(LSPClient *c, JValue *result) {
    c->completion_count = 0;
    c->completion_ready = true;

    if (!result) return;

    /* result can be an array or {items: [...]} */
    JValue *items = result;
    if (result->type == JV_OBJECT) {
        JValue *it = jv_get(result, "items");
        if (it) items = it;
    }

    if (items->type != JV_ARRAY) return;

    for (int i = 0; i < items->arr.count && c->completion_count < LSP_MAX_COMPLETIONS; i++) {
        JValue *item = items->arr.items[i];
        if (!item || item->type != JV_OBJECT) continue;

        LSPCompletionItem *ci = &c->completions[c->completion_count++];
        memset(ci, 0, sizeof(*ci));

        const char *label = jv_str(jv_get(item, "label"));
        strncpy(ci->label, label, LSP_MAX_LABEL - 1);

        JValue *insert = jv_get(item, "insertText");
        if (insert && insert->type == JV_STRING)
            strncpy(ci->insert_text, insert->s, LSP_MAX_LABEL - 1);
        else
            strncpy(ci->insert_text, label, LSP_MAX_LABEL - 1);

        JValue *detail = jv_get(item, "detail");
        if (detail && detail->type == JV_STRING)
            strncpy(ci->detail, detail->s, LSP_MAX_DETAIL - 1);

        ci->kind = jv_int(jv_get(item, "kind"));
    }
}

static void handle_hover_response(LSPClient *c, JValue *result) {
    c->hover_ready = true;
    c->hover.valid = false;

    if (!result || result->type != JV_OBJECT) return;

    JValue *contents = jv_get(result, "contents");
    if (!contents) return;

    const char *text = "";
    if (contents->type == JV_STRING) {
        text = contents->s;
    } else if (contents->type == JV_OBJECT) {
        /* MarkupContent: {kind: "...", value: "..."} */
        JValue *value = jv_get(contents, "value");
        if (value && value->type == JV_STRING)
            text = value->s;
    } else if (contents->type == JV_ARRAY && contents->arr.count > 0) {
        JValue *first = contents->arr.items[0];
        if (first->type == JV_STRING)
            text = first->s;
        else if (first->type == JV_OBJECT) {
            JValue *value = jv_get(first, "value");
            if (value && value->type == JV_STRING)
                text = value->s;
        }
    }

    if (text[0]) {
        strncpy(c->hover.contents, text, sizeof(c->hover.contents) - 1);
        c->hover.valid = true;
    }
}

static void handle_definition_response(LSPClient *c, JValue *result) {
    c->definition_ready = true;
    c->definition.valid = false;

    if (!result) return;

    /* result can be Location, Location[], or LocationLink[] */
    JValue *loc = result;
    if (result->type == JV_ARRAY && result->arr.count > 0)
        loc = result->arr.items[0];

    if (!loc || loc->type != JV_OBJECT) return;

    /* LocationLink has targetUri, Location has uri */
    JValue *uri_v = jv_get(loc, "uri");
    if (!uri_v) uri_v = jv_get(loc, "targetUri");
    if (!uri_v || uri_v->type != JV_STRING) return;

    strncpy(c->definition.uri, uri_v->s, LSP_MAX_URI - 1);

    JValue *range = jv_get(loc, "range");
    if (!range) range = jv_get(loc, "targetRange");
    if (range && range->type == JV_OBJECT) {
        JValue *start = jv_get(range, "start");
        if (start && start->type == JV_OBJECT) {
            c->definition.line = jv_int(jv_get(start, "line"));
            c->definition.col  = jv_int(jv_get(start, "character"));
        }
    }

    c->definition.valid = true;
}

static void handle_references_response(LSPClient *c, JValue *result) {
    c->references.count = 0;
    c->references.valid = false;
    c->references_ready = true;

    if (!result || result->type != JV_ARRAY) return;

    for (int i = 0; i < result->arr.count && c->references.count < LSP_MAX_REFERENCES; i++) {
        JValue *loc = result->arr.items[i];
        if (!loc || loc->type != JV_OBJECT) continue;

        JValue *uri_v = jv_get(loc, "uri");
        if (!uri_v || uri_v->type != JV_STRING) continue;

        JValue *range = jv_get(loc, "range");
        if (!range || range->type != JV_OBJECT) continue;

        JValue *start = jv_get(range, "start");
        if (!start || start->type != JV_OBJECT) continue;

        LSPReferenceLocation *ref = &c->references.locations[c->references.count++];
        strncpy(ref->uri, uri_v->s, LSP_MAX_URI - 1);
        ref->line = jv_int(jv_get(start, "line"));
        ref->col  = jv_int(jv_get(start, "character"));
    }
    c->references.valid = (c->references.count > 0);
}

static void handle_diagnostics_notification(LSPClient *c, JValue *params) {
    if (!params || params->type != JV_OBJECT) return;

    c->diagnostic_count = 0;
    c->diagnostics_ready = true;

    JValue *diags = jv_get(params, "diagnostics");
    if (!diags || diags->type != JV_ARRAY) return;

    for (int i = 0; i < diags->arr.count && c->diagnostic_count < LSP_MAX_DIAGNOSTICS; i++) {
        JValue *d = diags->arr.items[i];
        if (!d || d->type != JV_OBJECT) continue;

        LSPDiagnostic *ld = &c->diagnostics[c->diagnostic_count++];
        memset(ld, 0, sizeof(*ld));

        JValue *range = jv_get(d, "range");
        if (range && range->type == JV_OBJECT) {
            JValue *start = jv_get(range, "start");
            JValue *end   = jv_get(range, "end");
            if (start) {
                ld->start_line = jv_int(jv_get(start, "line"));
                ld->start_col  = jv_int(jv_get(start, "character"));
            }
            if (end) {
                ld->end_line = jv_int(jv_get(end, "line"));
                ld->end_col  = jv_int(jv_get(end, "character"));
            }
        }

        ld->severity = jv_int(jv_get(d, "severity"));
        if (ld->severity == 0) ld->severity = LSP_DIAG_ERROR;

        const char *msg = jv_str(jv_get(d, "message"));
        strncpy(ld->message, msg, sizeof(ld->message) - 1);
    }
}

static void handle_message(LSPClient *c, const char *json, int json_len) {
    JsonArena *arena = ja_new(json_len * 2 + 4096);
    JParser parser = { .src = json, .pos = 0, .len = json_len, .arena = arena };

    JValue *root = jp_parse(&parser);
    if (!root || root->type != JV_OBJECT) {
        ja_free(arena);
        return;
    }

    /* Check if it's a response (has "id") or notification (has "method") */
    JValue *id_v     = jv_get(root, "id");
    JValue *method_v = jv_get(root, "method");

    if (id_v && id_v->type == JV_INT) {
        /* Response to a request */
        int id = id_v->i;
        LSPResponseType rtype = find_pending_type(c, id);

        JValue *result = jv_get(root, "result");

        switch (rtype) {
            case LSP_RESP_INITIALIZE:
                c->initialized = true;
                /* Send initialized notification */
                lsp_send_notification(c, "initialized", "{}", 2);
                break;
            case LSP_RESP_COMPLETION:
                handle_completion_response(c, result);
                break;
            case LSP_RESP_HOVER:
                handle_hover_response(c, result);
                break;
            case LSP_RESP_DEFINITION:
                handle_definition_response(c, result);
                break;
            case LSP_RESP_REFERENCES:
                handle_references_response(c, result);
                break;
            default:
                break;
        }
    } else if (method_v && method_v->type == JV_STRING) {
        /* Server notification */
        const char *method = method_v->s;
        JValue *params = jv_get(root, "params");

        if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
            handle_diagnostics_notification(c, params);
        }
        /* Other notifications (window/logMessage, etc.) are silently ignored */
    }

    ja_free(arena);
}

/* ══════════════════════════════════════════════════════════════
   Poll — non-blocking read + message framing
   ══════════════════════════════════════════════════════════════ */

void lsp_poll(LSPClient *c) {
    if (!c || !c->running) return;

    /* Check if child is still alive */
    int status;
    pid_t result = waitpid(c->child_pid, &status, WNOHANG);
    if (result > 0) {
        c->running = false;
        return;
    }

    /* Non-blocking read with a hard upper bound against a misbehaving server. */
    struct pollfd pfd = { .fd = c->stdout_fd, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        if (c->read_len == c->read_cap) {
            int max_cap = LSP_MAX_FRAME_SIZE + LSP_MAX_HEADER_SIZE;
            if (c->read_cap >= max_cap) {
                c->running = false;
                kill(-c->child_pid, SIGTERM);
                return;
            }
            int ncap = c->read_cap > max_cap / 2 ? max_cap : c->read_cap * 2;
            char *new_buf = realloc(c->read_buf, (size_t)ncap);
            if (!new_buf) {
                c->running = false;
                kill(-c->child_pid, SIGTERM);
                return;
            }
            c->read_buf = new_buf;
            c->read_cap = ncap;
        }

        ssize_t n = read(c->stdout_fd, c->read_buf + c->read_len,
                         (size_t)(c->read_cap - c->read_len));
        if (n == 0) { c->running = false; return; }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        c->read_len += (int)n;
        pfd.revents = 0;
    }

    /* Try to parse complete messages from read buffer */
    while (c->read_len > 0) {
        /* Find Content-Length header */
        char *header_end = memmem(c->read_buf, c->read_len, "\r\n\r\n", 4);
        if (!header_end) {
            if (c->read_len > LSP_MAX_HEADER_SIZE) {
                c->running = false;
                kill(-c->child_pid, SIGTERM);
            }
            break;
        }

        int header_len = (int)(header_end - c->read_buf) + 4;
        if (header_len > LSP_MAX_HEADER_SIZE) {
            c->running = false;
            kill(-c->child_pid, SIGTERM);
            return;
        }

        /* Parse Content-Length */
        char *cl = memmem(c->read_buf, header_len, "Content-Length:", 15);
        if (!cl) {
            c->running = false;
            kill(-c->child_pid, SIGTERM);
            return;
        }
        char *length_start = cl + 15;
        char *header_limit = c->read_buf + header_len;
        while (length_start < header_limit && (*length_start == ' ' || *length_start == '\t'))
            length_start++;
        errno = 0;
        char *length_end = NULL;
        long parsed_length = strtol(length_start, &length_end, 10);
        if (errno || length_end == length_start || parsed_length <= 0 ||
            parsed_length > LSP_MAX_FRAME_SIZE || length_end >= header_limit ||
            (*length_end != '\r' && *length_end != ' ' && *length_end != '\t')) {
            c->running = false;
            kill(-c->child_pid, SIGTERM);
            return;
        }
        int content_length = (int)parsed_length;

        int total_msg = header_len + content_length;
        if (c->read_len < total_msg) break;  /* Wait for more data */

        /* Process the JSON body */
        handle_message(c, c->read_buf + header_len, content_length);

        /* Remove processed message */
        memmove(c->read_buf, c->read_buf + total_msg,
                c->read_len - total_msg);
        c->read_len -= total_msg;
    }
}
