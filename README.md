# Forge

Forge is a terminal code editor written in C with an optional Rust networking
companion. The current implementation includes a piece-tree text buffer,
multi-buffer editing, undo/redo, a command palette and file finder, syntax
highlighting, themes, optional libgit2 integration, an LSP client, native C
plugins, ForgeScript, and an experimental Guild layer.

The longer [`Readme.md`](Readme.md) is the architecture and product roadmap.
It contains design targets that are still in progress; use the status below and
the source code to determine what is currently supported.

## Build

The C editor requires GCC or Clang and Make. libgit2 is optional: install its
development headers to enable the Git gutter, blame, and timeline.

```sh
make
./forge path/to/file.c
```

Build the optional networking companion and example plugins with:

```sh
make net
make plugins
```

Run the C and Rust test suites with:

```sh
make test
(cd net && cargo test)
```

`./build.sh` provides dependency checks and release builds. `./build.sh all`
builds the editor, networking companion, and bundled plugins.

## Feature status

| Area | Current status |
|---|---|
| Editing | Core editing, multiple buffers, undo/redo, search, and multi-cursor are implemented. UTF-8 scalar input, cursor stepping, display-cell rendering, and LSP UTF-16 columns are implemented; grapheme-cluster behavior remains in progress. |
| LSP | Completion, diagnostics, hover, definition, and references are implemented; server launch avoids a shell, message sizes are bounded, and sessions are reused by server and workspace root across buffers. |
| Git | Optional libgit2 support provides branch, diff, blame, and timeline data. |
| Plugins | ForgeScript requires no build step. Simple C workers build with `forge plugin build <file.c>` and run as opt-in processes with bounded event, insert, and notification IPC. Legacy `.so` plugins remain in-process and disabled by default. |
| Guild | The Rust companion supports LAN discovery, encrypted TCP peer messages, chat, sharing, and experimental collaboration. Opt-in WebRTC uses room-based signaling, STUN/ICE, optional external TURN credentials, and the existing fingerprint plus ephemeral-DH application handshake. Remote use needs a deployed WSS broker; Forge does not host signaling or TURN services. |
| UI customization | Themes and basic slot/widget registration exist; arbitrary config-driven widget layouts are not fully implemented. |

## Improvement roadmap

The broad improvement work is ordered so each layer has a testable boundary:

1. **Editor Unicode support:** decode UTF-8 input, render codepoints using
   terminal cell widths, and keep byte offsets safe for storage. Then cover
   grapheme-cluster selections, clipboard, and remaining LSP position cases.
   Outgoing requests and definition/reference navigation now convert UTF-16
   columns. `build.sh` is valid UTF-8; Forge's old byte-per-cell input and
   rendering path caused the display problem shown in your screenshot.
2. **Plugin workers:** define a versioned, bounded RPC protocol; run native
   extensions out of process with timeouts and explicit capabilities; migrate
   built-ins and add lifecycle/restart handling. Keep legacy `dlopen` opt-in
   until migration is complete.
3. **LSP workspaces:** reuse sessions per server and nearest workspace root;
   track open documents per buffer and restart the active workspace session.
   Remaining work: nonblocking queued writes, cancellation, response timeouts,
   and diagnostics indexed by URI.
4. **Remote Guild:** WebRTC data-channel transport, room-based offer/answer
   signaling, trickled ICE, STUN, and optional TURN configuration are wired.
   Still needed: production deployment behind TLS, time-limited TURN credentials,
   reconnect/ICE restart, multi-network interoperability coverage, and careful
   resource limits for long-lived data channels. Data-channel messages up to
   10 MiB are fragmented into ordered encrypted frames; larger payloads try the
   existing TCP path. The signaling broker carries only bounded setup messages;
   peer payloads stay end-to-end encrypted.
5. **Guild trust and protocol:** complete reviewed identity persistence,
   replay/ordering protections, bounded resource use, transfer limits, and
   independently reviewed cryptographic design before calling Guild
   production-ready.

The implementation currently has foundations for Unicode and the worker
protocol, a multi-workspace LSP manager, and opt-in Guild rendezvous/WebRTC
transport. Full grapheme editing, worker capabilities and built-in migration,
LSP cancellation and queued writes, and production deployment/interoperability
work for remote Guild remain unfinished.
Track those items here; the checklist in `Readme.md` is the original proposal.

Forge targets Linux first. Guild is optional; the editor remains usable without
the Rust companion. Guild remains experimental and its peer protocol has not
received an independent security audit; do not treat it as production-ready for
sensitive data.

## Configuration

The editor reads `~/.config/forge/config.toml`; ForgeScript files can be placed
in `~/.config/forge/scripts`, and native plugins in
`~/.config/forge/plugins`. See the example configuration in `build.sh` and the
architecture roadmap for the current settings.

Native shared-object plugins execute inside Forge with full user privileges.
They are disabled by default. Worker plugins are also opt-in; their separate
process prevents a crash from taking down Forge, but workers still run with the
user's permissions and are not sandboxed:

```toml
[plugins]
allow_workers = false
allow_legacy_native = false

[scripts]
allow_shell = false
```

When enabled, Forge starts executable `*.forge-plugin` files from
`~/.config/forge/plugins`. Protocol v2 uses one JSON object per line on stdin
and stdout. Forge sends `save`, `open`, `close`, or `keypress` events with the
file path and key. Workers may return bounded notification or insert actions:

```json
{"protocol":2,"event":"save","key":0,"file":"/path/to/file.c"}
{"protocol":2,"action":"notify","message":"Saved successfully"}
{"protocol":2,"action":"insert","text":"generated text"}
```

Each worker has nonblocking pipes and a 4 KiB response limit; slow workers drop
events instead of blocking editing. `insert` text is UTF-8 validated and enters
Forge's undo and collaboration paths. Worker stdout is reserved for protocol
messages. An executable example
is in [`plugins/notify-on-save.forge-plugin`](plugins/notify-on-save.forge-plugin);
copy it into the user plugin directory to try it.

You do not need to edit `build.sh` for a custom plugin. A ForgeScript plugin can
be saved directly under `~/.config/forge/scripts`:

```text
on save {
    notify "Saved from my script"
}
```

For a simple C worker, write a `main` loop that reads event lines and prints
protocol responses, then use Forge's compiler command:

```sh
./forge plugin build plugins/hello_worker.c
```

That compiles it into `~/.config/forge/plugins/hello_worker.forge-plugin`.
Set `allow_workers = true` and reload config or restart Forge to start it;
with workers enabled, config reload also discovers newly built plugins. See
[`plugins/hello_worker.c`](plugins/hello_worker.c) for the complete example.

## Development

```sh
make test
make fuzz
(cd net && cargo test)
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for development workflow and
[Readme.md](Readme.md) for the evolving design and roadmap.
