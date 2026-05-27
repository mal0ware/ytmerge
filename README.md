# ytmerge

Copy YouTube URLs → press one keyboard shortcut → paste merged transcripts wherever you want them.

A small cross-platform CLI (macOS, Linux, Windows) for batching YouTube transcripts into a single copy-pasteable blob. Built for research-synthesis workflows — feeding many videos at once into Claude (or anything else) without manually opening each one.

Written in C++ (libcurl + nlohmann/json). Parallel by default. Rotates through a proxy pool when given one, so multi-IP setups can dodge YouTube's per-IP rate limit.

## Why

Watching 15 videos to extract a few useful threads is slow. Pasting transcripts one at a time into an LLM is also slow. `ytmerge` collapses the in-between step into a single keyboard shortcut:

1. Copy a batch of URLs from anywhere — browser tabs, notes, chat messages, scratchpad. Messy text is fine.
2. Press your bound shortcut.
3. Paste the merged transcripts wherever you're going to use them.

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

There's no `install.ps1` yet; the manual path takes about five minutes.

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
   Output: `ytmerge.exe`. Copy it anywhere on `%PATH%` (e.g. `~/AppData/Local/Microsoft/WindowsApps/`) or call it from its build location.

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
--proxy URL          single proxy for all requests
--proxy-list FILE    one proxy per line; round-robin with cooldown on 429
--workers N          parallel workers (default 4)
--no-cache           skip the disk cache for this run
-h, --help           usage
```

## Rate limits and proxies

YouTube rate-limits its captions endpoint per source IP. From one home IP you can typically fetch dozens of transcripts without issue, but enough repeated runs in a short window will trigger HTTP 429. When that happens `ytmerge` reports it explicitly:

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

Each video becomes a header block followed by its transcript:

```
================================================================================
TITLE: The video's actual title
URL:   https://www.youtube.com/watch?v=...
================================================================================

[full transcript as one paragraph]
```

Multiple videos are separated by blank lines. Clear delimiters help downstream LLMs attribute insights to the right source instead of blurring everything together.

## Notes

- URL parser is forgiving. Paste anything containing YouTube links and it'll extract them. Duplicates are deduped. Supports `youtube.com/watch`, `youtu.be`, `youtube.com/shorts`, `youtube.com/embed`.
- Output ordering follows input ordering (deterministic, independent of which worker finishes first).
- The C++ binary picks up `HTTPS_PROXY` / `ALL_PROXY` env vars automatically (libcurl default behavior) — `--proxy` overrides them.
- **Platform support:** macOS, Linux (X11 via `xclip` or Wayland via `wl-copy`), and Windows (Win32 clipboard API, no native toast). Clipboard and notification are the only OS-specific surfaces; everything else (HTTP, parsing, threading, cache) is plain C++20.

## Known limitations

**YouTube rate-limits the transcript endpoint, and that is the single biggest constraint on this tool.** `youtube-transcript-api` scrapes the same endpoint the YouTube web player uses, and YouTube has been progressively more aggressive about throttling and IP-blocking unauthenticated requests. In practice:

- Small batches (a handful of videos) usually go through cleanly.
- Larger batches start failing partway through with empty or blocked responses, even on videos that demonstrably have captions.
- Running `ytmerge` repeatedly within a short window can trip a soft block where subsequent calls return nothing until the cooldown expires.

The original design assumption was "paste fifteen URLs and get back fifteen transcripts in one keystroke." That assumption no longer holds reliably, and it's the reason this tool stayed a personal utility rather than something I'd hand to other people. None of the realistic workarounds belong inside the script itself:

- A residential or rotating proxy changes how `ytmerge` is invoked, not what it does.
- The official YouTube Data API v3 with `captions.download` requires OAuth and the video owner's permission — useless for arbitrary public URLs.
- A paid transcription service (AssemblyAI, Whisper API, etc.) is a different category of tool at a different cost.

Within those limits, `ytmerge` still earns its keep for small batches where the keyboard-shortcut workflow saves real time. Treat it as a "grab a few videos quickly" tool, not a "synthesize twenty videos at once" tool.

## License

MIT — see [LICENSE](LICENSE).
