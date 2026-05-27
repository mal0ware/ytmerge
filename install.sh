#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

OS="$(uname -s)"

install_macos() {
    if ! command -v brew >/dev/null 2>&1; then
        echo "Error: Homebrew is required to install dependencies on macOS." >&2
        echo "Install it from https://brew.sh, then re-run this script." >&2
        exit 1
    fi
    if ! xcode-select -p >/dev/null 2>&1; then
        echo "Error: Xcode Command Line Tools are required (provides clang++ + SDK)." >&2
        echo "Install with: xcode-select --install" >&2
        exit 1
    fi
    echo "Installing build dependencies via Homebrew..."
    brew install nlohmann-json >/dev/null
}

install_linux() {
    # Pick a package manager and install: a C++ compiler, libcurl-dev,
    # nlohmann-json3-dev, plus the clipboard/notify tools used at runtime.
    if command -v apt-get >/dev/null 2>&1; then
        echo "Installing build + runtime dependencies via apt..."
        sudo apt-get update -qq
        sudo apt-get install -y \
            build-essential pkg-config \
            libcurl4-openssl-dev nlohmann-json3-dev \
            xclip wl-clipboard libnotify-bin
    elif command -v dnf >/dev/null 2>&1; then
        echo "Installing build + runtime dependencies via dnf..."
        sudo dnf install -y \
            gcc-c++ make pkgconf-pkg-config \
            libcurl-devel nlohmann-json-devel \
            xclip wl-clipboard libnotify
    elif command -v pacman >/dev/null 2>&1; then
        echo "Installing build + runtime dependencies via pacman..."
        sudo pacman -S --needed --noconfirm \
            base-devel pkgconf \
            curl nlohmann-json \
            xclip wl-clipboard libnotify
    else
        echo "Error: no supported package manager found (apt/dnf/pacman)." >&2
        echo "Install equivalents manually:" >&2
        echo "  - C++20 compiler + pkg-config" >&2
        echo "  - libcurl development headers" >&2
        echo "  - nlohmann/json single-header (>=3.x)" >&2
        echo "  - xclip and/or wl-clipboard for clipboard I/O" >&2
        echo "  - notify-send (libnotify) for desktop notifications" >&2
        exit 1
    fi
}

case "$OS" in
    Darwin) install_macos ;;
    Linux)  install_linux ;;
    MINGW*|MSYS*|CYGWIN*)
        echo "Detected Windows shell ($OS). install.sh doesn't handle Windows." >&2
        echo "See the Windows section of README.md for the MSYS2 install steps." >&2
        exit 1
        ;;
    *)
        echo "Unsupported OS: $OS" >&2
        echo "ytmerge supports macOS, Linux, and Windows (via MSYS2). See README." >&2
        exit 1
        ;;
esac

echo "Building ytmerge..."
make clean >/dev/null 2>&1 || true
make

echo "Installing ytmerge to ~/.local/bin..."
mkdir -p "$HOME/.local/bin"
cp ytmerge "$HOME/.local/bin/ytmerge"
chmod +x "$HOME/.local/bin/ytmerge"

echo ""
echo "Done."
echo ""

if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    case "$(basename "${SHELL:-}")" in
        zsh)  rc="~/.zshrc" ;;
        bash) rc="~/.bashrc" ;;
        *)    rc="your shell's rc file" ;;
    esac
    echo "Warning: ~/.local/bin is NOT on your PATH. Add this to $rc:"
    echo '    export PATH="$HOME/.local/bin:$PATH"'
    echo "Then open a new terminal."
    echo ""
fi

echo "Test with: copy a YouTube URL, then run \`ytmerge\`"
case "$OS" in
    Darwin) echo "Next: open Shortcuts.app to bind it to a keyboard shortcut. See README." ;;
    Linux)  echo "Next: bind \`ytmerge\` to a keyboard shortcut via your DE's settings (GNOME Keyboard, KDE Shortcuts, etc.)." ;;
esac
