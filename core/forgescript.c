#include "forgescript.h"
#include "plugin.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* Forward declaration — defined in plugin.c */
extern void forge_notify(const char *message);

/* ══════════════════════════════════════════════════════════════
   ForgeScript — Lexer, Parser, Compiler, and VM

   ~1000 lines of C implementing a tiny scripting language for
   editor plugins. Scripts define event handlers (on save, on
   keypress, etc.) that are compiled to bytecode and executed
   in a sandboxed stack VM.
   ══════════════════════════════════════════════════════════════ */

/* ── Lexer ─────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    int         pos;
    int         len;
    int         line;
} FSLexer;

static void lexer_init(FSLexer *lex, const char *src) {
    lex->src = src;
    lex->pos = 0;
    lex->len = (int)strlen(src);
    lex->line = 1;
}

static void skip_whitespace(FSLexer *lex) {
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == '\n') { lex->line++; lex->pos++; }
        else if (c == ' ' || c == '\t' || c == '\r') { lex->pos++; }
        else if (c == '/' && lex->pos + 1 < lex->len &&
                 lex->src[lex->pos + 1] == '/') {
            /* Line comment */
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n')
                lex->pos++;
        }
        else break;
    }
}

static FSToken next_token(FSLexer *lex) {
    FSToken tok = { .type = FS_TOK_EOF, .value = "", .line = lex->line };

    skip_whitespace(lex);
    if (lex->pos >= lex->len) return tok;

    char c = lex->src[lex->pos];
    tok.line = lex->line;

    /* Single-char tokens */
    switch (c) {
        case '{': tok.type = FS_TOK_LBRACE;  tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case '}': tok.type = FS_TOK_RBRACE;  tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case '(': tok.type = FS_TOK_LPAREN;  tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case ')': tok.type = FS_TOK_RPAREN;  tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case '.': tok.type = FS_TOK_DOT;     tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case '+': tok.type = FS_TOK_PLUS;    tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
        case '=': tok.type = FS_TOK_EQUALS;  tok.value[0] = c; tok.value[1] = 0; lex->pos++; return tok;
    }

    /* String literal */
    if (c == '"') {
        tok.type = FS_TOK_STRING;
        lex->pos++;  /* skip opening quote */
        int vi = 0;
        while (lex->pos < lex->len && lex->src[lex->pos] != '"' &&
               vi < (int)sizeof(tok.value) - 1) {
            if (lex->src[lex->pos] == '\\' && lex->pos + 1 < lex->len) {
                lex->pos++;
                switch (lex->src[lex->pos]) {
                    case 'n':  tok.value[vi++] = '\n'; break;
                    case 't':  tok.value[vi++] = '\t'; break;
                    case '\\': tok.value[vi++] = '\\'; break;
                    case '"':  tok.value[vi++] = '"';  break;
                    default:   tok.value[vi++] = lex->src[lex->pos]; break;
                }
            } else {
                tok.value[vi++] = lex->src[lex->pos];
            }
            lex->pos++;
        }
        tok.value[vi] = '\0';
        if (lex->pos < lex->len) lex->pos++;  /* skip closing quote */
        return tok;
    }

    /* Number */
    if (isdigit((unsigned char)c)) {
        tok.type = FS_TOK_NUMBER;
        int vi = 0;
        while (lex->pos < lex->len && isdigit((unsigned char)lex->src[lex->pos])
               && vi < (int)sizeof(tok.value) - 1) {
            tok.value[vi++] = lex->src[lex->pos++];
        }
        tok.value[vi] = '\0';
        return tok;
    }

    /* Identifier / keyword */
    if (isalpha((unsigned char)c) || c == '_') {
        int vi = 0;
        while (lex->pos < lex->len &&
               (isalnum((unsigned char)lex->src[lex->pos]) ||
                lex->src[lex->pos] == '_' || lex->src[lex->pos] == '+') &&
               vi < (int)sizeof(tok.value) - 1) {
            tok.value[vi++] = lex->src[lex->pos++];
        }
        tok.value[vi] = '\0';

        /* Keyword matching */
        if (strcmp(tok.value, "on") == 0)         tok.type = FS_TOK_ON;
        else if (strcmp(tok.value, "save") == 0)   tok.type = FS_TOK_SAVE;
        else if (strcmp(tok.value, "open") == 0)   tok.type = FS_TOK_OPEN;
        else if (strcmp(tok.value, "close") == 0)  tok.type = FS_TOK_CLOSE;
        else if (strcmp(tok.value, "keypress") == 0) tok.type = FS_TOK_KEYPRESS;
        else if (strcmp(tok.value, "widget") == 0) tok.type = FS_TOK_WIDGET;
        else if (strcmp(tok.value, "in") == 0)     tok.type = FS_TOK_IN;
        else if (strcmp(tok.value, "width") == 0)  tok.type = FS_TOK_WIDTH;
        else if (strcmp(tok.value, "priority") == 0) tok.type = FS_TOK_PRIORITY;
        else if (strcmp(tok.value, "render") == 0) tok.type = FS_TOK_RENDER;
        else if (strcmp(tok.value, "run") == 0)    tok.type = FS_TOK_RUN;
        else if (strcmp(tok.value, "notify") == 0) tok.type = FS_TOK_NOTIFY;
        else if (strcmp(tok.value, "wrap") == 0)   tok.type = FS_TOK_WRAP;
        else if (strcmp(tok.value, "selection") == 0) tok.type = FS_TOK_SELECTION;
        else if (strcmp(tok.value, "with") == 0)   tok.type = FS_TOK_WITH;
        else if (strcmp(tok.value, "and") == 0)    tok.type = FS_TOK_AND;
        else if (strcmp(tok.value, "for") == 0)    tok.type = FS_TOK_FOR;
        else if (strcmp(tok.value, "line") == 0)   tok.type = FS_TOK_LINE;
        else if (strcmp(tok.value, "style") == 0)  tok.type = FS_TOK_STYLE;
        else if (strcmp(tok.value, "text") == 0)   tok.type = FS_TOK_TEXT;
        else if (strcmp(tok.value, "theme") == 0)  tok.type = FS_TOK_THEME;
        else if (strcmp(tok.value, "on_focus") == 0) tok.type = FS_TOK_ON_FOCUS;
        else if (strcmp(tok.value, "on_key") == 0) tok.type = FS_TOK_ON_KEY;
        else tok.type = FS_TOK_IDENT;
        return tok;
    }

    /* Unknown character */
    tok.type = FS_TOK_ERROR;
    tok.value[0] = c;
    tok.value[1] = '\0';
    lex->pos++;
    return tok;
}

