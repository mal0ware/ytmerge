#!/usr/bin/env bash
set -e

echo "Installing Python dependencies..."
pip3 install --break-system-packages --user -r requirements.txt

echo "Installing ytmerge to ~/.local/bin..."
mkdir -p "$HOME/.local/bin"
cp ytmerge.py "$HOME/.local/bin/ytmerge"
chmod +x "$HOME/.local/bin/ytmerge"

echo ""
echo "Done."
echo ""
if [[ ":$PATH:" != *":$HOME/.local/bin:"* ]]; then
    echo "⚠  ~/.local/bin is NOT on your PATH. Add this to ~/.zshrc:"
    echo '    export PATH="$HOME/.local/bin:$PATH"'
    echo "Then: source ~/.zshrc"
    echo ""
fi
echo "Test with: copy a YouTube URL, then run \`ytmerge\`"
echo "Next: open Shortcuts.app to bind it to a keyboard shortcut. See README."
