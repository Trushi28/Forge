#!/bin/bash
# build.sh — One-command build for Forge editor
#
# Usage:
#   ./build.sh          Build everything (core + net)
#   ./build.sh core     Build only the C core
#   ./build.sh net      Build only the Rust net layer
#   ./build.sh release  Build release versions of both
#   ./build.sh clean    Clean all build artifacts
#   ./build.sh plugins  Build example plugins

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${CYAN}[forge]${NC} $*"; }
ok()  { echo -e "${GREEN}[  OK ]${NC} $*"; }
err() { echo -e "${RED}[FAIL]${NC} $*"; }
warn(){ echo -e "${YELLOW}[WARN]${NC} $*"; }

build_core() {
    log "Building C core..."
    make -j$(nproc) 2>&1
    if [ $? -eq 0 ]; then
        ok "Core built: ./forge"
    else
        err "Core build failed"
        exit 1
    fi
}

build_net() {
    log "Building Rust net layer..."
    cd net
    cargo build 2>&1
    if [ $? -eq 0 ]; then
        ok "Net built: net/target/debug/forge-net"
    else
        err "Net build failed"
        exit 1
    fi
    cd ..
}

build_core_release() {
    log "Building C core (release)..."
    CFLAGS="-O3 -DNDEBUG" make -j$(nproc) 2>&1
    ok "Core built (release): ./forge"
}

build_net_release() {
    log "Building Rust net layer (release)..."
    cd net
    cargo build --release 2>&1
    ok "Net built (release): net/target/release/forge-net"
    cd ..
}

build_plugins() {
    log "Building plugins..."
    make plugins 2>&1
    ok "Plugins built in build/"
}

clean_all() {
    log "Cleaning all build artifacts..."
    make clean
    cd net && cargo clean 2>/dev/null && cd ..
    ok "Clean complete"
}

install_forge() {
    log "Installing forge to ~/.local/bin/..."
    mkdir -p "$HOME/.local/bin"
    cp forge "$HOME/.local/bin/forge"
    if [ -f net/target/release/forge-net ]; then
        cp net/target/release/forge-net "$HOME/.local/bin/forge-net"
    elif [ -f net/target/debug/forge-net ]; then
        cp net/target/debug/forge-net "$HOME/.local/bin/forge-net"
    fi
    ok "Installed to ~/.local/bin/"
}

# Setup config directory
setup_config() {
    local config_dir="$HOME/.config/forge"
    if [ ! -d "$config_dir" ]; then
        log "Creating config directory: $config_dir"
        mkdir -p "$config_dir/plugins"
        mkdir -p "$config_dir/scripts"
        mkdir -p "$config_dir/themes"

        # Copy default config if it doesn't exist
        if [ ! -f "$config_dir/config.toml" ]; then
            cat > "$config_dir/config.toml" << 'EOF'
# Forge Editor Configuration

[editor]
tab_width = 4
use_spaces = true
line_numbers = true
mouse = true
scrolloff = 8
theme = "catppuccin"

[lsp]
auto_detect = true
show_hints = true
completion = true
diagnostics = true

[git]
show_gutter = true
show_blame = false
timeline = true

[guild]
handle = "anon"
name = ""
color = "cyan"
announce_lan = true

[hooks]
# on_save = "echo saved"
# on_open = ""
# on_close = ""

[lang.c]
indent = 4
formatter = "clang-format -i $FILE"
lsp = "clangd"

[lang.python]
indent = 4
formatter = "black $FILE"
lsp = "pyright"

[lang.rust]
indent = 4
formatter = "rustfmt $FILE"
lsp = "rust-analyzer"
EOF
            ok "Created default config: $config_dir/config.toml"
        fi

        # Create guild profile
        if [ ! -f "$config_dir/profile.toml" ]; then
            cat > "$config_dir/profile.toml" << 'EOF'
# Forge Guild Profile
handle = "anon"
guild = ""
color = "cyan"
EOF
            ok "Created guild profile: $config_dir/profile.toml"
        fi
    fi
}

# Print usage
usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  (none)    Build everything (core + net)"
    echo "  core      Build only the C core"
    echo "  net       Build only the Rust net layer"
    echo "  release   Build release versions of both"
    echo "  plugins   Build example plugins"
    echo "  clean     Clean all build artifacts"
    echo "  install   Install to ~/.local/bin"
    echo "  setup     Create config directory and defaults"
}

case "${1:-all}" in
    all)
        setup_config
        build_core
        build_net
        echo ""
        ok "Build complete!"
        echo "  Run: ./forge <file>"
        ;;
    core)
        build_core
        ;;
    net)
        build_net
        ;;
    release)
        build_core_release
        build_net_release
        ok "Release build complete!"
        ;;
    plugins)
        build_plugins
        ;;
    clean)
        clean_all
        ;;
    install)
        build_core_release
        build_net_release
        install_forge
        ;;
    setup)
        setup_config
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        err "Unknown command: $1"
        usage
        exit 1
        ;;
esac