/* ── Compiler ──────────────────────────────────────────────── */

static int add_constant(FSScript *sc, const char *str) {
    /* Check for existing constant */
    for (int i = 0; i < sc->const_count; i++) {
        if (strcmp(sc->constants[i], str) == 0)
            return i;
    }
    if (sc->const_count >= FS_MAX_CONSTANTS) return 0;
    strncpy(sc->constants[sc->const_count], str,
            sizeof(sc->constants[0]) - 1);
    return sc->const_count++;
}

static void emit(FSScript *sc, FSOpCode op, int arg) {
    if (sc->code_len >= FS_MAX_BYTECODE) return;
    sc->code[sc->code_len].op = op;
    sc->code[sc->code_len].arg = arg;
    sc->code_len++;
}

/* Parse and compile a handler body: { stmt; stmt; ... } */
static bool compile_body(FSLexer *lex, FSScript *sc) {
    FSToken tok = next_token(lex);
    if (tok.type != FS_TOK_LBRACE) {
        fprintf(stderr, "forgescript:%d: expected '{'\n", tok.line);
        return false;
    }

    while (1) {
        tok = next_token(lex);

        if (tok.type == FS_TOK_RBRACE || tok.type == FS_TOK_EOF)
            break;

        switch (tok.type) {
            case FS_TOK_RUN: {
                /* run "command" */
                FSToken arg = next_token(lex);
                if (arg.type == FS_TOK_STRING) {
                    int ci = add_constant(sc, arg.value);
                    emit(sc, FS_OP_PUSH_STR, ci);
                    emit(sc, FS_OP_CALL_RUN, 0);
                }
                break;
            }
            case FS_TOK_NOTIFY: {
                /* notify "message" */
                FSToken arg = next_token(lex);
                if (arg.type == FS_TOK_STRING) {
                    int ci = add_constant(sc, arg.value);
                    emit(sc, FS_OP_PUSH_STR, ci);
                    emit(sc, FS_OP_CALL_NOTIFY, 0);
                }
                break;
            }
            case FS_TOK_WRAP: {
                /* wrap selection with "prefix" and "suffix" */
                FSToken sel = next_token(lex);  /* selection */
                (void)sel;
                FSToken with_tok = next_token(lex);  /* with */
                (void)with_tok;
                FSToken prefix = next_token(lex);
                FSToken and_tok = next_token(lex);  /* and */
                (void)and_tok;
                FSToken suffix = next_token(lex);

                if (prefix.type == FS_TOK_STRING && suffix.type == FS_TOK_STRING) {
                    int pi = add_constant(sc, prefix.value);
                    int si = add_constant(sc, suffix.value);
                    emit(sc, FS_OP_PUSH_STR, pi);
                    emit(sc, FS_OP_PUSH_STR, si);
                    emit(sc, FS_OP_CALL_WRAP, 0);
                }
                break;
            }
            case FS_TOK_IDENT: {
                /* variable = expression */
                char varname[64];
                strncpy(varname, tok.value, sizeof(varname) - 1);
                varname[63] = '\0';

                FSToken eq = next_token(lex);
                if (eq.type == FS_TOK_EQUALS) {
                    FSToken val = next_token(lex);
                    if (val.type == FS_TOK_STRING) {
                        int ci = add_constant(sc, val.value);
                        emit(sc, FS_OP_PUSH_STR, ci);
                        int vi = add_constant(sc, varname);
                        emit(sc, FS_OP_SET_VAR, vi);
                    }
                }
                break;
            }
            default:
                /* Skip unknown tokens */
                break;
        }
    }

    emit(sc, FS_OP_HALT, 0);
    return true;
}

