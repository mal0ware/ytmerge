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

## Smaller improvements worth doing regardless

These don't depend on the Whisper fallback and would be wins even if it's never built:

1. **Surface the real failure reason.** Map `IpBlocked` / `RequestBlocked` / `TranscriptsDisabled` / `NoTranscriptFound` / `VideoUnavailable` to distinct labels in per-video logs and the completion notification. The current "no captions available" hides actionable causes.
2. **Disk cache for successful fetches** keyed by video ID. Re-running on the same URL set after a partial failure costs nothing.
3. **Opt-in `--proxy` flag** plus transparent `HTTPS_PROXY` env var support. Doesn't help users with nothing to point at, but gives an escape hatch when they do.
4. **Backoff + jitter between fetches** to reduce the probability of tripping the limit in the first place. Honest framing in the UI: "next attempt in Xs" (which we know), not "unblocked in X" (which we don't).
