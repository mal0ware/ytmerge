<div align="center">

<img src="assets/logo.svg" width="80" alt="ytmerge logo">

# ytmerge

**Paste YouTube links. Get the transcripts, the videos, or both.**

[![CI](https://github.com/mal0ware/ytmerge/actions/workflows/ci.yml/badge.svg)](https://github.com/mal0ware/ytmerge/actions/workflows/ci.yml)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Linux%20%7C%20Windows-555)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

<img src="assets/screenshot.png" width="820" alt="The ytmerge web UI with three fetched videos">

</div>

ytmerge takes any text with YouTube links in it (a list, a chat log, your open browser tabs) and gives you back:

- the full transcript of each video, with timestamps and a metadata header, ready to paste into Claude or another LLM
- the video as an mp4, or just the audio
- one merged copy of every transcript, already on your clipboard

It runs on your own machine. Everything goes through [yt-dlp](https://github.com/yt-dlp/yt-dlp), which ytmerge downloads and keeps up to date for you.

There are two ways to use it: a **web page** on your computer (easiest, nothing to build) and a **command-line tool**.

## Web UI

```bash
git clone https://github.com/mal0ware/ytmerge.git
cd ytmerge
./ytmerge-web          # opens http://localhost:8765
```

You need `python3`. For video downloads you also need ffmpeg (`brew install ffmpeg deno` on a Mac). Transcripts and audio work without it.

On the page you can:

- paste links into the box, and fetching starts right away
- click **Grab my YouTube tabs** to pull in every YouTube video open in Opera GX, Chrome, Brave, Edge or Vivaldi
- copy, download or save each transcript, video or audio on its own, or all of them at once
- keep your list between visits, sorted by the day you added things, with your own named dividers and drag-to-reorder
- turn on **Markers** to pin videos for later, flag ones you haven't watched, and mark ones you're done with

To keep it running in the background on a Mac, run `./ytmerge-web install` (it starts at login; `./ytmerge-web uninstall` removes it). The server only listens on `127.0.0.1`, so nothing else on your network can use it.

## Command line

### Install

**macOS / Linux**

```bash
git clone https://github.com/mal0ware/ytmerge.git
cd ytmerge
./install.sh           # installs build deps, builds, copies ytmerge to ~/.local/bin
```

**Windows** (needs [MSYS2](https://www.msys2.org/): `winget install MSYS2.MSYS2`)

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

### Use

```bash
ytmerge                          # links from your clipboard
ytmerge links.txt                # links from a file
ytmerge "URL1" "URL2"            # links as arguments (quote them in zsh)
cat urls.txt | ytmerge -         # links from stdin
```

Each video ends up in `downloaded/` as `<title>.mp4` and `<title>.txt`, and all the transcripts land on your clipboard.

| Option | What it does |
| --- | --- |
| `--no-video` | transcripts only, no downloads |
| `--audio` / `--mp3` | audio only (mp3 converts it) |
| `--out DIR` | output folder (default `downloaded`) |
| `--workers N` | parallel downloads (default 4) |
| `--proxy URL` / `--proxy-list FILE` | send requests through a proxy, or rotate through a list |
| `--no-cache` | ignore cached transcripts |
| `--update` | update yt-dlp now |

Tip: bind `~/.local/bin/ytmerge` to a keyboard shortcut (Shortcuts.app on macOS, your desktop's custom shortcuts on Linux). Copy some links, press the keys, and the transcripts are on your clipboard.

## What a transcript looks like

```
================================================================================
TITLE:    The video's title
CHANNEL:  Channel Name
DATE:     2026-08-05
DURATION: 12:34
URL:      https://www.youtube.com/watch?v=...
CHAPTERS: [0:00] Intro | [2:15] Setup | [6:45] Main demo

DESCRIPTION:
The video description...
================================================================================

[0:00] Intro text here.
[0:45] More content.
```

## If YouTube blocks you

Fetch a lot in a short time and YouTube may rate-limit your IP (HTTP 429). ytmerge tells you when that happens. You can wait it out, switch networks (a phone hotspot works), or use proxies:

```bash
./refresh-proxies.sh                                   # fetch a free public proxy list
ytmerge --proxy-list ~/.cache/ytmerge/proxies.txt links.txt
```

Free proxies are hit and miss. A paid rotating proxy is more reliable.

Transcripts are cached in `~/.cache/ytmerge` for 7 days, so fetching the same video again costs nothing.

## License

MIT. See [LICENSE](LICENSE).
