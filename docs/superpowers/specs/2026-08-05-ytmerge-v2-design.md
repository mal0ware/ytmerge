# ytmerge v2 — yt-dlp engine, video + transcript output

**Date:** 2026-08-05
**Status:** approved design, pre-implementation

## Goal

Make ytmerge do everything yt-bat does, and more: links go into a text file, out
come the mp4 files **and** per-video transcript text documents (full metadata
header + timestamped transcript), built for feeding into AI. Keep the existing
clipboard/hotkey workflow. Beat yt-bat on speed via parallel downloads.

## Background

yt-bat (analyzed 2026-08-05, seven batch files wrapping bundled `yt-dlp.exe` +
ffmpeg) works because yt-dlp is battle-tested against YouTube's defenses.
ytmerge v1 hand-rolls captions scraping with libcurl and suffers IP blocks
(HTTP 429) on the timedtext endpoint — documented in `IDEAS.md`.

Ideas adopted from yt-bat: links-in-a-text-file workflow, self-updating yt-dlp,
zero-setup bundled/auto-fetched binaries, `--ignore-errors` per-video isolation,
`bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best` format selection.

Weaknesses fixed: sequential downloads, no text transcripts, discarded
metadata, update check blocking every run, Windows-only, no dedup/cache.

## Architecture

The existing C++20 binary remains the orchestrator. All YouTube interaction
moves to `yt-dlp` subprocesses. libcurl remains only for fetching the yt-dlp
binary itself (and is no longer used against youtube.com).

```
links.txt / clipboard / argv / stdin
        │
        ▼
URL extractor (existing: forgiving regex, dedup, order-preserving)
        │
        ▼
worker pool (existing, default 4) ── one yt-dlp subprocess per video ──► CDN
        │                                │
        │                                ├─ <title>.mp4        (video+audio)
        │                                ├─ <id>.info.json     (metadata)
        │                                └─ <id>.<lang>.json3  (subtitles)
        ▼
json3 → transcript converter (new, pure function, unit-tested)
        │
        ▼
composer: metadata header + timestamped transcript
        │
        ├─► downloaded/<title>.txt          (per video)
        ├─► clipboard (merged blob, clipboard mode)
        └─► cache ~/.cache/ytmerge/<id>.txt (existing, 7-day eviction)
```

## Components

### 1. Dependency manager (new: `src/deps.hpp`)

- Locates `yt-dlp` (and `ffmpeg`): first on `PATH`, else in
  `~/.cache/ytmerge/bin/`, else downloads the official release binary for the
  current OS/arch into that directory (libcurl, HTTPS, follows GitHub redirect).
- Update policy: at most once per 24h, non-blocking — the check runs after the
  batch completes, not before (yt-bat blocks startup on it; we don't).
  `ytmerge --update` forces it.
- ffmpeg: required only for merging bestvideo+bestaudio. If absent and not
  downloadable, fall back to `--format best[ext=mp4]/best` (single-file, no
  merge needed) and warn.

### 2. Subprocess runner (new: `src/proc.hpp`)

- Cross-platform spawn/wait/capture (CreateProcess on Windows, posix_spawn
  elsewhere). Captures stderr for error classification; stdout progress is
  parsed for per-video status lines.
- Timeout guard (default 30 min/video) so one hung download can't wedge a
  worker forever.

### 3. Fetch stage (replaces v1 scraper in `src/ytmerge.cpp`)

One yt-dlp invocation per video:

```
yt-dlp <URL>
  --no-playlist
  --output "downloaded/%(title)s.%(ext)s"
  --format "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best"
  --write-info-json
  --write-subs --write-auto-subs
  --sub-langs "en.*,en"
  --sub-format json3
  --no-progress --print-json
```

- `--no-video` mode drops the download to `--skip-download` (transcript+metadata
  only).
- Proxy flags pass through: `--proxy` maps to yt-dlp `--proxy`; `--proxy-list`
  keeps v1's rotation/cooldown at the orchestrator level (each retry picks the
  next proxy).
