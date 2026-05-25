#ifndef FORGE_SCRIPT_H
#define FORGE_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ══════════════════════════════════════════════════════════════
   ForgeScript — a tiny DSL for editor plugins

   ForgeScript is a minimal scripting language for Forge editor
   plugins. Scripts are compiled to bytecode and executed in a
   sandboxed stack VM.

   Example script:
     on save {
         run "prettier $file"
         notify "formatted!"
     }

     on keypress Ctrl+B {
         wrap selection with "**" and "**"
     }
   ══════════════════════════════════════════════════════════════ */

/* ── Token types ───────────────────────────────────────────── */

typedef enum {
    FS_TOK_EOF,
    FS_TOK_ON,           /* 'on' */
    FS_TOK_SAVE,         /* 'save' */
    FS_TOK_OPEN,         /* 'open' */
    FS_TOK_CLOSE,        /* 'close' */
    FS_TOK_KEYPRESS,     /* 'keypress' */
    FS_TOK_WIDGET,       /* 'widget' */
    FS_TOK_IN,           /* 'in' */
    FS_TOK_WIDTH,        /* 'width' */
    FS_TOK_PRIORITY,     /* 'priority' */
    FS_TOK_RENDER,       /* 'render' */
    FS_TOK_RUN,          /* 'run' */
    FS_TOK_NOTIFY,       /* 'notify' */
    FS_TOK_WRAP,         /* 'wrap' */
    FS_TOK_SELECTION,    /* 'selection' */
    FS_TOK_WITH,         /* 'with' */
    FS_TOK_AND,          /* 'and' */
    FS_TOK_FOR,          /* 'for' */
    FS_TOK_LINE,         /* 'line' */
    FS_TOK_STYLE,        /* 'style' */
    FS_TOK_TEXT,         /* 'text' */
    FS_TOK_THEME,        /* 'theme' */
    FS_TOK_ON_FOCUS,     /* 'on_focus' */
    FS_TOK_ON_KEY,       /* 'on_key' */
    FS_TOK_LBRACE,       /* '{' */
    FS_TOK_RBRACE,       /* '}' */
    FS_TOK_STRING,       /* "..." */
    FS_TOK_NUMBER,       /* 123 */
    FS_TOK_IDENT,        /* identifier */
    FS_TOK_DOT,          /* '.' */
    FS_TOK_LPAREN,       /* '(' */
    FS_TOK_RPAREN,       /* ')' */
    FS_TOK_PLUS,         /* '+' */
    FS_TOK_EQUALS,       /* '=' */
    FS_TOK_ERROR
} FSTokenType;

typedef struct {
    FSTokenType type;
    char        value[256];
    int         line;
} FSToken;

/* ── Bytecode opcodes ──────────────────────────────────────── */

typedef enum {
    FS_OP_NOP,
    FS_OP_PUSH_STR,      /* push string constant */
    FS_OP_PUSH_NUM,      /* push number constant */
    FS_OP_CALL_RUN,      /* call run(str) */
    FS_OP_CALL_NOTIFY,   /* call notify(str) */
    FS_OP_CALL_INSERT,   /* call insert(str) */
    FS_OP_CALL_WRAP,     /* call wrap(prefix, suffix) */
    FS_OP_GET_SELECTION, /* push current selection */
    FS_OP_GET_FILE,      /* push current file path */
    FS_OP_CONCAT,        /* concat top two strings */
    FS_OP_SET_VAR,       /* store to variable */
    FS_OP_GET_VAR,       /* load from variable */
    FS_OP_HALT           /* end execution */
} FSOpCode;

#define FS_MAX_BYTECODE  4096
#define FS_MAX_CONSTANTS 256
#define FS_MAX_STACK     64
#define FS_MAX_VARS      32
#define FS_MAX_HANDLERS  16
#define FS_MAX_SCRIPTS   32

typedef struct {
    FSOpCode op;
    int      arg;        /* index into constants or var table */
} FSInstruction;

/* ── Event handler ─────────────────────────────────────────── */

typedef enum {
    FS_EVENT_SAVE,
    FS_EVENT_OPEN,
    FS_EVENT_CLOSE,
    FS_EVENT_KEYPRESS
} FSEventType;

typedef struct {
    FSEventType    event;
    char           key_combo[32]; /* for keypress events: "Ctrl+B" etc */
    int            code_start;    /* index into bytecode array */
    int            code_len;      /* number of instructions */
} FSHandler;

/* ── Script state ──────────────────────────────────────────── */

typedef struct {
    /* Bytecode program */
    FSInstruction  code[FS_MAX_BYTECODE];
    int            code_len;

    /* String constant pool */
    char           constants[FS_MAX_CONSTANTS][256];
    int            const_count;

    /* Event handlers */
    FSHandler      handlers[FS_MAX_HANDLERS];
    int            handler_count;

    /* Runtime stack */
    char           stack[FS_MAX_STACK][256];
    int            sp;

    /* Variables */
    char           var_names[FS_MAX_VARS][64];
    char           var_values[FS_MAX_VARS][256];
    int            var_count;

    /* Source info */
    char           filepath[512];
    bool           loaded;
    time_t         mtime;         /* for hot-reload detection */
} FSScript;

/* ── VM instance (holds all loaded scripts) ────────────────── */

typedef struct {
    FSScript   scripts[FS_MAX_SCRIPTS];
    int        script_count;
    bool       initialized;
    char       current_filepath[512];
    char       current_selection[256];
    unsigned   executed_handlers;
    unsigned   failed_loads;
} ForgeScriptVM;

/* ── Public API ────────────────────────────────────────────── */

/* Initialize the VM */
void fs_vm_init(ForgeScriptVM *vm);

/* Free the VM and all scripts */
void fs_vm_free(ForgeScriptVM *vm);

/* Load a script from file. Returns true on success. */
bool fs_vm_load_script(ForgeScriptVM *vm, const char *path);

/* Load all .fs scripts from a directory */
int  fs_vm_load_dir(ForgeScriptVM *vm, const char *dir);

/* Check for modified scripts and hot-reload them */
int  fs_vm_hot_reload(ForgeScriptVM *vm);

/* Set runtime context exposed to scripts as file / selection */
void fs_vm_set_context(ForgeScriptVM *vm, const char *filepath,
                       const char *selection);

/* Fire an event: runs all handlers across all loaded scripts */
void fs_vm_fire_event(ForgeScriptVM *vm, FSEventType event,
                      const char *filepath);

/* Fire a keypress event with key combo string */
void fs_vm_fire_keypress(ForgeScriptVM *vm, const char *key_combo);

#endif /* FORGE_SCRIPT_H */
