# ytmerge

Copy YouTube URLs → press one keyboard shortcut → paste merged transcripts wherever you want them.

A small macOS CLI for batching YouTube transcripts into a single copy-pasteable blob. Built for research-synthesis workflows — feeding many videos at once into Claude (or anything else) without manually opening each one.

## Why

Watching 15 videos to extract a few useful threads is slow. Pasting transcripts one at a time into an LLM is also slow. `ytmerge` collapses the in-between step into a single keyboard shortcut:

1. Copy a batch of URLs from anywhere — browser tabs, notes, chat messages, scratchpad. Messy text is fine.
2. Press your bound shortcut.
3. Paste the merged transcripts wherever you're going to use them.

## Install

```bash
git clone https://github.com/mal0ware/ytmerge.git
cd ytmerge
./install.sh
```

The install script will:
1. Install the two Python dependencies (`youtube-transcript-api`, `pyperclip`)
2. Copy the script to `~/.local/bin/ytmerge`
3. Mark it executable

Make sure `~/.local/bin` is on your `PATH`. If it isn't, add this to `~/.zshrc`:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Then `source ~/.zshrc` or open a new terminal.

## Quick test

Copy any YouTube URL to your clipboard, then run:

```bash
ytmerge
```

You'll see per-video progress in the terminal, get a macOS notification when it's done, and your clipboard will be replaced with the merged transcripts.

## Bind to a global keyboard shortcut

This is the part that makes it actually fast.

1. Open **Shortcuts.app** (built into macOS).
2. Click **+** to create a new shortcut. Name it `ytmerge`.
3. From the action search on the right, drag in **Run Shell Script**.
4. Set Shell to `/bin/zsh` and the script body to:
   ```
   ~/.local/bin/ytmerge
   ```
5. Click the info icon (ⓘ) at the top → **Add Keyboard Shortcut** → press your preferred combo (e.g. `⌃⌥⌘Y`).
6. Optional: tick **Pin in Menu Bar** for a clickable fallback.

No code signing or developer profile needed — Shortcuts.app runs the script as you.

## Output format

Each video becomes a header block followed by its transcript:

```
================================================================================
TITLE: The video's actual title
URL:   https://www.youtube.com/watch?v=...
================================================================================

[full transcript as one paragraph]
```

Multiple videos are separated by blank lines. The structure is intentional — clear delimiters help downstream LLMs attribute insights to the right source instead of blurring everything together.

## Notes

- The URL parser is forgiving. Paste anything containing YouTube links and it'll extract them. Duplicates are deduped automatically. Supports `youtube.com/watch`, `youtu.be`, `youtube.com/shorts`, and `youtube.com/embed`.
- Videos with disabled captions or only non-English auto-captions are skipped silently. The completion notification reports how many were skipped.
- Transcripts come from [`youtube-transcript-api`](https://github.com/jdepoix/youtube-transcript-api); titles from YouTube's public oEmbed endpoint. No API keys, no auth.
- Tested on macOS. The script itself is platform-agnostic, but the notification step uses `osascript`. On Linux, swap in `notify-send`; on Windows, remove it.

## License

MIT — see [LICENSE](LICENSE).
