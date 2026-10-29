# Contributing

## Build and checks

Build the C editor with `make`. Use `make WITH_GIT=0` to check the build without
libgit2, or `make WITH_GIT=1` when its development headers are installed.

Run the relevant checks before opening a change:

```sh
make test
make fuzz
(cd net && cargo test)
```

Keep changes focused on one subsystem when possible. For C changes, preserve
the public header/API contract and add regression coverage for behavior changes.
For Rust protocol changes, update both the serde message types and the C-side
IPC consumer, and cover malformed or out-of-order input.

## Product and security notes

`Readme.md` describes future product goals as well as implemented behavior.
Update `README.md` whenever a feature moves between implemented, experimental,
and planned status.

Guild networking and native plugins cross trust boundaries. Do not describe
in-process native plugins as sandboxed, and do not treat LAN discovery as peer
authentication. Changes to identity, trust, encryption, or wire formats need
explicit migration behavior and focused tests.