/* Compile one script file into bytecode */
static bool compile_script(FSScript *sc, const char *src) {
    FSLexer lex;
    lexer_init(&lex, src);

    sc->code_len = 0;
    sc->const_count = 0;
    sc->handler_count = 0;
    sc->var_count = 0;

    while (1) {
        FSToken tok = next_token(&lex);
        if (tok.type == FS_TOK_EOF) break;

        if (tok.type == FS_TOK_ON) {
            /* on <event> { ... } */
            FSToken event = next_token(&lex);
            FSHandler *h = &sc->handlers[sc->handler_count];
            h->key_combo[0] = '\0';

            switch (event.type) {
                case FS_TOK_SAVE:
                    h->event = FS_EVENT_SAVE;
                    break;
                case FS_TOK_OPEN:
                    h->event = FS_EVENT_OPEN;
                    break;
                case FS_TOK_CLOSE:
                    h->event = FS_EVENT_CLOSE;
                    break;
                case FS_TOK_KEYPRESS: {
                    h->event = FS_EVENT_KEYPRESS;
                    /* Next token is the key combo */
                    FSToken key = next_token(&lex);
                    strncpy(h->key_combo, key.value, sizeof(h->key_combo) - 1);
                    break;
                }
                default:
                    fprintf(stderr, "forgescript:%d: unknown event '%s'\n",
                            event.line, event.value);
                    continue;
            }

            h->code_start = sc->code_len;
            if (!compile_body(&lex, sc)) return false;
            h->code_len = sc->code_len - h->code_start;
            sc->handler_count++;

            if (sc->handler_count >= FS_MAX_HANDLERS) break;
        }
        /* Skip widget/theme definitions for now */
    }

    return true;
}

/* ── VM execution ──────────────────────────────────────────── */

static void vm_exec(FSScript *sc, int start, int len) {
    sc->sp = 0;

    for (int ip = start; ip < start + len && ip < sc->code_len; ip++) {
        FSInstruction *inst = &sc->code[ip];

        switch (inst->op) {
            case FS_OP_PUSH_STR:
                if (sc->sp < FS_MAX_STACK)
                    strncpy(sc->stack[sc->sp++], sc->constants[inst->arg],
                            sizeof(sc->stack[0]) - 1);
                break;

            case FS_OP_PUSH_NUM: {
                if (sc->sp < FS_MAX_STACK) {
                    snprintf(sc->stack[sc->sp], sizeof(sc->stack[0]),
                             "%d", inst->arg);
                    sc->sp++;
                }
                break;
            }

            case FS_OP_CALL_RUN:
                if (sc->sp > 0) {
                    sc->sp--;
                    plugin_run_hook(sc->stack[sc->sp], NULL);
                }
                break;

            case FS_OP_CALL_NOTIFY:
                if (sc->sp > 0) {
                    sc->sp--;
                    forge_notify(sc->stack[sc->sp]);
                }
                break;

            case FS_OP_CALL_WRAP:
                /* Pop suffix, then prefix */
                if (sc->sp >= 2) {
                    sc->sp -= 2;
                    /* TODO: implement selection wrapping via editor API */
                }
                break;

            case FS_OP_CONCAT:
                if (sc->sp >= 2) {
                    sc->sp--;
                    char tmp[256];
                    snprintf(tmp, sizeof(tmp), "%s%s",
                             sc->stack[sc->sp - 1], sc->stack[sc->sp]);
                    strncpy(sc->stack[sc->sp - 1], tmp,
                            sizeof(sc->stack[0]) - 1);
                }
                break;

            case FS_OP_SET_VAR:
                if (sc->sp > 0 && sc->var_count < FS_MAX_VARS) {
                    sc->sp--;
                    strncpy(sc->var_names[sc->var_count],
                            sc->constants[inst->arg],
                            sizeof(sc->var_names[0]) - 1);
                    strncpy(sc->var_values[sc->var_count],
                            sc->stack[sc->sp],
                            sizeof(sc->var_values[0]) - 1);
                    sc->var_count++;
                }
                break;

            case FS_OP_GET_VAR:
                if (sc->sp < FS_MAX_STACK) {
                    const char *name = sc->constants[inst->arg];
                    for (int i = 0; i < sc->var_count; i++) {
                        if (strcmp(sc->var_names[i], name) == 0) {
                            strncpy(sc->stack[sc->sp], sc->var_values[i],
                                    sizeof(sc->stack[0]) - 1);
                            break;
                        }
                    }
                    sc->sp++;
                }
                break;

            case FS_OP_GET_FILE:
                /* TODO: push current filepath from editor state */
                if (sc->sp < FS_MAX_STACK) {
                    sc->stack[sc->sp][0] = '\0';
                    sc->sp++;
                }
                break;

            case FS_OP_GET_SELECTION:
                /* TODO: push current selection from editor state */
                if (sc->sp < FS_MAX_STACK) {
                    sc->stack[sc->sp][0] = '\0';
                    sc->sp++;
                }
                break;

            case FS_OP_HALT:
                return;

            case FS_OP_NOP:
            case FS_OP_CALL_INSERT:
                break;
        }
    }
}

