# Forge

A terminal code editor in C, with an optional Rust companion (`forge-net`) for LAN/remote collaboration.

- Piece-tree buffer, multi-buffer tabs, undo tree, multi-cursor, incremental search
- LSP client: completion, diagnostics, hover, go-to-definition, find references
- Optional git integration via libgit2: diff gutter, inline blame, commit timeline
- Command palette, fuzzy file finder, six themes, live config reload
- Extensible with ForgeScript, shell hooks, and out-of-process worker plugins
- **Guild** (experimental): peer chat, file sharing, pings, and CRDT co-editing

Linux only for now.

## Build

Requires GCC or Clang and Make. libgit2 (with headers) and a Rust toolchain are optional.

```sh
make                 # builds ./forge (libgit2 auto-detected; WITH_GIT=0/1 to force)
./forge path/to/file.c

make net             # builds forge-net (needs cargo)
make plugins         # builds the example .so plugins
```

`./build.sh` wraps the same steps with dependency checks (`./build.sh all`, `release`, `install`, `setup`).

## Keys

| Key | Action |
|---|---|
| `Ctrl+S` / `Ctrl+Q` | Save / quit |
| `Ctrl+P` | Command palette (`>` for files, `:` for go-to-line) |
| `Ctrl+F` / `Ctrl+R` | Search forward / backward |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+K` | Hover docs |
| `Ctrl+]` | Go to definition |
| `Ctrl+T` | Git timeline (`←` `→` browse, `Enter` view commit, `Esc` back) |
| `Ctrl+B` | Git blame |
| `Ctrl+G` | Guild panel |
| `Ctrl+D` | Duplicate line |
| `Ctrl+U` / `Ctrl+W` | Delete to line start / delete word |
| `Ctrl+A` / `Ctrl+E` | Line start / end |
| `Ctrl+←` / `Ctrl+→` | Word jump |
| `Shift+arrows` | Select (`Ctrl+C` copies via `xclip` or `xsel`) |
| `Alt+click`, `Ctrl+Alt+↑/↓` | Add cursor (`Esc` clears) |
| `Ctrl+PgUp` / `Ctrl+PgDn` | Switch buffers |

## Configuration

`~/.config/forge/config.toml`, reloaded on change. `./build.sh setup` writes a starter file.

```toml
[editor]
tab_width  = 4
use_spaces = true
scrolloff  = 8
theme      = "catppuccin"   # catppuccin | dracula | tokyo_night | gruvbox | solarized | light

[lsp]
auto_detect = true

[git]
timeline = true

[hooks]
on_save = "clang-format -i $FILE"   # runs via sh; $FILE is quoted for you

[lang.c]
formatter = "clang-format -i $FILE"
```

Language servers are found on `$PATH`: `clangd`, `pyright-langserver` / `pylsp`, `rust-analyzer`,
`typescript-language-server`, `gopls`, `lua-language-server`.

## Extending Forge

Everything below is off by default except hooks, which you write yourself.

| Mechanism | Runs | Enable |
|---|---|---|
| Shell hooks (`[hooks]`) | `sh -c`, as you | set a command |
| ForgeScript (`~/.config/forge/scripts/*.fs`) | in-editor VM | drop a file in; `run` needs `[scripts] allow_shell = true` |
| Worker plugins (`~/.config/forge/plugins/*.forge-plugin`) | separate process | `[plugins] allow_workers = true` |
| Native `.so` plugins | **inside the editor, full privileges** | `[plugins] allow_legacy_native = true` |

None of these are sandboxed. Workers can't crash the editor, but they run with your permissions.

**ForgeScript**

```
on save {
    notify "saved"
}
```

(`wrap selection with ...` parses but is not implemented yet.)

**Worker plugins** speak one JSON object per line over stdin/stdout:

```json
{"protocol":2,"event":"save","key":0,"file":"/path/to/file.c"}
{"protocol":2,"action":"notify","message":"Saved"}
{"protocol":2,"action":"insert","text":"generated text"}
```

Events: `save`, `open`, `close`, `keypress`. Actions: `notify`, `insert`. Responses over 4 KiB or invalid output stop the worker; a slow worker drops events rather than blocking the editor.
Build a C worker with `./forge plugin build plugins/hello_worker.c`; see also `plugins/notify-on-save.forge-plugin`.

## Guild (experimental)

`forge-net` is started automatically by the editor and talks to it over a Unix socket
(`~/.config/forge/net/forge-net.sock`). Set your identity in `~/.config/forge/profile.toml`:

```toml
handle = "yourname"
guild  = "your guild"
color  = "cyan"
```

- **Discovery:** mDNS on the LAN. Optional WebRTC for other networks (see below).
- **Transport:** TCP with X25519 key exchange (identity + per-connection ephemeral keys), XChaCha20-Poly1305.
- **Trust:** first contact is pinned by fingerprint. Verify it out-of-band, then approve with `:trust accept <fingerprint>`. A changed key is rejected until you approve it again.
- **Commands** (palette): `:chat <text>`, `:ping`, `:share`, `:grab <peer> <file>`, `:drop <file>`, `:collab <peer>`, `:collab_accept`.
- **Limits:** shared files and collab buffers are capped at 700 KB.

**Remote peers.** Set `FORGE_RENDEZVOUS_URL` (a `wss://` signaling broker) before `forge-net` starts. ICE uses
`FORGE_STUN_SERVERS`, and optionally `FORGE_TURN_URL` / `FORGE_TURN_USERNAME` / `FORGE_TURN_CREDENTIAL`.
Run a broker with `forge-net --rendezvous-server 127.0.0.1:8765` behind a TLS proxy. Forge does not host a broker or TURN server.

The protocol has not been independently audited. Don't use Guild for sensitive data.

## Known limitations

- Unicode: codepoint input, cell-width rendering, and LSP UTF-16 columns work; grapheme-cluster editing doesn't.
- Syntax highlighting is a generic tokenizer (C/Rust/Go/JS-style keywords), not per-language grammars.
- Clipboard is copy-only; paste with your terminal.
- No split panes, no modal editing.
- Layout slots exist, but config-driven widget layout is only partly implemented.

## Development

```sh
make test            # C unit tests
make fuzz            # ASan/UBSan: piece-tree fuzz, IPC framing, LSP JSON, palette rendering
make gittest         # libgit2 integration (WITH_GIT=1)
(cd net && cargo test)
```

```
core/      C editor (buffer, render, lsp, git, palette, plugins, ipc, ...)
net/       Rust: forge-net (guild, crypto, CRDT, WebRTC, rendezvous broker)
plugins/   example plugins
scripts/   example ForgeScript
tests/     C tests
```

See [CONTRIBUTING.md](CONTRIBUTING.md).
