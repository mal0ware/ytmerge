# ytmerge v1.0.0

First tagged release. ytmerge is a small cross-platform CLI that batches YouTube
transcripts into a single copy-pasteable blob: copy URLs, run one command (or press
one bound hotkey), paste merged transcripts.

## What's in this release

- **Three input sources**, in priority order: CLI args, stdin (`ytmerge -`), clipboard (default).
- **Forgiving URL parsing** — extracts YouTube links from messy text; dedupes; supports
  `watch`, `youtu.be`, `shorts`, `embed` URL forms.
- **Parallel fetching** (default 4 workers, `--workers N`) with deterministic output
  ordering that follows input ordering.
- **Proxy support**: `--proxy URL` for a single proxy, `--proxy-list FILE` for
  round-robin rotation with a 10-minute cooldown on HTTP 429. A bundled
  `refresh-proxies.sh` fetches free public proxies (unreliable by nature — see README).
- **Disk cache** at `~/.cache/ytmerge/<video_id>.txt`, 7-day eviction, `--no-cache` to skip.
- **Native clipboard I/O** on macOS, Linux (X11 via xclip / Wayland via wl-copy), and
  Windows (Win32 clipboard API). Desktop notifications on macOS/Linux; Windows prints
  status to stderr instead.
- **Explicit rate-limit reporting**: when YouTube 429s your IP, ytmerge says so and
  prints the realistic workarounds (wait, switch network, use proxies).

## Fixed in this release

- Windows compile failure: `windows.h`'s `min`/`max` macros (pulled in via
  `curl/curl.h`) broke `std::min`/`std::max`. `NOMINMAX` is now defined before all
  includes.

## Installation (compile from source)

This release is source-only; no prebuilt binaries are attached yet. CI
(`.github/workflows/ci.yml`) builds on ubuntu / macos / windows and uploads
per-platform build artifacts — prebuilt binaries can be attached to releases once
those CI runs are validated. Note the Windows CI artifact is a bare `ytmerge.exe`
that additionally needs the MSYS2 UCRT64 runtime DLLs; `install.ps1` handles that
locally by copying them next to the installed exe.

- **macOS / Linux:** `./install.sh` — installs deps (Homebrew / apt / dnf / pacman),
  builds with `make`, installs to `~/.local/bin`.
- **Windows:** `powershell -ExecutionPolicy Bypass -File .\install.ps1` — requires an
  existing [MSYS2](https://www.msys2.org/) install; pulls the UCRT64 toolchain +
  libcurl + nlohmann-json via pacman, builds with `make`, installs exe + runtime DLLs
  to `%LOCALAPPDATA%\ytmerge\bin` and adds it to your user PATH.
- **Manual:** see README for the five-minute MSYS2 walkthrough or plain `make` with
  libcurl + nlohmann-json headers available.

## Known limitations

- YouTube rate-limits its captions endpoint per source IP; sustained heavy use from
  one IP will hit HTTP 429. This is a YouTube-side ceiling, not a bug — proxy
  rotation and the documented workarounds are the supported mitigations.
- Free public proxy lists (via `refresh-proxies.sh`) are unreliable; typically only a
  fraction of entries work at any given time.
- No Windows toast notifications (status prints to stderr).

## License

MIT.
