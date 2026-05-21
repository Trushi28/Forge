# Forge Editor — Architecture & Design Document
> A hybrid C/Rust terminal editor with built-in Git, LSP, collab, and guild networking

---

## Table of Contents

1. [Vision & Philosophy](#1-vision--philosophy)
2. [Feature Overview](#2-feature-overview)
3. [Hybrid Language Strategy](#3-hybrid-language-strategy)
4. [System Architecture](#4-system-architecture)
5. [Codebase Layout](#5-codebase-layout)
6. [Core Editor (C)](#6-core-editor-c)
7. [UI Layout System](#7-ui-layout-system)
8. [Network Layer (Rust)](#8-network-layer-rust)
9. [LSP Client](#9-lsp-client)
10. [Git TimeLine](#10-git-timeline)
11. [Plugin System](#11-plugin-system)
12. [Guild Layer](#12-guild-layer)
13. [Configuration](#13-configuration)
14. [Build System](#14-build-system)
15. [Phased Build Plan](#15-phased-build-plan)
16. [Key Design Decisions](#16-key-design-decisions)

---

## 1. Vision & Philosophy

Forge is a terminal text editor built around one idea: **never leave the editor to talk to your team.**

Everything a developer needs — editing, version history, code intelligence, and team communication — lives inside a single binary. No browser tabs. No Slack alt-tabs. No email threads about a bug on line 47.

### Core Principles

| Principle | What it means |
|-----------|---------------|
| **Lightweight** | < 2MB RAM idle. Starts in < 5ms. Piece tree buffer (O(log n) ops). |
| **Easy interface** | Command palette, always-visible statusbar, mouse support |
| **Built-in Git** | Time-travel through commits without leaving the editor |
| **Local-first** | Guild chat and file sharing work on LAN with zero internet |
| **Safe plugins** | User plugins run in isolated processes. Core stays stable. |

---

## 2. Feature Overview

```
FORGE
├── Core Editor
│   ├── Piece tree buffer (O(log n) insert/delete/seek)
│   ├── Arena allocator (low memory, fast free)
│   ├── Slot-based UI layout (every panel is a widget; fully rearrangeable)
│   ├── Dirty-line renderer (only redraws changed lines)
│   ├── Mouse support
│   ├── Command palette  (Ctrl+P)
│   └── Multi-cursor editing
│
├── LSP Client
│   ├── Auto-detects language servers in $PATH
│   ├── Completion popup
│   ├── Diagnostics in gutter
│   ├── Hover docs
│   └── Go-to-definition / find-references
│
├── Git TimeLine
│   ├── Commit scrubber at bottom of screen
│   ├── Diff gutter (±  colored lines)
│   ├── Inline blame (hover line → who + when)
│   └── One-key file restore to any commit
│
├── Plugin System
│   ├── C plugins (.so)  — loaded via dlopen, process-isolated
│   ├── ForgeScript DSL  — tiny VM, hot-reload, safe sandbox
│   └── Shell hooks      — on_save = "prettier $FILE"
│
├── Guild Layer
│   ├── mDNS presence    — who's online on your LAN, zero config
│   ├── Guild chat       — text only, stays in editor
│   ├── Ping system      — :ping user → sends file+line context
│   ├── File sharing     — :share / :grab, direct P2P
│   └── Collab mode      — CRDT live editing, colored cursors
│
└── Configuration
    ├── ~/.config/forge/config.toml
    ├── Live reload (no restart needed)
    ├── Per-language settings
    └── 5 built-in themes
```

---

## 3. Hybrid Language Strategy

Forge uses **C for the core** and **Rust for networking** — the right tool for each job.

```
┌─────────────────────────────────────────────────────────┐
│                    FORGE PROCESS (C)                    │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │  Buffer  │  │ Renderer │  │  Input   │             │
│  │ (C)      │  │ (C)      │  │ Loop (C) │             │
│  └──────────┘  └──────────┘  └──────────┘             │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │   LSP    │  │   Git    │  │  Config  │             │
│  │ Client   │  │ TimeLine │  │  (C)     │             │
│  │ (C)      │  │ (C)      │  │          │             │
│  └──────────┘  └──────────┘  └──────────┘             │
│                                                         │
│         Unix socket / shared memory                     │
└───────────────────┬─────────────────────────────────────┘
                    │
┌───────────────────▼─────────────────────────────────────┐
│              FORGE-NET PROCESS (Rust)                   │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │  CRDT    │  │  Guild   │  │   mDNS   │             │
│  │  Engine  │  │  Chat    │  │ Discovery│             │
│  │ (Rust)   │  │ (Rust)   │  │ (Rust)   │             │
│  └──────────┘  └──────────┘  └──────────┘             │
│                                                         │
│  ┌──────────┐  ┌──────────┐                            │
│  │   P2P    │  │   File   │                            │
│  │ Collab   │  │ Transfer │                            │
│  │ (Rust)   │  │ (Rust)   │                            │
│  └──────────┘  └──────────┘                            │
└─────────────────────────────────────────────────────────┘
```

### Why C for the core?

- You control every allocation — arena allocator, zero surprise GC pauses
- Terminal I/O via `termios` is straightforward in C
- Compiles in < 2 seconds, tight feedback loop
- Binary stays tiny (target: < 500KB for core)
- No runtime overhead — starts in < 5ms

### Why Rust for networking?

- CRDT requires concurrent shared state — Rust's ownership model makes data races a compile error
- `tokio` async runtime handles hundreds of guild connections cleanly
- `diamond-types` crate gives battle-tested CRDT (same tech powering Figma collab)
- `mdns-sd` crate handles zero-config LAN discovery in ~20 lines
- Memory safety matters most where untrusted data comes in (network packets)

### Boundary between them

The two processes communicate via a **Unix domain socket** on Linux/macOS (named pipe on Windows):

```
C editor  ──── socket ────  Rust net layer
sends:                      sends back:
  COLLAB_START                PEER_JOINED
  SEND_CHAT_MSG               CHAT_MSG_RECEIVED
  SHARE_FILE                  FILE_RECEIVED
  CRDT_OP                     CRDT_OP_REMOTE
```

This means: if the network layer crashes, the editor keeps running. If the editor crashes, the network layer cleans up gracefully.

---

## 4. System Architecture

### Full system view

```
User keypress
     │
     ▼
┌─────────────────────────────────────────────────┐
│  INPUT LOOP  (main.c)                           │
│  reads termios raw input                        │
│  dispatches to: editor / command palette / guild│
└────────────┬────────────────────────────────────┘
             │
    ┌────────▼────────┐        ┌─────────────────┐
    │  EDITOR CORE    │        │   LSP CLIENT    │
    │  buffer.c       │◄──────►│   lsp.c         │
    │  (piece tree)   │        │   (JSON-RPC)    │
    └────────┬────────┘        └────────┬────────┘
             │                          │ stdio pipe
    ┌────────▼────────┐        ┌────────▼────────┐
    │   RENDERER      │        │  clangd/pyright │
    │   render.c      │        │  (3rd party)    │
    │  (dirty lines)  │        └─────────────────┘
    └────────┬────────┘
             │
    ┌────────▼────────┐        ┌─────────────────┐
    │   GIT LAYER     │        │  PLUGIN HOST    │
    │   git.c         │        │  (child proc)   │
    │  (libgit2)      │        │  plugins/*.so   │
    └────────┬────────┘        └─────────────────┘
             │
    ┌────────▼────────┐
    │  FORGE-NET      │  ◄── separate Rust process
    │  (guild/collab) │
    └─────────────────┘
```

---

## 5. Codebase Layout

```
forge/
│
├── core/                        # C — the editor
│   ├── main.c                   # entry point, input loop
│   ├── buffer.c / buffer.h      # piece table implementation
│   ├── render.c / render.h      # terminal rendering
│   ├── ui.c / ui.h              # slot registry + widget lifecycle
│   ├── input.c / input.h        # keybind dispatch
│   ├── lsp.c / lsp.h            # LSP client (JSON-RPC over stdio)
│   ├── git.c / git.h            # git TimeLine (libgit2)
│   ├── config.c / config.h      # TOML parser + live reload
│   ├── theme.c / theme.h        # color themes
│   ├── plugin.c / plugin.h      # plugin host manager
│   ├── palette.c / palette.h    # command palette (Ctrl+P)
│   ├── arena.c / arena.h        # arena allocator
│   └── ipc.c / ipc.h            # socket bridge to forge-net
│
├── net/                         # Rust — networking
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs              # tokio entry, socket server
│       ├── crdt.rs              # CRDT engine (diamond-types)
│       ├── guild.rs             # guild state, chat, presence
│       ├── discovery.rs         # mDNS peer discovery
│       ├── collab.rs            # collab session management
│       ├── transfer.rs          # P2P file transfer
│       └── ipc.rs               # message protocol (C ↔ Rust)
│
├── plugins/                     # built-in plugins (C)
│   ├── autopairs.c
│   ├── indent.c
│   └── statusbar.c
│
├── scripts/                     # ForgeScript DSL
│   └── user/                    # user plugin scripts
│       └── example.fs
│
├── themes/                      # built-in themes
│   ├── dark.toml
│   ├── light.toml
│   ├── gruvbox.toml
│   ├── catppuccin.toml
│   └── solarized.toml
│
├── docs/                        # documentation
│   └── ARCHITECTURE.md          # this file
│
├── Makefile                     # top-level build (calls both)
├── build.sh                     # one-command build script
└── README.md
```

---

## 6. Core Editor (C)

### 6.1 Buffer — Piece Tree

The piece tree is the heart of the editor. It combines the non-destructive property of a piece table (original content is never moved) with a **Red-Black tree** index that makes every positional operation O(log n) instead of O(n).

A pure linked-list piece table requires walking the chain from the head to find any position — fine for a few dozen pieces, but after thousands of edits a heavily fragmented file degrades to linear scan. The piece tree solves this by maintaining cumulative length in every node's subtree metadata.

```
Original file content:  "Hello World"
                         stored once, never modified

After inserting "Beautiful " at pos 6:

Piece tree (RB-tree, each node stores subtree_char_len):

              [B] add_buf[0..9] "Beautiful "
             /   subtree_len=21
            /
    [R] orig[0..5] "Hello "          [B] orig[6..10] "World"
        subtree_len=6                    subtree_len=5

Position lookup: binary-search on subtree_len  →  O(log n)
Logical text:   "Hello Beautiful World"
```

Each node also caches **subtree line count**, enabling O(log n) line-number → byte-offset translation without a separate scan.

```c
// buffer.h

typedef enum { RB_RED, RB_BLACK } RBColor;

typedef struct PieceNode {
    // --- piece data ---
    bool        is_original;    // original buffer or add buffer?
    size_t      buf_start;      // byte offset into the source buffer
    size_t      buf_len;        // length of this piece in bytes

    // --- subtree metadata (maintained on every rotation/insert) ---
    size_t      subtree_len;    // total chars in this subtree (left + self + right)
    size_t      subtree_lines;  // total newlines in this subtree

    // --- RB-tree links ---
    RBColor     color;
    struct PieceNode *left, *right, *parent;
} PieceNode;

typedef struct {
    char        *original;          // original file content (read-only)
    size_t       original_len;
    char        *add_buf;           // append-only add buffer
    size_t       add_len;
    size_t       add_cap;

    PieceNode   *root;              // RB-tree root
    PieceNode   *nil;               // sentinel nil node (BLACK, all fields 0)
    size_t       piece_count;       // total live nodes

    // --- line index cache ---
    size_t      *line_starts;       // line_starts[i] = char offset of line i
    size_t       line_count;        // number of lines (entries in line_starts)
    bool         line_cache_dirty;  // set true after any insert/delete
} Buffer;

// --- Public API ---
Buffer* buffer_open(const char *path);
void    buffer_close(Buffer *b);

// O(log n)  — tree descent using subtree_len
void    buffer_insert(Buffer *b, size_t char_pos, const char *text, size_t len);
void    buffer_delete(Buffer *b, size_t char_pos, size_t len);
char*   buffer_slice(Buffer *b, size_t start, size_t end);   // arena-alloc'd string

// O(log n)  — descent using subtree_lines
size_t  buffer_line_to_offset(Buffer *b, size_t line);
size_t  buffer_offset_to_line(Buffer *b, size_t char_pos);

void    buffer_save(Buffer *b, const char *path);
```

**Complexity summary vs. the old linked-list piece table:**

| Operation | Linked-list piece table | Piece tree |
|-----------|------------------------|------------|
| Insert at position | O(n pieces) seek + O(1) splice | O(log n) |
| Delete range | O(n pieces) seek | O(log n) |
| Slice / render range | O(n pieces) walk | O(log n) seek + O(k) read |
| Line N → byte offset | O(n pieces) full scan | O(log n) via subtree_lines |
| Byte offset → line N | O(n pieces) full scan | O(log n) via subtree_lines |

`n` is the number of pieces (can reach tens of thousands in a long session); `k` is the number of characters actually read. The tree keeps `n` bounded naturally — consecutive edits to the same piece are merged in the node before the tree is rebalanced.

**Node metadata maintenance:** on every RB-tree rotation or insertion the fixup pass re-computes `subtree_len` and `subtree_lines` bottom-up in O(log n). The line cache (`line_starts[]`) is rebuilt lazily on the next render frame if `line_cache_dirty` is set — one O(n chars) pass amortised across many keystrokes.

### 6.2 Arena Allocator

One slab per edit session. Free the whole thing at once — no GC, no fragmentation:

```c
// arena.h
typedef struct {
    uint8_t *base;
    size_t   offset;
    size_t   capacity;
} Arena;

Arena* arena_new(size_t capacity);   // mmap a slab
void*  arena_alloc(Arena *a, size_t size);
void   arena_reset(Arena *a);        // O(1) free everything
void   arena_free(Arena *a);         // unmap the slab
```

Per-frame arena: reset every render frame. Per-session arena: reset on file close. This pattern eliminates nearly all malloc/free calls in hot paths.

### 6.3 Renderer — Dirty Line Tracking

Only re-render lines that actually changed. The renderer is slot-aware: it iterates the UI slot registry on each frame and composites each slot's widgets into the terminal grid before the dirty-line pass decides what to flush.

```c
typedef struct {
    char    **lines;          // array of rendered line strings
    bool     *dirty;          // dirty[i] = true if line i needs redraw
    int       width, height;  // terminal dimensions
    int       scroll_offset;  // top visible line
    UILayout *layout;         // slot geometry, updated on SIGWINCH
} RenderState;

void render_mark_dirty(RenderState *r, int line);
void render_frame(RenderState *r, Buffer *b, UIRegistry *ui);
// render_frame now calls ui_render_slot() for each slot before
// writing dirty lines — widgets control their own regions
```

ANSI escape sequences are written in one `write()` call per dirty line — minimizes terminal flicker. When a widget marks itself dirty, only the terminal rows it occupies are redrawn.

### 6.4 Input Loop

```c
// main loop skeleton
while (running) {
    int key = read_key();           // blocking read, raw termios

    // route to focused widget first; fall back to editor
    if (!ui_dispatch_key(&ui_registry, key)) {
        if (in_command_palette) {
            palette_handle(key);
        } else {
            editor_handle(key);     // normal editing
        }
    }

    render_frame(&render, &buffer, &ui_registry);  // slot-aware
    ipc_flush(&net_bridge);                        // queued net messages
}
```

---

## 7. UI Layout System

Forge's UI is built from **slots and widgets** — no hardcoded panel positions. Every visible element, including built-in ones like the statusbar, gutter, and git timeline, is a widget registered into a named slot. The renderer only knows about slots.

This lets users rearrange, hide, or extend the entire UI from `config.toml` or a ForgeScript plugin — something no other terminal editor offers out of the box.

### 7.1 Layout Model

```
┌─────────────────────────────────────────────┐
│  [slot: topbar]          (hidden by default) │
├────────┬────────────────────────┬────────────┤
│        │                        │            │
│[slot:  │   [slot: content]      │ [slot:     │
│ gutter]│   (the editor itself)  │  right_    │
│        │                        │  panel]    │
│        │                        │ (hidden)   │
├────────┴────────────────────────┴────────────┤
│  [slot: statusbar]                           │
├─────────────────────────────────────────────┤
│  [slot: bottombar]   (git timeline, etc.)    │
└─────────────────────────────────────────────┘
```

Slots have fixed roles in the layout grid (top, left, right, bottom, center). Within a slot, **widgets** are an ordered list — each claims a region of the slot's area, rendered in priority order.

### 7.2 Core Data Structures

```c
// ui.h

#define UI_MAX_WIDGETS 32

typedef struct {
    char   name[64];           // e.g. "git_diff", "cursor_pos"
    int    priority;           // lower = rendered first (leftmost / topmost)
    int    min_width;          // 0 = flexible
    int    fixed_height;       // 0 = fills slot height
    bool   visible;

    // callbacks — all optional, set NULL to skip
    void (*render)(struct Widget *self, RenderState *r, Buffer *b);
    void (*on_key)(struct Widget *self, int key);
    void (*on_resize)(struct Widget *self, int w, int h);
    void (*on_focus)(struct Widget *self);
    void (*on_blur)(struct Widget *self);

    void  *userdata;           // plugin-owned state
} Widget;

typedef enum {
    SLOT_TOPBAR,
    SLOT_GUTTER,
    SLOT_CONTENT,              // special: always the editor viewport
    SLOT_RIGHT_PANEL,
    SLOT_STATUSBAR,
    SLOT_BOTTOMBAR,
    SLOT_COUNT
} SlotId;

typedef struct {
    int     row, col;          // top-left terminal cell
    int     width, height;     // dimensions in cells
    bool    visible;
    Widget *widgets[UI_MAX_WIDGETS];
    int     widget_count;
} Slot;

typedef struct {
    Slot    slots[SLOT_COUNT];
    Widget *focused;           // receives key events
    int     term_w, term_h;
} UIRegistry;

// --- API ---
void    ui_init(UIRegistry *ui, int term_w, int term_h);
void    ui_register_widget(UIRegistry *ui, SlotId slot, Widget *w);
void    ui_unregister_widget(UIRegistry *ui, const char *name);
void    ui_move_widget(UIRegistry *ui, const char *name, SlotId new_slot);
void    ui_set_slot_visible(UIRegistry *ui, SlotId slot, bool visible);
void    ui_render_slot(UIRegistry *ui, SlotId slot, RenderState *r, Buffer *b);
bool    ui_dispatch_key(UIRegistry *ui, int key);
void    ui_on_resize(UIRegistry *ui, int new_w, int new_h);
```

### 7.3 Built-in Widgets

All built-in UI components are just widgets registered at startup — they get no special treatment from the renderer:

| Widget name | Default slot | Description |
|---|---|---|
| `line_numbers` | `SLOT_GUTTER` | Line number column |
| `git_diff_gutter` | `SLOT_GUTTER` | +/- diff markers |
| `diagnostics_gutter` | `SLOT_GUTTER` | LSP error/warning icons |
| `mode_indicator` | `SLOT_STATUSBAR` | NORMAL / INSERT / VISUAL |
| `filename` | `SLOT_STATUSBAR` | Current file + modified flag |
| `lsp_status` | `SLOT_STATUSBAR` | Language server name + status |
| `cursor_pos` | `SLOT_STATUSBAR` | Line:col |
| `git_branch` | `SLOT_STATUSBAR` | Current branch name |
| `git_timeline` | `SLOT_BOTTOMBAR` | Commit scrubber |
| `diagnostics_bar` | `SLOT_BOTTOMBAR` | LSP error list |
| `guild_panel` | `SLOT_RIGHT_PANEL` | Guild chat + presence (Ctrl+G) |

Any of these can be moved, hidden, or reordered from config without touching C code.

### 7.4 Config-Driven Layout

```toml
# ~/.config/forge/config.toml

[ui.slots.topbar]
visible = false

[ui.slots.gutter]
widgets = ["line_numbers", "git_diff_gutter", "diagnostics_gutter"]

[ui.slots.statusbar]
widgets = ["mode_indicator", "filename", "git_branch", "lsp_status", "cursor_pos"]
height  = 1

[ui.slots.bottombar]
widgets = ["git_timeline"]
height  = 3
visible = true

[ui.slots.right_panel]
visible = false
width   = 40
```

Reordering entries in `widgets` reorders rendering. Setting `visible = false` removes the slot entirely — the content area expands to fill the gap. Changes are hot-reloaded; no restart needed.

### 7.5 ForgeScript Widget API

Plugins can register widgets into any slot with full render control:

```
// word_count.fs — a custom statusbar widget

widget "word_count" in statusbar priority 90 {
    render {
        text = str(buffer.word_count()) + "w " + str(buffer.char_count()) + "c"
        style = theme.subtle          // inherits theme colors
    }
}
```

```
// todo_panel.fs — a right-panel widget

widget "todo_panel" in right_panel width 36 {
    on_focus {
        list = buffer.grep("TODO|FIXME|HACK")
    }
    render {
        for item in list {
            line item.lnum + "  " + item.text style theme.warning
        }
    }
    on_key Ctrl+Enter {
        buffer.goto(selected_item.lnum)
        panel.blur()
    }
}
```

C plugins use the same `forge_api.h` surface:

```c
// forge_api.h additions for UI
Widget* forge_widget_new(const char *name, SlotId slot, int priority);
void    forge_widget_set_render_cb(Widget *w, void (*cb)(Widget*, RenderState*, Buffer*));
void    forge_widget_set_key_cb(Widget *w, void (*cb)(Widget*, int key));
void    forge_widget_mark_dirty(Widget *w);
void    forge_widget_register(Widget *w);   // adds to the live registry
```

### 7.6 Complexity Trade-offs

| What gets simpler | What gets more complex |
|---|---|
| Renderer has no hardcoded regions — just iterates slots | Slot registry + widget lifecycle (init, render, resize, destroy) ~400–600 lines |
| Adding a new panel is registering one widget | Layout engine must handle slot sizing and overflow gracefully |
| Built-in components are not special — same API as plugins | Widget API becomes part of the stable plugin contract |
| Themes apply uniformly to all widgets | More indirection when tracing a rendering bug |

The layout engine (`ui.c`) is roughly 500 lines of C — a slot table, an ordered widget list per slot, geometry recalculation on `SIGWINCH`, and a render pass that calls each widget's callback. The ForgeScript widget API adds ~200 lines to the VM bindings. Total overhead is modest compared to the flexibility gained.

---

## 8. Network Layer (Rust)

### 7.1 IPC Protocol

Messages between C and Rust are length-prefixed JSON over a Unix socket:

```
[4 bytes: message length][JSON payload]
```

```json
// C → Rust examples
{ "type": "COLLAB_START", "peer": "anya", "file": "main.c" }
{ "type": "CRDT_OP", "op": { "ins": { "pos": 42, "text": "hello" } } }
{ "type": "CHAT_SEND", "guild": "C Wizards", "text": "found it!" }
{ "type": "FILE_SHARE", "name": "bugfix.c", "data_b64": "..." }

// Rust → C examples
{ "type": "PEER_ONLINE", "handle": "anya", "file": "parser.c" }
{ "type": "CRDT_REMOTE", "op": { "ins": { "pos": 42, "text": "hello" } } }
{ "type": "CHAT_RECV", "from": "anya", "text": "use-after-free line 42" }
{ "type": "PING_RECV", "from": "rex", "file": "main.c", "line": 47 }
```

### 7.2 CRDT Engine

Uses `diamond-types` — the same CRDT powering production collaborative editors:

```rust
// crdt.rs
use diamond_types::list::ListCRDT;

pub struct CollabDoc {
    inner: ListCRDT,
    agent: AgentId,
}

impl CollabDoc {
    pub fn insert(&mut self, pos: usize, text: &str) -> Op {
        self.inner.insert(self.agent, pos, text)
        // returns an Op that can be sent to peers
        // peers apply it → guaranteed convergence
    }

    pub fn apply_remote(&mut self, op: Op) {
        self.inner.apply_op(op);
        // automatic conflict resolution
        // both users always end up with identical text
    }
}
```

### 7.3 Guild Discovery (mDNS)

Zero-config peer discovery on LAN:

```rust
// discovery.rs
use mdns_sd::{ServiceDaemon, ServiceInfo};

pub async fn announce(handle: &str, guild: &str, port: u16) {
    let mdns = ServiceDaemon::new().unwrap();
    let service = ServiceInfo::new(
        "_forge._tcp.local.",
        handle,
        &format!("{}.local.", handle),
        "",
        port,
        &[("guild", guild), ("handle", handle)],
    ).unwrap();
    mdns.register(service).unwrap();
    // now discoverable by everyone on the same LAN
}

pub async fn discover(guild: &str) -> impl Stream<Item = Peer> {
    // returns a stream of peers joining/leaving
    // C layer gets notified via IPC for each event
}
```

### 7.4 P2P File Transfer

Direct TCP — no relay, no cloud:

```rust
// transfer.rs
pub async fn send_file(peer_addr: SocketAddr, path: &Path) -> Result<()> {
    let mut stream = TcpStream::connect(peer_addr).await?;
    let data = tokio::fs::read(path).await?;
    let msg = FileMsg { name: path.file_name(), data };
    stream.write_all(&serialize(msg)).await?;
    Ok(())
    // receiver gets it directly in their editor buffer pool
}
```

---

## 9. LSP Client

Forge speaks the Language Server Protocol natively. You bring the server.

### Supported servers (auto-detected from $PATH)

| Language | Server | Install |
|----------|--------|---------|
| C / C++  | `clangd` | `apt install clangd` |
| Python   | `pyright` | `pip install pyright` |
| Rust     | `rust-analyzer` | `rustup component add rust-analyzer` |
| JS / TS  | `typescript-language-server` | `npm i -g typescript-language-server` |
| Go       | `gopls` | `go install golang.org/x/tools/gopls@latest` |
| Lua      | `lua-language-server` | from GitHub releases |

Any LSP-compliant server works automatically.

### What the editor handles

```
textDocument/completion      → dropdown popup at cursor
textDocument/publishDiagnostics → red/yellow lines in gutter
textDocument/hover           → doc popup on Ctrl+K
textDocument/definition      → jump with gd
textDocument/references      → list panel with gr
textDocument/formatting      → format file with :fmt
workspace/symbol             → fuzzy symbol search Ctrl+P @
```

### LSP lifecycle

```c
// lsp.c — simplified flow
LSPServer* lsp_start(const char *server_binary) {
    // fork + exec the server
    // connect via stdin/stdout pipes
    // send initialize request
}

void lsp_on_buffer_change(LSPServer *s, Buffer *b, Change *c) {
    // send textDocument/didChange notification
    // server replies asynchronously with diagnostics
}

Completion* lsp_complete(LSPServer *s, int line, int col) {
    // send textDocument/completion request
    // render popup with results
}
```

---

## 10. Git TimeLine

The time-travel feature — browse your entire commit history without leaving the editor.

### UI Layout

```
┌─ main.c ────────────────────────── [main] ──── modified ─┐
│+ 1  #include <stdio.h>                                   │
│  2                                                       │
│- 3  int mian() {        ← diff gutter: red = deleted     │
│+ 3  int main() {        ← diff gutter: green = added     │
│  4      printf("hello\n");  [anya • 2h ago]  ← blame    │
│  5  }                                                    │
│                                                          │
│ ◄─── Mar 1 ──── Mar 5 ──── [Mar 8] ──── Mar 10 ────► │
│      fix typo   add tests  current   add feature         │
└──────────────────────────────────────────────────────────┘
          ↑ TimeLine scrubber (Ctrl+T to toggle)
```

### Keybinds

| Key | Action |
|-----|--------|
| `Ctrl+T` | Toggle TimeLine panel |
| `←` / `→` | Step through commits |
| `Enter` | Load file at this commit (read-only view) |
| `Ctrl+D` | Diff current vs selected commit |
| `Escape` | Return to present |
| `:blame` | Toggle inline blame on current file |

### Implementation

Uses `libgit2` — a pure C library, no shelling out to `git`:

```c
// git.c
git_repository *repo;
git_repository_open(&repo, ".");

// get commit history for current file
git_revwalk *walker;
git_revwalk_new(&walker, repo);
git_revwalk_push_head(walker);

git_oid oid;
while (git_revwalk_next(&oid, walker) == 0) {
    git_commit *commit;
    git_commit_lookup(&commit, repo, &oid);
    // add to timeline strip
}

// get file at a specific commit
git_blob *blob;
git_blob_lookup(&blob, repo, &file_oid);
// display in buffer (read-only)
```

---

## 11. Plugin System

Four tiers — each with a different tradeoff between power and safety.

### Tier 0 — Native Widgets (UI slot system)

The lightest extension point. Register a widget into any UI slot directly from `config.toml` (for layout changes) or ForgeScript (for dynamic content). No compilation, no process boundary, no IPC — the widget callback runs inline in the render loop. See §7 for the full widget API.

```toml
# Simple example: move git_branch to the topbar — zero code needed
[ui.slots.topbar]
visible = true
widgets = ["git_branch"]

[ui.slots.statusbar]
widgets = ["mode_indicator", "filename", "lsp_status", "cursor_pos"]
```

### Tier 1 — C Plugins (.so, process-isolated)

For powerful plugins. Runs in a child process — a crash cannot affect the editor:

```
forge (main)
└── forge-plugin-host (child process)
    ├── autopairs.so
    ├── linting.so
    └── user_plugin.so

Crash in plugin → host dies → editor spawns new host → seamless
```

```c
// forge_api.h — what C plugins can call
void forge_on_save(void (*cb)(const char *filepath));
void forge_on_keypress(void (*cb)(int key, ForgeBuffer *buf));
void forge_insert(const char *text);
void forge_notify(const char *message);
void forge_run(const char *cmd);
```

```c
// example: user_plugin.c
#include "forge_api.h"

void handle_save(const char *path) {
    forge_run("prettier --write $FILE");
    forge_notify("formatted!");
}

FORGE_PLUGIN_INIT {
    forge_on_save(handle_save);
}
```

Build: `forge plugin build myplugin.c` → outputs `myplugin.so`

### Tier 2 — ForgeScript DSL (VM sandboxed, hot-reload)

For quick automations. No compilation. Edit and save — changes apply instantly:

```
// myplugin.fs — ForgeScript

on save {
    run "prettier $file"
    notify "formatted!"
}

on keypress Ctrl+B {
    wrap selection with "**" and "**"
}

theme {
    background = #1e1e2e
    foreground = #cdd6f4
    accent     = #89b4fa
}
```

ForgeScript VM is ~1000 lines of C: lexer → parser → bytecode compiler → tiny stack VM. Fully sandboxed — scripts cannot access memory outside their API surface.

### Tier 3 — Shell Hooks (escape hatch)

One-liner automations via config.toml:

```toml
[hooks]
on_save    = "prettier --write $FILE"
on_open    = "echo opened $FILE >> ~/.forge/log"
on_git_commit = "cargo test"
```

Any shell command. Zero plugin code needed.

---

## 12. Guild Layer

The social layer — team communication without leaving the editor.

### Setup (30 seconds)

```
$ forge --setup

  Handle   > shadowcoder
  Guild    > C Wizards
  Color    > cyan

  Scanning LAN...
  Found: anya (C Wizards) • rex (C Wizards)

  ✓ You're in. 2 guildmates online.
```

Profile saved to `~/.config/forge/profile.toml`. Done.

### Guild Panel (Ctrl+G)

```
┌─ Guild: C Wizards ───────────────────────────── 3 online ─┐
│                                                           │
│  ONLINE                                                   │
│  🟢 shadowcoder   main.c       line 42   [ping] [collab] │
│  🟢 anya          parser.c     line 8    [ping] [collab] │
│  🟡 rex           away         20min                      │
│                                                           │
│  CHAT ──────────────────────────────────────────────────  │
│  shadowcoder: this segfault makes no sense                │
│  anya: classic use-after-free, free() called twice        │
│  shadowcoder: oh god you're right                         │
│  > _                                                      │
│                                                           │
│  SHARED FILES ──────────────────────────────────────────  │
│  📄 bugfix.c       anya       2 min ago      [grab]       │
│  📄 Makefile       rex        1 hr ago       [grab]       │
└───────────────────────────────────────────────────────────┘
```

### Commands

| Command | Description |
|---------|-------------|
| `:ping <user>` | Send file + line context to a guildmate |
| `:collab <user>` | Start live co-editing session |
| `:share` | Share current file to guild pool |
| `:share <file>` | Share a specific file |
| `:grab <user> <file>` | Pull a shared file directly into editor |
| `:drop <file>` | Remove file from shared pool |
| `/dm <user> <msg>` | Direct message in chat |

### Ping Flow

```
shadowcoder on main.c line 47:
  types: :ping anya

anya's editor shows non-blocking notification:
  ┌──────────────────────────────────────────────┐
  │ 📍 shadowcoder pinged you                   │
  │    main.c  line 47                          │
  │    [jump]  [chat]  [collab]  [dismiss]      │
  └──────────────────────────────────────────────┘

anya hits [jump] → her editor opens main.c at line 47
anya sees exactly what shadowcoder sees
```

The editor automatically attaches: file, line number, 5 lines of context, current git branch, and any LSP diagnostics at that line. Zero manual copy-pasting.

### Collab Flow

```
:collab anya

anya sees accept/decline prompt
anya accepts →

┌─ main.c ─────────────────────────────────[collab: anya]─┐
│  1  #include <stdio.h>                                  │
│  2  ▋                    ← shadowcoder cursor (white)   │
│  3  int main() {         ← [anya] cursor (cyan)         │
│  4      printf("hello"); ← both can type simultaneously │
└─────────────────────────────────────────────────────────┘
```

CRDT ensures both users always converge to the same text — no conflicts, no overwriting each other.

### Network topology

```
Same LAN:
  A ←──── direct TCP ────► B
  Zero internet. < 1ms latency.

Different networks:
  A ←── TCP hole-punch ──► B
  If NAT blocks hole-punch:
  A ←─── tiny relay ─────► B   (relay only brokers connection,
                                 never sees file content)
```

---

## 13. Configuration

All configuration lives in `~/.config/forge/config.toml`. Changes are hot-reloaded — no restart needed.

```toml
[editor]
tab_width    = 4
use_spaces   = true
line_numbers = true
mouse        = true
scrolloff    = 8        # lines to keep above/below cursor
theme        = "catppuccin"

[ui.slots.topbar]
visible  = false

[ui.slots.gutter]
widgets  = ["line_numbers", "git_diff_gutter", "diagnostics_gutter"]

[ui.slots.statusbar]
widgets  = ["mode_indicator", "filename", "git_branch", "lsp_status", "cursor_pos"]
height   = 1

[ui.slots.bottombar]
widgets  = ["git_timeline"]
height   = 3

[ui.slots.right_panel]
visible  = false
width    = 40

[keybinds]
"Ctrl+P"     = "command_palette"
"Ctrl+G"     = "guild_panel"
"Ctrl+T"     = "git_timeline"
"Ctrl+/"     = "toggle_comment"
"leader+f"   = "fuzzy_files"

[lsp]
auto_detect  = true     # scan $PATH for known servers
show_hints   = true
completion   = true
diagnostics  = true

[git]
show_gutter  = true     # +/- diff markers
show_blame   = false    # inline blame (toggle with :blame)
timeline     = true

[guild]
handle       = "shadowcoder"
name         = "C Wizards"
color        = "cyan"
announce_lan = true

[hooks]
on_save      = ""       # shell command, $FILE available
on_open      = ""
on_close     = ""

[lang.c]
indent       = 4
formatter    = "clang-format -i $FILE"
lsp          = "clangd"

[lang.python]
indent       = 4
formatter    = "black $FILE"
lsp          = "pyright"

[lang.rust]
indent       = 4
formatter    = "rustfmt $FILE"
lsp          = "rust-analyzer"

[plugins]
paths = [
    "~/.config/forge/plugins/",
]

[theme.custom]
# override any theme color
background   = "#1e1e2e"
foreground   = "#cdd6f4"
accent       = "#89b4fa"
error        = "#f38ba8"
warning      = "#fab387"
```

---

## 14. Build System

### One-command build

```bash
./build.sh          # builds everything
./build.sh core     # C core only
./build.sh net      # Rust net layer only
./build.sh release  # optimized release build
```

### Makefile (core)

```makefile
CC      = clang
CFLAGS  = -O2 -Wall -Wextra -std=c11
LIBS    = -lgit2 -ldl -lm

SRCS    = core/main.c core/buffer.c core/render.c core/ui.c core/input.c \
          core/lsp.c core/git.c core/config.c core/theme.c \
          core/plugin.c core/palette.c core/arena.c core/ipc.c

forge-core: $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o forge-core $(LIBS)

clean:
	rm -f forge-core forge-net
```

### Cargo.toml (net)

```toml
[package]
name    = "forge-net"
version = "0.1.0"
edition = "2021"

[dependencies]
tokio          = { version = "1", features = ["full"] }
diamond-types  = "0.6"
mdns-sd        = "0.9"
serde          = { version = "1", features = ["derive"] }
serde_json     = "1"
```

### Build times (target)

| Component | Debug | Release |
|-----------|-------|---------|
| C core | < 2s | < 5s |
| Rust net | ~15s | ~45s |
| Total (cached) | < 3s | < 10s |

### Dependencies

| Library | Used for | Language |
|---------|----------|----------|
| `libgit2` | Git operations | C |
| `libc` / `termios` | Terminal I/O | C |
| `dl` (dlopen) | Plugin loading | C |
| `tokio` | Async runtime | Rust |
| `diamond-types` | CRDT engine | Rust |
| `mdns-sd` | LAN discovery | Rust |
| `serde_json` | IPC protocol | Rust |

**Zero other dependencies.** Everything else is written from scratch.

---

## 15. Phased Build Plan

### Phase 1 — Foundation (Weeks 1–2)
*Goal: a usable editor*

- [ ] Piece tree buffer (RB-tree of pieces; O(log n) insert, delete, seek, line lookup)
- [ ] Arena allocator
- [ ] Slot-based UI registry (ui.c) with built-in widgets: line_numbers, statusbar items
- [ ] Terminal renderer (dirty lines, ANSI colors, slot-aware)
- [ ] Raw input loop (termios)
- [ ] Basic keybinds (move, insert, delete, save)
- [ ] Config parser (TOML subset)
- [ ] 2 built-in themes

**Milestone:** Open a file, edit it, save it. Feels snappy.

---

### Phase 2 — Intelligence (Weeks 3–4)
*Goal: a smart editor*

- [ ] LSP client (JSON-RPC over stdio)
- [ ] Completion popup UI
- [ ] Diagnostics gutter
- [ ] Hover documentation
- [ ] Go-to-definition
- [ ] Command palette (Ctrl+P)
- [ ] Fuzzy file finder

**Milestone:** Open a C file, get completions from clangd, jump to definitions.

---

### Phase 3 — Time Travel (Week 5)
*Goal: Git as a first-class feature*

- [ ] libgit2 integration
- [ ] Diff gutter (+ / - markers)
- [ ] TimeLine scrubber UI
- [ ] File restore to any commit
- [ ] Inline blame

**Milestone:** Browse full commit history, restore any file version, see who wrote every line.

---

### Phase 4 — Plugins (Week 6)
*Goal: user extensibility*

- [ ] Plugin host process (fork + dlopen)
- [ ] C plugin API (forge_api.h)
- [ ] ForgeScript lexer + parser
- [ ] ForgeScript bytecode VM
- [ ] Shell hooks in config
- [ ] `forge plugin build` command

**Milestone:** Write a 10-line ForgeScript plugin that auto-formats on save.

---

### Phase 5 — Guild (Weeks 7–8)
*Goal: team communication*

- [ ] forge-net Rust binary (IPC bridge)
- [ ] mDNS presence announcement + discovery
- [ ] Guild panel UI (Ctrl+G)
- [ ] Guild chat
- [ ] Ping system (:ping)
- [ ] P2P file sharing (:share / :grab)
- [ ] CRDT collab engine
- [ ] Collab session UI (colored cursors)

**Milestone:** Two terminals on the same wifi, live co-editing, zero config, zero internet.

---

### Phase 6 — Polish (ongoing)
*Goal: daily driver quality*

- [ ] Mouse support
- [ ] Multi-cursor editing
- [ ] Undo tree (not stack)
- [ ] Split panes
- [ ] Macro recording
- [ ] More themes
- [ ] Plugin registry (GitHub topic: `forge-plugin`)
- [ ] Windows support (named pipes instead of Unix sockets)

---

## 16. Key Design Decisions

### Why piece tree over a plain piece table (linked list)?

A linked-list piece table keeps edits O(1) at the splice point, but **finding** that point requires walking the chain from the head — O(n pieces). After a long editing session a file can accumulate tens of thousands of pieces; every cursor movement that crosses a piece boundary, every `buffer_slice` for the renderer, and every line-number calculation degrades to a full linear scan.

The piece tree replaces the linked list with a **Red-Black tree** where each node caches the total character count and line count of its subtree. This turns every positional query into a binary search — O(log n) regardless of fragmentation. Consecutive edits to adjacent positions are merged into the same node before the tree is updated, so the piece count stays low in practice.

VSCode made the same transition (their "piece tree" was written up by Peng Lyu in 2018) and reported that large-file editing became dramatically more responsive as a result.

### Why piece tree over gap buffer?

Gap buffers (used by Emacs) shift content on every insert outside the gap. For a 500MB file, that's hundreds of MB of `memcpy` per edit. The piece tree never copies original content — insert and delete are always O(log n) regardless of file size or cursor position.

### Why a slot-based UI instead of hardcoded panels?

Vim, Neovim, and VSCode hardcode their layout regions in the renderer — plugins can only fill in the *content* of predefined slots, not move or create regions. This means a user who wants a top statusbar, or a right-aligned file tree, or a floating diagnostic panel has to fight against the editor's core assumptions.

Forge's renderer knows nothing about specific UI elements. It only iterates a slot registry and calls each widget's render callback. Built-in components (gutter, statusbar, git timeline) are registered at startup exactly like a user plugin would be — they get no special treatment. The result is that rearranging or hiding any part of the UI is a config change, not a patch.

The cost is real but bounded: `ui.c` is ~500 lines for the slot table, widget lifecycle, and geometry recalculation on terminal resize. The ForgeScript widget bindings add ~200 more. That's the full price of the feature.

### Why process isolation for plugins?

A `.so` loaded into the editor process with a bad pointer dereference will crash the entire editor and lose unsaved work. A plugin in a child process can segfault, hang, or be killed — the editor never notices.

### Why two separate processes (C + Rust)?

The editor must never stutter due to network activity. A dropped packet, a slow peer, a network timeout — none of these should cause the cursor to lag. Separate processes with async IPC means the editor is completely isolated from all network latency.

### Why mDNS for discovery?

mDNS (Multicast DNS) is the same protocol your computer uses to find printers and AirPlay devices. It requires zero configuration, works on any LAN, and has no central server. Every modern OS supports it natively. When two Forge users are on the same network, they find each other automatically within 2 seconds of launching the editor.

### Why CRDT over OT (Operational Transformation)?

OT (used by early Google Docs) requires a central server to order operations. CRDT is fully peer-to-peer — every peer can apply operations in any order and converge to the same result. This fits perfectly with Forge's local-first, no-internet-required philosophy.

### Why ForgeScript instead of Lua?

Lua is a great choice (Neovim uses it). ForgeScript is chosen because:
1. You own the entire stack — debugging is inside your own codebase
2. It can be designed specifically for editor plugins — no general-purpose baggage
3. The VM sandboxes plugins at zero IPC cost (vs process isolation for C plugins)
4. It's a genuinely fun sub-project (~1000 lines of C)

Lua remains an option if ForgeScript proves insufficient — the plugin API is the same either way.

---

*Document version: 0.2 — evolves with the project*
*Last updated: May 2026*
