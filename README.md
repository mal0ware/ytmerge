# ytmerge

Links in a text file → mp4s and AI-ready transcript documents out. One command.

A small cross-platform CLI (macOS, Linux, Windows) that batch-fetches YouTube
videos **and** their transcripts. For every URL it produces:

- `downloaded/<title>.mp4` — the video (yt-dlp, best mp4+m4a)
- `downloaded/<title>.txt` — full metadata header (title, channel, date,
  duration, views, tags, chapters, description) + the complete transcript
  with `[mm:ss]` timestamps

All transcripts are also merged onto the clipboard, ready to paste into
Claude or any other LLM. All YouTube interaction goes through
[yt-dlp](https://github.com/yt-dlp/yt-dlp) (auto-downloaded on first run,
self-updating daily), so ytmerge inherits its resilience to YouTube changes.
Videos download in parallel (4 workers by default).

## Quick start

```bash
# put any text containing YouTube links into links.txt, then:
ytmerge links.txt
# or just `ytmerge` — clipboard URLs, falling back to ./links.txt
```

## Install

### macOS and Linux

```bash
git clone https://github.com/mal0ware/ytmerge.git
cd ytmerge
./install.sh
```

`install.sh` detects your OS and installs the right deps:

- **macOS:** Homebrew + Xcode Command Line Tools; pulls `nlohmann-json` via brew; libcurl from the SDK.
- **Linux (apt/dnf/pacman):** installs `build-essential`/`gcc-c++`, `libcurl-dev`, `nlohmann-json3-dev`, plus `xclip` + `wl-clipboard` + `libnotify` for clipboard and toast notifications.

Then it builds with `make` and copies `ytmerge` to `~/.local/bin/`. Ensure that's on your `PATH`:

```bash
# in ~/.zshrc or ~/.bashrc
export PATH="$HOME/.local/bin:$PATH"
```

### Windows (MSYS2 + mingw-w64)

