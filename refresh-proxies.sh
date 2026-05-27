#!/usr/bin/env bash
# Refresh ~/.cache/ytmerge/proxies.txt with a fresh batch of free public HTTP
# proxies from TheSpeedX/PROXY-List. The list goes stale fast (entries die in
# hours); rerun this when ytmerge starts reporting mostly-dead proxies.
#
# Usage:  ./refresh-proxies.sh [COUNT]
# COUNT defaults to 100.
#
# Free public proxies are noisy by nature. Most will be dead, slow, or blocked
# by Google. ytmerge's rotation + cooldown is designed for exactly that. Expect
# only a fraction of any given list to actually reach YouTube successfully.

set -e

COUNT="${1:-100}"
DEST="$HOME/.cache/ytmerge/proxies.txt"
SOURCE_URL="https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/http.txt"

mkdir -p "$(dirname "$DEST")"

raw=$(curl -sS --max-time 20 "$SOURCE_URL")
if [ -z "$raw" ]; then
    echo "Error: upstream list fetch returned empty." >&2
    exit 1
fi

{
    echo "# Fetched $(date -u +%FT%TZ) from TheSpeedX/PROXY-List (http.txt)"
    echo "# Public free proxies. Most will be dead or blocked. Refresh periodically."
    echo "#"
    echo "$raw" \
        | grep -E '^[0-9]{1,3}(\.[0-9]{1,3}){3}:[0-9]{1,5}[[:space:]]*$' \
        | head -"$COUNT" \
        | sed 's|^|http://|'
} > "$DEST"

LINES=$(grep -cv '^#' "$DEST")
echo "wrote $LINES proxies to $DEST"
echo "use with: ytmerge --proxy-list \"$DEST\""
