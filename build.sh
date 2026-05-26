#!/bin/bash
# build.sh — One-command build for Forge editor
#
# Usage:
#   ./build.sh          Build C core only (fastest, no Rust required)
#   ./build.sh all      Build core + forge-net (requires Rust toolchain)
#   ./build.sh release  Build optimised release binaries of both
#   ./build.sh plugins  Build example plugins (.so)
#   ./build.sh install  Install to ~/.local/bin
#   ./build.sh setup    Create ~/.config/forge with defaults
#   ./build.sh clean    Remove all build artefacts
#   ./build.sh deps     Check / install build dependencies

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

log()  { echo -e "${CYAN}[forge]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK ]${NC} $*"; }
err()  { echo -e "${RED}[FAIL]${NC} $*" >&2; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
bold() { echo -e "${BOLD}$*${NC}"; }

# ── Dependency checker ─────────────────────────────────────────
check_deps() {
    local missing=0

    _check() {
        local name="$1" cmd="$2" hint="$3"
        if ! command -v "$cmd" &>/dev/null 2>&1; then
            err "Missing: $name  ($hint)"
            missing=1
        else
            ok "$name  ($(command -v "$cmd"))"
        fi
    }

    _check_lib() {
        local name="$1" pkg="$2"
        if ! pkg-config --exists "$name" 2>/dev/null && \
           ! ldconfig -p 2>/dev/null | grep -q "${name}"; then
            err "Missing lib: $pkg"
            if command -v apt-get &>/dev/null; then
                warn "  Fix: sudo apt-get install $pkg"
            elif command -v dnf &>/dev/null; then
                warn "  Fix: sudo dnf install $pkg"
            elif command -v pacman &>/dev/null; then
                warn "  Fix: sudo pacman -S $pkg"
            fi
            missing=1
        else
            ok "lib: $pkg"
        fi
    }

    bold "── Checking build dependencies ─────────────────────"
    _check "gcc"   gcc   "sudo apt-get install build-essential"
    _check "make"  make  "sudo apt-get install make"
    _check_lib "libgit2" "libgit2-dev"

    if [ $missing -ne 0 ]; then
        echo ""
        err "One or more dependencies are missing. Install them and retry."
        exit 1
    fi
    echo ""
    ok "All core dependencies present."
}

# ── Build functions ────────────────────────────────────────────
build_core() {
    log "Building C core…"
    make -j"$(nproc 2>/dev/null || echo 4)"
    ok "Core built → ./forge"
}

build_net() {
    if ! command -v cargo &>/dev/null; then
        warn "Rust/cargo not found — skipping forge-net."
        warn "Install Rust: https://rustup.rs"
        return 0
    fi
    log "Building Rust net layer…"
    cd net
    cargo build 2>&1
    ok "Net built → net/target/debug/forge-net"
    cd ..
}

build_plugins() {
    log "Building plugins…"
    make plugins
    ok "Plugins built → build/*.so"
}

build_core_release() {
    log "Building C core (release)…"
    make clean
    CFLAGS="-Wall -Wextra -std=c11 -O3 -DNDEBUG -Wno-unused-result \
            -Wno-format-truncation -Wno-missing-field-initializers \
            -D_GNU_SOURCE -flto" \
    make -j"$(nproc 2>/dev/null || echo 4)"
    ok "Core built (release) → ./forge"
}

build_net_release() {
    if ! command -v cargo &>/dev/null; then
        warn "Rust/cargo not found — skipping forge-net release."
        return 0
    fi
    log "Building Rust net layer (release)…"
    cd net && cargo build --release && cd ..
    ok "Net built (release) → net/target/release/forge-net"
}

clean_all() {
    log "Cleaning…"
    make clean
    if command -v cargo &>/dev/null && [ -f net/Cargo.toml ]; then
        cd net && cargo clean 2>/dev/null; cd ..
    fi
    ok "Clean complete."
}

install_forge() {
    local bin="$HOME/.local/bin"
    mkdir -p "$bin"
    cp forge "$bin/forge"
    chmod +x "$bin/forge"
    ok "Installed: $bin/forge"

    if [ -f net/target/release/forge-net ]; then
        cp net/target/release/forge-net "$bin/forge-net"
        ok "Installed: $bin/forge-net"
    elif [ -f net/target/debug/forge-net ]; then
        cp net/target/debug/forge-net "$bin/forge-net"
        ok "Installed: $bin/forge-net  (debug build)"
    fi

    # Remind about PATH if needed
    if ! echo "$PATH" | grep -q "$bin"; then
        warn "Add $bin to your PATH:"
        warn "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
    fi
}

setup_config() {
    local cfg="$HOME/.config/forge"
    [ -d "$cfg" ] && { ok "Config already exists: $cfg"; return 0; }

    log "Creating config directory: $cfg"
    mkdir -p "$cfg/plugins" "$cfg/scripts" "$cfg/themes"

    cat > "$cfg/config.toml" << 'EOF'
# Forge Editor Configuration
# See: https://github.com/Trushi28/Forge

[editor]
tab_width   = 4
use_spaces  = true
line_numbers = true
scrolloff   = 8
theme       = "catppuccin"   # catppuccin | gruvbox | tokyo_night | solarized | light

[lsp]
auto_detect  = true
show_hints   = true
completion   = true
diagnostics  = true

[git]
show_gutter  = true
show_blame   = false
timeline     = true

[guild]
handle       = "anon"
color        = "cyan"
announce_lan = false

[hooks]
# on_save  = "echo saved $FILE"
# on_open  = ""
# on_close = ""

[lang.c]
indent    = 4
formatter = "clang-format -i $FILE"
lsp       = "clangd"

[lang.python]
indent    = 4
formatter = "black $FILE"
lsp       = "pyright"

[lang.rust]
indent    = 4
formatter = "rustfmt $FILE"
lsp       = "rust-analyzer"

[lang.go]
indent    = 4
formatter = "gofmt -w $FILE"
lsp       = "gopls"

[lang.typescript]
indent    = 2
formatter = "prettier --write $FILE"
lsp       = "typescript-language-server --stdio"
EOF
    ok "Created: $cfg/config.toml"

    cat > "$cfg/profile.toml" << 'EOF'
handle = "anon"
guild  = ""
color  = "cyan"
EOF
    ok "Created: $cfg/profile.toml"
}

print_usage() {
    bold "Forge build script"
    echo ""
    echo "  ./build.sh              Build C core only"
    echo "  ./build.sh all          Build core + forge-net"
    echo "  ./build.sh release      Optimised release build"
    echo "  ./build.sh plugins      Build example plugins (.so)"
    echo "  ./build.sh install      Install to ~/.local/bin"
    echo "  ./build.sh setup        Create ~/.config/forge defaults"
    echo "  ./build.sh clean        Remove all build artefacts"
    echo "  ./build.sh deps         Check build dependencies"
    echo ""
    echo "Quick start (no Rust needed):"
    echo "  ./build.sh deps && ./build.sh && ./forge <file>"
}

# ── Dispatch ───────────────────────────────────────────────────
case "${1:-core}" in
    core|"")
        check_deps
        build_core
        echo ""
        ok "Done.  Run: ./forge <file>"
        echo "   Keybinds: Ctrl+S save  Ctrl+F find  Ctrl+P palette"
        echo "             Ctrl+T git   Ctrl+Z undo  Ctrl+Y redo"
        echo "             Ctrl+Q quit"
        ;;
    all)
        check_deps
        build_core
        build_net
        build_plugins
        echo ""
        ok "Full build complete.  Run: ./forge <file>"
        ;;
    release)
        check_deps
        build_core_release
        build_net_release
        echo ""
        ok "Release build complete."
        ;;
    plugins)
        build_plugins
        ;;
    install)
        check_deps
        build_core_release
        build_net_release
        install_forge
        ;;
    setup)
        setup_config
        ;;
    clean)
        clean_all
        ;;
    deps)
        check_deps
        ;;
    -h|--help|help)
        print_usage
        ;;
    *)
        err "Unknown command: $1"
        print_usage
        exit 1
        ;;
esac
