#!/usr/bin/env python3
"""ytmerge — YouTube URLs in → merged transcripts on the clipboard.

Input sources, in priority order:
  1. Command-line args:    ytmerge URL [URL ...]
  2. Stdin (when piped):   cat urls.txt | ytmerge
  3. Clipboard (default):  ytmerge

In every case, the merged transcript blob is written back to the clipboard
with clear dividers between videos, and a macOS notification fires on completion.
"""

USAGE = """\
usage: ytmerge [URL ...] | ytmerge -

Reads YouTube URLs from CLI args, stdin (with `-`), or the clipboard
(default). Writes merged transcripts to the clipboard.

Examples:
  ytmerge                          # read from clipboard (works from Shortcuts.app)
  ytmerge "URL1" "URL2"            # read from args — QUOTE URLs in zsh (the ? globs)
  cat urls.txt | ytmerge -         # read from stdin
"""

import json
import re
import subprocess
import sys
import urllib.request

import pyperclip
from youtube_transcript_api import (
    YouTubeTranscriptApi,
    YouTubeTranscriptApiException,
)

VIDEO_ID_RE = re.compile(r"(?:v=|youtu\.be/|/embed/|/shorts/)([A-Za-z0-9_-]{11})")
DIVIDER = "=" * 80


def notify(msg: str) -> None:
    """macOS banner notification + stderr fallback."""
    safe = msg.replace('"', "'")
    try:
        subprocess.run(
            ["osascript", "-e", f'display notification "{safe}" with title "ytmerge"'],
            check=False,
            capture_output=True,
        )
    except Exception:
        pass
    print(msg, file=sys.stderr)


def extract_ids(text: str) -> list[str]:
    """Pull unique YouTube video IDs from arbitrary pasted text."""
    seen, ids = set(), []
    for token in re.split(r"\s+", text):
        token = token.strip().rstrip(",;)")
        m = VIDEO_ID_RE.search(token)
        if m and m.group(1) not in seen:
            seen.add(m.group(1))
            ids.append(m.group(1))
    return ids


def fetch_title(video_id: str) -> str:
    """Title via YouTube oEmbed (no API key)."""
    url = (
        f"https://www.youtube.com/oembed?url="
        f"https://www.youtube.com/watch?v={video_id}&format=json"
    )
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read()).get("title", video_id)
    except Exception:
        return video_id


def fetch_transcript(video_id: str) -> str | None:
    # Narrow except: only swallow the library's own "can't get this transcript"
    # errors (disabled captions, blocked, unavailable). Letting other exceptions
    # bubble up makes real bugs visible instead of masquerading as "no captions."
    try:
        segments = YouTubeTranscriptApi().fetch(video_id)
    except YouTubeTranscriptApiException:
        return None
    return " ".join(s.text.replace("\n", " ") for s in segments)


def read_input() -> tuple[str, str]:
    """Return (raw_text, source_label).

    A bare `-` in argv means read stdin. Auto-detecting stdin via isatty()
    would break the Shortcuts.app launch path (Shortcuts hands the script
    a non-tty stdin even though there's no data piped in).
    """
    args = sys.argv[1:]
    if args == ["-"]:
        return sys.stdin.read(), "stdin"
    if args:
        return " ".join(args), "args"
    return pyperclip.paste() or "", "clipboard"


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(USAGE)
        return 0

    raw, source = read_input()
    if not raw or not raw.strip():
        notify(f"{source} is empty")
        return 1

    ids = extract_ids(raw)
    if not ids:
        notify(f"no YouTube URLs found in {source}")
        return 1

    print(f"found {len(ids)} videos. fetching...", file=sys.stderr)
    blocks, skipped = [], []
    for i, vid in enumerate(ids, 1):
        print(f"  [{i}/{len(ids)}] {vid}", file=sys.stderr)
        transcript = fetch_transcript(vid)
        if not transcript:
            skipped.append(vid)
            continue
        title = fetch_title(vid)
        url = f"https://www.youtube.com/watch?v={vid}"
        blocks.append(
            f"{DIVIDER}\n"
            f"TITLE: {title}\n"
            f"URL:   {url}\n"
            f"{DIVIDER}\n\n"
            f"{transcript}\n"
        )

    if not blocks:
        notify(f"all {len(ids)} failed — no captions available")
        return 1

    output = "\n\n".join(blocks)
    pyperclip.copy(output)

    msg = f"✓ {len(blocks)} transcripts copied ({len(output):,} chars)"
    if skipped:
        msg += f"  [skipped {len(skipped)}]"
    notify(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
