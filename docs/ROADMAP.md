# Roadmap

Where ytmerge goes from "a small CLI that works" to "the obvious tool for turning
a pile of YouTube links into LLM-ready text." The current release is a single
binary that fetches and merges transcripts in parallel, with a proxy pool and a
disk cache. The pure logic now lives in `src/core.hpp` behind a unit-test suite,
and the build runs on a three-OS CI matrix — the foundation everything below
builds on.

Items are grouped by priority. Estimates are rough engineering effort for one
developer, not calendar time.

---

## Now — make it dependable (highest priority)

The single biggest weakness is that every transcript ultimately comes from
YouTube's `timedtext` endpoint, which is rate-limited per source IP. When that
trips, a whole batch fails at once. Removing that single point of failure is
worth more than any new feature.

### 1. whisper.cpp local-transcription fallback — **L (2-3 weeks)**

Already designed in [`IDEAS.md`](../IDEAS.md). When the captions API returns a
429 (or a video has no captions at all), fall back to a fully local path that is
not on the rate limiter:

1. `yt-dlp` downloads audio-only from the media CDN (a separate endpoint every
   ordinary viewer hits — not the `timedtext` limiter).
2. `whisper.cpp` transcribes locally.
3. The existing disk cache makes repeats free.

Properties: no rate limits, genuinely parallel (audio download + N transcribe
workers → real wall-clock speedup), often higher quality than YouTube's
auto-captions, free and account-free.

Scope decisions to make first:
- **Model default.** `base.en` (~150 MB, ~15-25x realtime) for the common
  short/medium-video case; expose `--whisper-model` for users who want
  `small.en`/`medium.en`. `large-v3` runs near 1:1 realtime, so document it as
  opt-in only — a 5-hour video would take 2.5-5 hours, which defeats the point
  for long-form content. This is exactly the caveat recorded in `IDEAS.md`.
- **Trigger policy.** `--fallback whisper` (explicit), `--fallback auto` (only
  after a 429 or a no-captions result), or `never` (default — preserves today's
  behavior). Auto keeps the fast captions path primary and only pays the local
  cost when the network path fails.
- **Dependency handling.** `ffmpeg`, `whisper-cpp`, and `yt-dlp` are external.
  Detect them at runtime; if missing, print one actionable install line rather
  than failing opaquely. `install.sh` gains an optional `--with-whisper` flag.

Testability: the orchestration (when to fall back, how to fan out audio vs.
transcribe work, cache interplay) is pure logic and belongs in `core.hpp`
behind unit tests. The subprocess calls stay in `ytmerge.cpp` behind the same
seam the clipboard/network code already uses.

### 2. Structured machine-readable output — **S (3-5 days)**

`--format json` / `--format jsonl` emits one record per video
(`{id, title, url, transcript, source, error}`) instead of the human-readable
blob. This is the unlock for every downstream integration below — piping into
`jq`, a script, or an LLM harness — and it's cheap because the data model
already exists internally. Keep the current pretty format as the default.

### 3. Per-request backoff hint surfacing — **XS (1-2 days)**

Today a 429 with no proxy pool stops after one attempt (correct). Add a
`Retry-After`-style note to the failure summary when present, and a
`--wait-on-block SECONDS` opt-in for users who'd rather sleep than switch
networks. Small, but it removes a common "why did everything fail" confusion.

---

## Next — distribution (so people can actually install it)

Right now installation is "clone and run `install.sh`" (or a five-minute MSYS2
dance on Windows). One-line installs on every platform dramatically lower the
barrier.

### 4. Homebrew tap — **S (2-4 days)**

`brew install mal0ware/tap/ytmerge` for macOS and Linuxbrew. A
`homebrew-tap` repo with a formula that builds from a release tarball via CMake,
declares `curl` + `nlohmann-json` (or uses the vendored copy), and runs
`ctest` in the formula's `test do` block. Cut a tagged release first so the
formula has a stable URL + sha256 to point at.

### 5. winget package — **S (2-4 days)**

`winget install ytmerge`. Needs a tagged release with a built `ytmerge.exe`
(the CI matrix already produces the Windows binary), a manifest submitted to
`microsoft/winget-pkgs`, and a documented build provenance. Pairs well with a
proper `install.ps1` so the from-source path on Windows stops being manual.

### 6. AUR package — **XS-S (1-3 days)**

`yay -S ytmerge` for Arch users. A `PKGBUILD` that builds via CMake and depends
on `curl`. Cheapest of the three packaging tracks because the AUR is just a git
repo with a build script; mostly a matter of getting `depends`/`makedepends`
right and adding a `check()` that runs the tests.

### 7. `install.ps1` for Windows — **S (2-3 days)**

The README admits there's no Windows installer yet. A PowerShell script that
installs a toolchain + libcurl (vcpkg or MSYS2), builds, and drops the binary on
`%PATH%` closes the most awkward gap in the current onboarding story.

---

## Later — workflow features

These multiply the tool's value once the dependability and distribution work is
done.

### 8. Watch-later / playlist batch mode — **M (1-2 weeks)**

Accept a playlist or channel URL and expand it to its member video IDs before
fetching (`ytmerge "https://youtube.com/playlist?list=..."`). The natural input
for the research-synthesis use case the tool exists to serve: "summarize
everything in this playlist." Watch-Later specifically requires authentication
(it's a private list), so scope that as a follow-on that reuses the
cookies-from-browser plumbing — noting `IDEAS.md`'s finding that cookies do
*not* lift the rate limit, but they *do* unlock private list membership, which
is a different problem.

### 9. LLM-workflow integrations — **M (1-2 weeks, depends on #2)**

With structured output in place:
- **`--summarize` passthrough.** Pipe the merged blob straight to a configured
  LLM and put the summary on the clipboard instead of (or alongside) the raw
  transcripts — collapsing the tool's whole reason-for-being into one command.
  Provider-agnostic: read an endpoint + key from env/config, ship no key.
- **Prompt templating.** `--prompt-prefix FILE` / `--prompt-suffix FILE` so the
  clipboard output is drop-in ready for a specific downstream workflow.
- **Token-aware chunking.** Split the merged output at model context limits with
  clean per-video boundaries, so a 30-video batch doesn't silently overflow.
- **MCP server mode.** Expose ytmerge as a Model Context Protocol tool so an
  agent can request transcripts directly. The pure `core.hpp` seam makes this a
  thin adapter rather than a rewrite.

### 10. Output sinks beyond the clipboard — **S (3-5 days)**

`--out FILE`, `--out -` (stdout), and per-video file splitting
(`--split-dir DIR`). The clipboard is the right default for the Shortcuts.app
flow, but file/stdout sinks make ytmerge composable in larger pipelines.

---

## Ongoing — engineering health

- **Grow the test suite as features land.** Every new pure function goes through
  `core.hpp` with fixtures, network-free, the way the existing scanner/parser
  functions do.
- **Lint + format gate in CI.** Add `clang-format` and `clang-tidy` checks to
  the matrix so style stays consistent without manual policing.
- **Sanitizer build.** An ASan/UBSan job over the test target catches
  memory/UB regressions early — cheap to add to the existing CI.
- **Release automation.** A tag-triggered workflow that builds all three OS
  binaries, runs the tests, and attaches artifacts — the prerequisite the
  Homebrew/winget/AUR tracks all depend on.