- Subtitle preference: manual subs over auto-captions when both exist.

### 4. json3 → transcript converter (new: `src/transcript.hpp`, pure)

- Input: json3 event list. Output: plain text with `[mm:ss]` (or `[h:mm:ss]`
  past one hour) markers.
- Handles auto-caption quirks: rolling duplicate fragments are merged; empty
  and formatting-only events dropped.
- Paragraphing: new timestamped paragraph at chapter boundaries (from
  info.json) when present, else every ~45 seconds of speech. No flat
  one-paragraph wall (v1 behavior) — timestamps are a requirement.

### 5. Composer (extends existing formatter in `src/core.hpp`)

Per-video document:

```
================================================================================
TITLE:    ...
CHANNEL:  ...
DATE:     YYYY-MM-DD
DURATION: h:mm:ss
VIEWS:    n        LIKES: n
URL:      https://www.youtube.com/watch?v=...
TAGS:     a, b, c
CHAPTERS: [0:00] Intro | [3:12] ...        (when present)

DESCRIPTION:
<full description>
================================================================================

[0:00] transcript...
```

- Written to `downloaded/<title>.txt` (same sanitized title stem yt-dlp used
  for the mp4, so pairs sort together).
- Clipboard mode concatenates these documents in input order (existing
  deterministic-ordering rule).

### 6. Modes and CLI

```
ytmerge                       # clipboard in → downloads + docs + merged blob to clipboard
ytmerge links.txt             # file in (any text file containing URLs)
ytmerge "URL1" "URL2"         # args
cat urls.txt | ytmerge -      # stdin
--no-video                    # transcripts + metadata only (fast path)
--audio / --mp3               # yt-bat parity presets (audio-only output)
--out DIR                     # output dir (default ./downloaded)
--workers N                   # default 4
--proxy URL / --proxy-list F  # unchanged from v1
--no-cache / --update / -h    # unchanged / new / unchanged
```

Default behavior in every mode: both mp4 and .txt ("always both" per user).
If a bare `links.txt` exists in the CWD and ytmerge is run with no args **and
the clipboard has no YouTube URLs**, the file is used — covers the
double-click-a-shortcut workflow without breaking clipboard mode.

## Error handling

- Per-video isolation: a failure marks that video FAILED and the batch
  continues (yt-bat's `--ignore-errors` spirit).
- stderr classification: private/deleted video, geo-block, 429/IP-block, no
  subtitles (video still downloads; doc gets metadata header + "no transcript
  available"), network timeout.
- End-of-run summary: `12 ok, 2 failed (private), 1 failed (rate-limited)` +
  v1's existing 429 hints (wait / change IP / proxy list).
- Cache: unchanged (7-day eviction). A cached transcript doesn't skip the mp4
  download; yt-dlp's own existing-file check skips completed videos.

## Testing

- Existing doctest suite continues to cover URL extraction, dedup, ordering,
  formatting.
- New unit tests: json3 parsing (fixture files for manual subs, auto-captions
  with rolling duplicates, empty events), timestamp formatting, metadata
  header composition, stderr classification.
- Subprocess and dependency-manager layers get thin integration tests gated
  behind an env var (not run in CI against real YouTube).
- CI: unchanged three-OS matrix.

## Non-goals

- Keyframe extraction, AI visual analysis (explicitly deferred by user).
- Whisper local-transcription fallback (stays in IDEAS.md).
- Keeping the v1 libcurl captions scraper as a fallback path (dual pipeline
  rejected — Approach C).

## Risks

- yt-dlp still hits the same timedtext endpoint for captions; hard IP blocks
  remain possible on very large batches. Mitigations unchanged: cache, proxy
  rotation, honest error messages.
- Video titles as filenames can collide or exceed path limits; yt-dlp's
  filename sanitization is used as-is. Two distinct videos with identical
  titles in one batch would collide — same limitation yt-bat has; accepted,
  documented in README. (`--windows-filenames` guards path-illegal characters.)