/* ── Public API ────────────────────────────────────────────── */

void fs_vm_init(ForgeScriptVM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->initialized = true;
}

void fs_vm_free(ForgeScriptVM *vm) {
    vm->script_count = 0;
    vm->initialized = false;
}

bool fs_vm_load_script(ForgeScriptVM *vm, const char *path) {
    if (!vm || !vm->initialized) return false;
    if (vm->script_count >= FS_MAX_SCRIPTS) return false;

    /* Read file */
    FILE *f = fopen(path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return false;
    }

    char *src = malloc((size_t)sz + 1);
    size_t got = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[got] = '\0';

    /* Compile */
    FSScript *sc = &vm->scripts[vm->script_count];
    memset(sc, 0, sizeof(*sc));
    strncpy(sc->filepath, path, sizeof(sc->filepath) - 1);

    struct stat st;
    if (stat(path, &st) == 0)
        sc->mtime = st.st_mtime;

    if (!compile_script(sc, src)) {
        free(src);
        return false;
    }

    sc->loaded = true;
    vm->script_count++;
    free(src);

    fprintf(stderr, "forge: loaded script: %s (%d handlers)\n",
            path, sc->handler_count);
    return true;
}

int fs_vm_load_dir(ForgeScriptVM *vm, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    int loaded = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".fs") != 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

        if (fs_vm_load_script(vm, full_path))
            loaded++;
    }

    closedir(d);
    return loaded;
}

int fs_vm_hot_reload(ForgeScriptVM *vm) {
    if (!vm || !vm->initialized) return 0;

    int reloaded = 0;
    for (int i = 0; i < vm->script_count; i++) {
        FSScript *sc = &vm->scripts[i];
        if (!sc->loaded) continue;

        struct stat st;
        if (stat(sc->filepath, &st) != 0) continue;
        if (st.st_mtime == sc->mtime) continue;

        /* File changed — reload */
        char path[512];
        strncpy(path, sc->filepath, sizeof(path) - 1);
        path[511] = '\0';

        FILE *f = fopen(path, "r");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);

        char *src = malloc((size_t)sz + 1);
        size_t got = fread(src, 1, (size_t)sz, f);
        fclose(f);
        src[got] = '\0';

        memset(sc, 0, sizeof(*sc));
        strncpy(sc->filepath, path, sizeof(sc->filepath) - 1);
        sc->mtime = st.st_mtime;

        if (compile_script(sc, src)) {
            sc->loaded = true;
            reloaded++;
            fprintf(stderr, "forge: hot-reloaded script: %s\n", path);
        }
        free(src);
    }

    return reloaded;
}

void fs_vm_fire_event(ForgeScriptVM *vm, FSEventType event,
                      const char *filepath) {
    if (!vm || !vm->initialized) return;
    (void)filepath;

    for (int i = 0; i < vm->script_count; i++) {
        FSScript *sc = &vm->scripts[i];
        if (!sc->loaded) continue;

        for (int j = 0; j < sc->handler_count; j++) {
            if (sc->handlers[j].event == event) {
                vm_exec(sc, sc->handlers[j].code_start,
                        sc->handlers[j].code_len);
            }
        }
    }
}

void fs_vm_fire_keypress(ForgeScriptVM *vm, const char *key_combo) {
    if (!vm || !vm->initialized || !key_combo) return;

    for (int i = 0; i < vm->script_count; i++) {
        FSScript *sc = &vm->scripts[i];
        if (!sc->loaded) continue;

        for (int j = 0; j < sc->handler_count; j++) {
            if (sc->handlers[j].event == FS_EVENT_KEYPRESS &&
                strcmp(sc->handlers[j].key_combo, key_combo) == 0) {
                vm_exec(sc, sc->handlers[j].code_start,
                        sc->handlers[j].code_len);
            }
        }
    }
}