With [MSYS2](https://www.msys2.org/) already installed (`winget install MSYS2.MSYS2`),
`install.ps1` does everything below automatically — pacman deps, `make`, and an
install to `%LOCALAPPDATA%\ytmerge\bin` (added to your user PATH), with the required
MSYS2 runtime DLLs copied next to the exe so it runs from any shell:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

The manual path takes about five minutes:

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 UCRT64** shell.
2. Pull the toolchain and libraries:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-toolchain \
                       mingw-w64-ucrt-x86_64-curl \
                       mingw-w64-ucrt-x86_64-nlohmann-json \
                       make pkgconf
   ```
3. From the same shell, in the cloned repo:
   ```bash
   make
   ```
   Output: `ytmerge.exe`. Copy it anywhere on `%PATH%` (e.g. `~/AppData/Local/Microsoft/WindowsApps/`) or call it from its build location. Note: the exe depends on MSYS2 UCRT64 runtime DLLs (`libcurl-4.dll`, `libstdc++-6.dll`, ...), so outside an MSYS2 shell you must copy those DLLs next to it (`ldd ytmerge.exe` lists them) — `install.ps1` does this for you.

Windows uses the Win32 clipboard API directly (no PowerShell shell-out) and skips desktop notifications — completion status prints to stderr instead.

## Quick test

Copy any YouTube URL to your clipboard, then run:

```bash
ytmerge
```

You'll see per-video progress in the terminal, get a macOS notification when it's done, and your clipboard will be replaced with the merged transcripts.

## Three ways to feed it URLs

```bash
ytmerge                              # 1. clipboard (default — best for the Shortcuts.app flow)
ytmerge "URL1" "URL2" "URL3"         # 2. CLI args
cat urls.txt | ytmerge -             # 3. stdin (the `-` is required)
```

**Important for zsh users:** always quote URLs on the command line. The `?` in `?v=...` is a glob character, and an unquoted URL will produce `zsh: no matches found`.

## Options

```
--no-video           transcripts + metadata only (no download)
--audio              audio-only download (best available format)
--mp3                audio-only download, converted to mp3
--out DIR            output directory (default: downloaded)
--workers N          parallel workers (default 4)
--proxy URL          single proxy for all requests
--proxy-list FILE    one proxy per line; round-robin with cooldown on 429
--no-cache           skip disk cache for this run
--update             force a yt-dlp self-update check now
-h, --help           this message
```

## Rate limits and proxies

YouTube rate-limits its captions endpoint per source IP. All caption fetches go through [yt-dlp](https://github.com/yt-dlp/yt-dlp) now, which inherits YouTube's resilience patterns — but the per-IP 429 ceiling on the timedtext endpoint still exists. From one home IP you can typically fetch dozens of transcripts without issue, but enough repeated runs in a short window will trigger HTTP 429. When that happens `ytmerge` reports it explicitly:

```
[1/1] InH25PzMqpk  FAIL: ip blocked (429)
all 1 failed — 1 ip blocked (429)
  hint: YouTube has rate-limited this IP. Either:
    - switch network (phone hotspot, VPN, different wifi)
    - wait a few hours
    - re-run with --proxy URL or --proxy-list FILE
```

Three honest options to work around it:

1. **Wait** — blocks clear in minutes to a few hours.
2. **Change source IP** — phone hotspot for 30 seconds, a VPN you already have, or a different network.
3. **Use a proxy list.** Write one HTTP/SOCKS proxy URL per line in a text file (lines starting with `#` are ignored), then pass `--proxy-list /path/to/proxies.txt`. Workers cycle through the list, and on a 429 a proxy is cooled down for 10 minutes before being tried again.

### Quick start with free public proxies

A bundled helper grabs ~100 fresh entries from a public list and writes them to `~/.cache/ytmerge/proxies.txt`:

```bash
./refresh-proxies.sh                 # writes ~100 proxies to ~/.cache/ytmerge/proxies.txt
ytmerge --proxy-list ~/.cache/ytmerge/proxies.txt "URL1" "URL2"
```

Reality check: free public proxies are unreliable by nature. Most will be dead, slow, or blocked by Google. Only a fraction (often 5–30% of a fresh list) will actually reach YouTube. The rotation + cooldown logic is designed for exactly that — try one, on failure cool it down, try the next. Re-run `refresh-proxies.sh` periodically; lists go stale within hours. Also be aware that public proxies are untrusted infrastructure and can observe request URLs (though HTTPS protects the content, and `ytmerge` sends no credentials).

### Other proxy sources

- **Webshare** (~$1/mo, residential, reliable): they give you a single endpoint URL that internally rotates IPs. Drop that one URL into your proxy list and you're done.
- **Your VPN provider's proxy endpoints** if it exposes SOCKS (Mullvad, ProtonVPN, etc.).

Cookies-from-browser was considered and tested — it does *not* lift this particular rate limit. The 429 keys on IP regardless of authentication state.

## Cache

Successful transcripts are cached at `~/.cache/ytmerge/<video_id>.txt` and evicted after 7 days. Re-running on the same URLs is free. `--no-cache` skips it. Nuke with `rm -rf ~/.cache/ytmerge`.

## Bind to a global keyboard shortcut

### macOS (Shortcuts.app)

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

### Linux

Use your desktop environment's keyboard settings:
- **GNOME:** Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts → add a binding with command `~/.local/bin/ytmerge`.
- **KDE Plasma:** System Settings → Shortcuts → Custom Shortcuts → New → Global Shortcut → Command/URL.
- **Window managers (i3/sway/Hyprland/etc.):** add a `bindsym` / `bind` entry pointing at the binary.

### Windows

Two approaches:
- **PowerToys "Keyboard Manager"** + a small `.cmd` wrapper that calls `ytmerge.exe`.
- **AutoHotkey** v2 with a hotkey that runs `Run "ytmerge.exe"`.

## Output format

Each video produces `downloaded/<title>.txt` with a full metadata header followed by a timestamped transcript:

```
================================================================================
TITLE:    The video's actual title
CHANNEL:  Channel Name
DATE:     2026-08-05
DURATION: 12:34
VIEWS:    45000        LIKES: 1200
URL:      https://www.youtube.com/watch?v=...
TAGS:     python, tutorial, coding
CHAPTERS: [0:00] Intro | [2:15] Setup | [6:45] Main demo

DESCRIPTION:
Full video description text here...
================================================================================

[0:00] Intro text here.
[0:45] More content.
[2:15] Setup section begins...
```

When using clipboard mode (default), all transcripts from the batch are merged and placed on the clipboard, each separated by a blank line. Clear delimiters help downstream LLMs attribute insights to the right source.

## Notes

- URL parser is forgiving. Paste anything containing YouTube links and it'll extract them. Duplicates are deduped. Supports `youtube.com/watch`, `youtu.be`, `youtube.com/shorts`, `youtube.com/embed`.
- Output ordering follows input ordering (deterministic, independent of which worker finishes first).
- Both the C++ binary (for downloading yt-dlp) and yt-dlp itself (for fetches) pick up `HTTPS_PROXY` / `ALL_PROXY` env vars automatically — `--proxy` overrides them.
- **Platform support:** macOS, Linux (X11 via `xclip` or Wayland via `wl-copy`), and Windows (Win32 clipboard API, no native toast). Clipboard and notification are the only OS-specific surfaces; everything else (HTTP, parsing, threading, cache) is plain C++20.

## License

MIT — see [LICENSE](LICENSE).
