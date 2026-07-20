# Ideas / WIP

Design notes for features that have been considered but not built. Captured here so the reasoning isn't lost.

---

## Local-transcription fallback (Whisper)

**Status:** idea only. Not built. See "Why it's not built yet" below.

### The problem

`youtube-transcript-api` and every other client (including `yt-dlp`) ultimately fetch caption data from `https://www.youtube.com/api/timedtext`. YouTube rate-limits this endpoint per source IP. When the limit trips, every video in the run fails with `IpBlocked` — currently surfaced as the misleading "no captions available."

There is no `Retry-After` header on the 429, no documented threshold, and no way to query "am I unblocked yet?" without making a request (which itself counts). Blocks lift in minutes to hours.

### The idea

Bypass the captions API entirely:

1. `yt-dlp` downloads audio-only. The audio CDN is a separate endpoint, not on the timedtext limiter — it's what every regular viewer hits.
2. `whisper.cpp` transcribes locally on the host machine.
3. Disk cache so repeated runs on the same video IDs cost nothing.

### Properties

- **No rate limits.** All work happens locally after the audio download.
- **Genuinely parallelizable.** Audio downloads in parallel against the CDN, plus N worker processes for transcription. Real wall-clock speedup, unlike parallelizing the captions API (which would just trip the block faster).
- **Often higher-quality transcripts** than YouTube's auto-generated captions, depending on model size.
- **Free, fully local, no accounts, no proxies.**

### Performance estimates (Apple Silicon)

| Whisper model | Size | Speed | 5-hr video |
|---|---|---|---|
| `tiny.en` | 75 MB | ~25-40x realtime | ~8-12 min |
| `base.en` | 150 MB | ~15-25x realtime | ~12-20 min |
| `small.en` | 500 MB | ~8-12x realtime | ~25-40 min |
| `medium.en` | 1.5 GB | ~3-6x realtime | ~50-100 min |
| `large-v3` | 3 GB | ~1-2x realtime | ~2.5-5 hrs |

For typical short content (5-20 min videos): 5-60 seconds per video with `base.en`. Batch of 30 short videos with 4 workers: ~1-3 minutes wall time.

### Dependencies the install script would need to add

- `ffmpeg` (Homebrew)
- `whisper-cpp` (Homebrew)
- `yt-dlp` (pip or Homebrew)
- One Whisper model file (~150 MB for `base.en`, ~3 GB for `large-v3`)

### Why it's not built yet

Top-tier accuracy on long videos requires `large-v3`, which runs at roughly 1:1 realtime — a 5-hour video would take 2.5-5 hours to transcribe locally. That defeats the point. The smaller models are fast enough but their accuracy isn't acceptable for the long-form use case this tool exists to serve.

The idea stays viable for a future variant scoped at short/medium videos where `base.en` or `small.en` accuracy is sufficient.

---

## Cookies-from-browser — tested, doesn't help

**Status:** tested negative. Do not pursue further unless something material changes.

Hypothesis was: authenticated requests get a higher rate-limit ceiling, so borrowing the user's browser cookies (without requiring any new sign-up) would lift the IP block.

Result: tested with a valid, authenticated YouTube browser session. yt-dlp accepted the cookies and got past the player API call. The subsequent fetch from `/api/timedtext` still returned HTTP 429. The rate limit is keyed on source IP, not auth state.

Conclusion: cookies cannot bypass this block. Documented here so this path isn't re-pitched in the future.

---

## Done in the C++ rewrite

These were originally on a "smaller improvements" list. All shipped:

1. **Distinct failure reasons** — `ip blocked (429)`, `video unavailable`, `no captions on video`, `no english track`, `network error`, `parse failed`. Completion notification and per-video log both use them.
2. **Disk cache** at `~/.cache/ytmerge/<id>.txt`, 7-day TTL eviction, `--no-cache` opt-out.
3. **`--proxy` flag and `--proxy-list` rotation** with per-proxy cooldown on 429. `HTTPS_PROXY` / `ALL_PROXY` env vars also picked up automatically via libcurl defaults.
4. **Parallelism** — configurable worker count (default 4). Output ordering is preserved regardless of completion order.

Not done (intentional):
- Backoff + jitter between fetches. With the proxy-rotation design, the right response to a 429 is "switch proxy," not "wait longer on this IP." Direct-connection users hit the limit once and stop (`max_attempts = 1` when pool is empty).
