#!/usr/bin/env python3
"""ytmerge web — a local browser UI for ytmerge.

Standard library only: no pip install, no compiler. Serves web/index.html on
http://localhost:8765 and drives yt-dlp exactly like the C++ CLI does (same
commands, same transcript document format, same ~/.cache/ytmerge cache), so
the two front-ends share cached transcripts and the yt-dlp binary.

Run it through the ./ytmerge-web launcher, or directly:  python3 web/server.py
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse

import browsers

DEFAULT_PORT = 8765
CACHE_TTL_DAYS = 7
MEDIA_TTL_HOURS = 24
WORKERS = 4
WEB_DIR = Path(__file__).resolve().parent

FORMAT_MERGED = "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best"
FORMAT_SINGLE = "best[ext=mp4]/best"
FORMAT_AUDIO = "bestaudio[ext=m4a]/bestaudio"  # no ffmpeg needed

ID_RE = re.compile(r"(?:v=|youtu\.be/|/embed/|/shorts/|/live/)([A-Za-z0-9_-]{11})")
VALID_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")
PROGRESS_RE = re.compile(r"^PROG\s+([\d.]+)%")
DIV = "=" * 80


# ─── Pure logic (mirrors src/core.hpp, transcript.hpp, doc.hpp) ─────────────

def extract_video_ids(text: str) -> list[str]:
    """Every YouTube video id in arbitrary text, deduped, first-seen order."""
    seen: dict[str, None] = {}
    for m in ID_RE.finditer(text):
        seen.setdefault(m.group(1), None)
    return list(seen)


def parse_json3_cues(doc: dict) -> list[tuple[float, str]]:
    """yt-dlp json3 subtitles -> [(start_s, text)], dropping empty cues and
    consecutive duplicates (auto-caption rolling windows)."""
    cues: list[tuple[float, str]] = []
    for ev in doc.get("events") or []:
        segs, start = ev.get("segs"), ev.get("tStartMs")
        if not isinstance(segs, list) or not isinstance(start, (int, float)):
            continue
        raw = "".join(s.get("utf8", "") for s in segs if isinstance(s.get("utf8"), str))
        text = re.sub(r" {2,}", " ", re.sub(r"[\r\n\t]", " ", raw)).strip(" ")
        if not text or (cues and cues[-1][1] == text):
            continue
        cues.append((start / 1000.0, text))
    return cues


def format_timestamp(seconds: float) -> str:
    s = int(seconds)
    h, m, sec = s // 3600, (s % 3600) // 60, s % 60
    return f"[{h}:{m:02d}:{sec:02d}]" if h else f"[{m}:{sec:02d}]"


def format_duration(s: int) -> str:
    h, m, sec = s // 3600, (s % 3600) // 60, s % 60
    return f"{h}:{m:02d}:{sec:02d}" if h else f"{m}:{sec:02d}"


def render_transcript(cues, chapters, paragraph_interval_s: float = 45.0) -> str:
    """Cues joined into paragraphs; a new timestamped paragraph starts at each
    chapter boundary and every paragraph_interval_s seconds of speech."""
    out: list[str] = []
    next_ch, para_start, first = 0, 0.0, True
    for start, text in cues:
        brk = first or (start - para_start >= paragraph_interval_s)
        while next_ch < len(chapters) and start >= chapters[next_ch][0] - 0.001:
            if not first:
                brk = True
            next_ch += 1
        if brk:
            if not first:
                out.append("\n\n")
            out.append(format_timestamp(start) + " ")
            para_start, first = start, False
        else:
            out.append(" ")
        out.append(text)
    return "".join(out)


def _s(info: dict, k: str) -> str:
    v = info.get(k)
    return v if isinstance(v, str) else ""


def _i(info: dict, k: str, default: int) -> int:
    v = info.get(k)
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else default


def info_chapters(info: dict) -> list[tuple[float, str]]:
    return [(float(c.get("start_time") or 0.0), _s(c, "title"))
            for c in info.get("chapters") or [] if isinstance(c, dict)]


def compose_document(info: dict, transcript_text: str) -> str:
    """The per-video text document — byte-compatible with doc::compose_document."""
    vid = _s(info, "id")
    lines = [DIV, f"TITLE:    {_s(info, 'title')}",
             f"CHANNEL:  {_s(info, 'channel') or _s(info, 'uploader')}"]
    d = _s(info, "upload_date")
    if len(d) == 8:
        lines.append(f"DATE:     {d[:4]}-{d[4:6]}-{d[6:]}")
    dur = _i(info, "duration", 0)
    if dur > 0:
        lines.append(f"DURATION: {format_duration(dur)}")
    views, likes = _i(info, "view_count", -1), _i(info, "like_count", -1)
    if views >= 0:
        lines.append(f"VIEWS:    {views}" + (f"    LIKES: {likes}" if likes >= 0 else ""))
    lines.append(f"URL:      https://www.youtube.com/watch?v={vid}")
    tags = [t for t in info.get("tags") or [] if isinstance(t, str)]
    if tags:
        lines.append("TAGS:     " + ", ".join(tags))
    chapters = info_chapters(info)
    if chapters:
        lines.append("CHAPTERS: " + " | ".join(f"{format_timestamp(t)} {n}" for t, n in chapters))
    desc = _s(info, "description")
    if desc:
        lines += ["", "DESCRIPTION:", desc]
    lines += [DIV, ""]
    return "\n".join(lines) + "\n" + (transcript_text or "(no transcript available)") + "\n"


def parse_header(document: str) -> dict:
    """Pull the display fields back out of a composed document (cache hits)."""
    head = document.split(DIV + "\n\n", 1)[0]
    fields = {}
    for key in ("TITLE", "CHANNEL", "DATE", "DURATION"):
        m = re.search(rf"^{key}:\s+(.*)$", head, re.M)
        fields[key.lower()] = m.group(1) if m else ""
    fields["has_transcript"] = not document.rstrip("\n").endswith("(no transcript available)")
    return fields


def classify_stderr(e: str) -> str:
    if "HTTP Error 429" in e or "Too Many Requests" in e:
        return "rate-limited by YouTube (429) — switch network or wait"
    if "Sign in to confirm" in e:
        return "YouTube wants a sign-in check (bot detection) — try another network"
    for s in ("Private video", "Video unavailable", "This video is unavailable",
              "members-only", "removed by the uploader", "is not a valid URL",
              "Unsupported URL", "not available in your country"):
        if s in e:
            return "video unavailable"
    last = [ln for ln in e.strip().splitlines() if ln.strip()]
    return ("network error: " + last[-1][:200]) if last else "network error"


# ─── Paths, cache, yt-dlp bootstrap ─────────────────────────────────────────

def cache_dir() -> Path:
    xdg = os.environ.get("XDG_CACHE_HOME")
    return (Path(xdg) if xdg else Path.home() / ".cache") / "ytmerge"


def cache_read_doc(vid: str) -> str | None:
    p = cache_dir() / f"{vid}.txt"
    try:
        if time.time() - p.stat().st_mtime > CACHE_TTL_DAYS * 86400:
            p.unlink(missing_ok=True)
            return None
        doc = p.read_text(encoding="utf-8")
    except OSError:
        return None
    return doc if "TITLE:    " in doc else None  # v1-format entries are refetched


def cache_write_doc(vid: str, doc: str) -> None:
    try:
        cache_dir().mkdir(parents=True, exist_ok=True)
        tmp = cache_dir() / f".{vid}.txt.tmp"
        tmp.write_text(doc, encoding="utf-8")
        tmp.replace(cache_dir() / f"{vid}.txt")
    except OSError:
        pass


def ytdlp_asset() -> str:
    if sys.platform == "darwin":
        return "yt-dlp_macos"
    if sys.platform.startswith("win"):
        return "yt-dlp.exe"
    return "yt-dlp_linux"


def ensure_ytdlp(log=print) -> str:
    """Prefer yt-dlp on PATH (e.g. brew), else the shared ~/.cache/ytmerge/bin
    copy, downloading the standalone build with curl on first run."""
    on_path = shutil.which("yt-dlp")
    if on_path:
        return on_path
    local = cache_dir() / "bin" / ("yt-dlp.exe" if sys.platform.startswith("win") else "yt-dlp")
    if local.is_file():
        return str(local)
    local.parent.mkdir(parents=True, exist_ok=True)
    url = f"https://github.com/yt-dlp/yt-dlp/releases/latest/download/{ytdlp_asset()}"
    log(f"yt-dlp not found — downloading {url}")
    tmp = local.with_suffix(".part")
    # curl rather than urllib: python.org builds on macOS often lack CA certs.
    r = subprocess.run(["curl", "-fsSL", "--retry", "2", "-o", str(tmp), url])
    if r.returncode != 0 or not tmp.is_file() or tmp.stat().st_size < 1_000_000:
        tmp.unlink(missing_ok=True)
        raise RuntimeError("could not download yt-dlp (no network?). "
                           "Install it yourself with: brew install yt-dlp")
    tmp.chmod(0o755)
    tmp.replace(local)
    (local.parent / ".last-update-check").touch()
    return str(local)


def maybe_self_update(ytdlp: str) -> None:
    """Daily `yt-dlp -U` for our own copy (a brew install updates via brew)."""
    if Path(ytdlp).parent != cache_dir() / "bin":
        return
    stamp = cache_dir() / "bin" / ".last-update-check"
    try:
        if time.time() - stamp.stat().st_mtime < 86400:
            return
    except OSError:
        pass
    subprocess.run([ytdlp, "-U"], capture_output=True, timeout=300)
    stamp.touch()


def clean_old_media(media_root: Path) -> None:
    cutoff = time.time() - MEDIA_TTL_HOURS * 3600
    for p in media_root.rglob("*"):
        try:
            if p.is_file() and p.stat().st_mtime < cutoff:
                p.unlink()
        except OSError:
            pass


# ─── Jobs ───────────────────────────────────────────────────────────────────

class App:
    def __init__(self, ytdlp: str):
        self.ytdlp = ytdlp
        self.lock = threading.Lock()
        self.videos: dict[str, dict] = {}
        self.downloads: dict[str, dict] = {}  # key "<id>:<kind>"
        self.meta_pool = ThreadPoolExecutor(WORKERS, thread_name_prefix="meta")
        self.dl_pool = ThreadPoolExecutor(WORKERS, thread_name_prefix="dl")
        self.media_root = cache_dir() / "media"
        self.media_root.mkdir(parents=True, exist_ok=True)
        clean_old_media(self.media_root)

    @property
    def has_ffmpeg(self) -> bool:
        # Checked live so `brew install ffmpeg` takes effect without a restart.
        return shutil.which("ffmpeg") is not None

    # transcripts ------------------------------------------------------------

    def queue_videos(self, ids: list[str], refresh: bool = False) -> None:
        with self.lock:
            for vid in ids:
                cur = self.videos.get(vid)
                if cur and not refresh and cur["status"] in ("queued", "fetching", "done"):
                    continue
                self.videos[vid] = {"id": vid, "status": "queued"}
                self.meta_pool.submit(self._fetch, vid, refresh)

    def _set(self, vid: str, **kw) -> None:
        with self.lock:
            self.videos[vid].update(kw)

    def _fetch(self, vid: str, refresh: bool) -> None:
        doc = None if refresh else cache_read_doc(vid)
        if doc is None:
            self._set(vid, status="fetching")
            try:
                doc = self._fetch_doc(vid)
            except Exception as e:  # noqa: BLE001 — surfaced to the UI
                self._set(vid, status="error", error=str(e))
                return
            cache_write_doc(vid, doc)
        self._set(vid, status="done", document=doc, **parse_header(doc))

    def _fetch_doc(self, vid: str) -> str:
        work_root = cache_dir() / "work"
        work_root.mkdir(parents=True, exist_ok=True)
        workdir = Path(tempfile.mkdtemp(prefix=f"{vid}-", dir=work_root))
        try:
            cmd = [self.ytdlp, f"https://www.youtube.com/watch?v={vid}",
                   "--no-playlist", "--no-progress", "--no-warnings", "--skip-download",
                   "--write-info-json", "--write-subs", "--write-auto-subs",
                   # Original English tracks only: "en.*" also matches dozens of
                   # auto-translations (en-de, ...), each a request that can 429.
                   "--sub-langs", "en,en-orig,en-US,en-GB", "--sub-format", "json3",
                   # A failed subtitle fetch must not cost us the metadata.
                   "--ignore-errors",
                   "--paths", str(workdir), "--output", "%(id)s.%(ext)s"]
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired:
                raise RuntimeError("timed out") from None
            info_path = workdir / f"{vid}.info.json"
            if not info_path.is_file():
                raise RuntimeError(classify_stderr(r.stderr))
            info = json.loads(info_path.read_text(encoding="utf-8"))
            subs = sorted(workdir.glob(f"{vid}.*.json3"),
                          key=lambda p: (p.name != f"{vid}.en.json3",
                                         p.name != f"{vid}.en-orig.json3", p.name))
            text = ""
            if subs:
                try:
                    cues = parse_json3_cues(json.loads(subs[0].read_text(encoding="utf-8")))
                    text = render_transcript(cues, info_chapters(info))
                except (OSError, ValueError):
                    text = ""
            return compose_document(info, text)
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    # downloads --------------------------------------------------------------

    def queue_download(self, vid: str, kind: str) -> dict:
        key = f"{vid}:{kind}"
        with self.lock:
            cur = self.downloads.get(key)
            if cur and (cur["status"] in ("queued", "downloading")
                        or (cur["status"] == "done" and Path(cur["path"]).is_file())):
                return dict(cur)
            self.downloads[key] = {"id": vid, "kind": kind, "status": "queued",
                                   "progress": 0.0, "stage": ""}
            self.dl_pool.submit(self._download, vid, kind)
            return dict(self.downloads[key])

    def _dl_set(self, key: str, **kw) -> None:
        with self.lock:
            self.downloads[key].update(kw)

    def _download(self, vid: str, kind: str) -> None:
        key = f"{vid}:{kind}"
        outdir = self.media_root / kind
        outdir.mkdir(parents=True, exist_ok=True)
        ffmpeg = self.has_ffmpeg
        if kind == "audio":
            fmt = FORMAT_AUDIO
        else:
            fmt = FORMAT_MERGED if ffmpeg else FORMAT_SINGLE
        cmd = [self.ytdlp, f"https://www.youtube.com/watch?v={vid}",
               "--no-playlist", "--no-warnings", "--newline", "--progress",
               "--progress-template", "download:PROG %(progress._percent_str)s",
               "--windows-filenames", "--format", fmt,
               "--paths", str(outdir), "--output", "%(title)s [%(id)s].%(ext)s",
               "--print", "after_move:FILE %(filepath)s", "--no-simulate"]
        if kind == "video" and ffmpeg:
            cmd += ["--merge-output-format", "mp4"]
        self._dl_set(key, status="downloading", stage="starting")
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, encoding="utf-8", errors="replace")
        except OSError as e:
            self._dl_set(key, status="error", error=str(e))
            return
        err_chunks: list[str] = []
        t = threading.Thread(target=lambda: err_chunks.append(p.stderr.read()), daemon=True)
        t.start()
        path, stream, last = "", 1, 0.0
        for line in p.stdout:
            line = line.strip()
            m = PROGRESS_RE.match(line)
            if m:
                pct = float(m.group(1))
                if pct + 30 < last:  # second stream (audio) of a merged download
                    stream += 1
                last = pct
                self._dl_set(key, progress=pct,
                             stage=("audio" if stream > 1 else kind) if kind == "video" else "audio")
                if pct >= 100 and kind == "video" and ffmpeg and stream > 1:
                    self._dl_set(key, stage="merging")
            elif line.startswith("FILE "):
                path = line[5:]
        p.wait()
        t.join(timeout=5)
        if p.returncode == 0 and path and Path(path).is_file():
            self._dl_set(key, status="done", progress=100.0, stage="done", path=path,
                         filename=Path(path).name, size=Path(path).stat().st_size)
        else:
            err = "".join(err_chunks)
            msg = classify_stderr(err)
            if kind == "video" and not ffmpeg and "Requested format is not available" in err:
                # YouTube mostly serves separate video/audio streams now; joining
                # them needs ffmpeg.
                msg = "video needs ffmpeg — run: brew install ffmpeg (no restart needed)"
            self._dl_set(key, status="error", error=msg)

    # views ------------------------------------------------------------------

    def snapshot(self, ids: list[str]) -> dict:
        with self.lock:
            vids = [dict(self.videos.get(v, {"id": v, "status": "unknown"})) for v in ids]
            dls = {k: {kk: vv for kk, vv in d.items() if kk != "path"}
                   for k, d in self.downloads.items() if d["id"] in ids}
        return {"videos": vids, "downloads": dls}


# ─── HTTP ───────────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    app: App
    port: int
    server_version = "ytmerge-web"

    def log_message(self, fmt, *args):  # quiet: the terminal stays readable
        pass

    def _host_ok(self) -> bool:
        # Bound to 127.0.0.1, but also reject foreign Host/Origin headers so a
        # random website can't drive this server via DNS rebinding or CSRF.
        allowed = {f"localhost:{self.port}", f"127.0.0.1:{self.port}"}
        if self.headers.get("Host", "") not in allowed:
            return False
        origin = self.headers.get("Origin")
        return origin is None or urlparse(origin).netloc in allowed

    def _json(self, obj, code: int = 200) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n > 1_000_000:
            return {}
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        if not self._host_ok():
            return self.send_error(403)
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path in ("/", "/index.html"):
            body = (WEB_DIR / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/api/status":
            ids = [i for i in q.get("ids", [""])[0].split(",") if VALID_ID.match(i)]
            snap = self.app.snapshot(ids)
            snap["ffmpeg"] = self.app.has_ffmpeg
            self._json(snap)
        elif u.path == "/api/file":
            self._send_file(q.get("id", [""])[0], q.get("kind", [""])[0])
        else:
            self.send_error(404)

    def do_POST(self):
        if not self._host_ok():
            return self.send_error(403)
        u = urlparse(self.path)
        b = self._body()
        if u.path == "/api/videos":
            ids = extract_video_ids(str(b.get("text", "")))
            self.app.queue_videos(ids, refresh=bool(b.get("refresh")))
            self._json({"ids": ids})
        elif u.path == "/api/tabs":
            browser = str(b.get("browser") or "auto")
            if browser not in ("auto", *browsers.BROWSERS):
                return self._json({"error": "unknown browser"}, 400)
            try:
                name, urls, as_of = browsers.open_tab_urls(browser)
            except (LookupError, OSError) as e:
                return self._json({"error": str(e)}, 404)
            # asOf (epoch seconds) is set when the live session was locked and
            # an older snapshot was read instead; the page shows its time.
            self._json({"browser": name, "ids": extract_video_ids(" ".join(urls)), "asOf": as_of})
        elif u.path == "/api/download":
            vid, kind = str(b.get("id", "")), str(b.get("kind", ""))
            if not VALID_ID.match(vid) or kind not in ("video", "audio"):
                return self._json({"error": "bad request"}, 400)
            self._json(self.app.queue_download(vid, kind))
        else:
            self.send_error(404)

    def _send_file(self, vid: str, kind: str) -> None:
        with self.app.lock:
            d = dict(self.app.downloads.get(f"{vid}:{kind}", {}))
        path = Path(d.get("path", ""))
        if d.get("status") != "done" or not path.is_file():
            return self.send_error(404)
        name = path.name
        ascii_name = name.encode("ascii", "replace").decode().replace('"', "'")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Content-Disposition",
                         f"attachment; filename=\"{ascii_name}\"; filename*=UTF-8''{quote(name)}")
        self.end_headers()
        try:
            with path.open("rb") as f:
                shutil.copyfileobj(f, self.wfile, 1 << 20)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="ytmerge local web UI")
    ap.add_argument("--port", type=int, default=int(os.environ.get("YTMERGE_PORT", DEFAULT_PORT)))
    ap.add_argument("--no-open", action="store_true", help="don't open the browser")
    args = ap.parse_args()

    url = f"http://localhost:{args.port}"
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError:
        # Most likely already running (e.g. the LaunchAgent) — just open it.
        print(f"port {args.port} busy — ytmerge web is probably already running at {url}")
        if not args.no_open:
            webbrowser.open(url)
        return 0

    try:
        ytdlp = ensure_ytdlp()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    threading.Thread(target=maybe_self_update, args=(ytdlp,), daemon=True).start()

    Handler.app = App(ytdlp)
    Handler.port = args.port
    server.daemon_threads = True
    print(f"ytmerge web → {url}   (yt-dlp: {ytdlp}; ffmpeg: "
          f"{'yes' if Handler.app.has_ffmpeg else 'MISSING — video downloads need it: brew install ffmpeg'})")
    print("Ctrl+C to stop.")
    if not args.no_open:
        threading.Timer(0.4, webbrowser.open, args=(url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
