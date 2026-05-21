#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

if ! command -v python3 >/dev/null 2>&1; then
    echo "Error: python3 is not installed." >&2
    echo "Install it with \`brew install python\` (macOS) or your system package manager." >&2
    exit 1
fi

echo "Installing Python dependencies..."

# `python3 -m pip` ties the install to the same interpreter the shebang
# resolves to, so the script can't end up with deps installed against a
# different python than the one it runs under.
PIP_FLAGS=(--user --no-warn-script-location)

# pip >= 23.0 requires --break-system-packages on PEP 668 systems
# (Homebrew Python, recent Debian/Ubuntu). Older pip errors on the
# unknown flag, so only add it when supported.
if python3 -m pip install --help 2>/dev/null | grep -q -- '--break-system-packages'; then
    PIP_FLAGS+=(--break-system-packages)
fi

python3 -m pip install "${PIP_FLAGS[@]}" -r requirements.txt

echo "Installing ytmerge to ~/.local/bin..."
mkdir -p "$HOME/.local/bin"
cp ytmerge.py "$HOME/.local/bin/ytmerge"
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
    echo "⚠  ~/.local/bin is NOT on your PATH. Add this to $rc:"
    echo '    export PATH="$HOME/.local/bin:$PATH"'
    echo "Then open a new terminal."
    echo ""
fi

echo "Test with: copy a YouTube URL, then run \`ytmerge\`"
echo "Next: open Shortcuts.app to bind it to a keyboard shortcut. See README."
