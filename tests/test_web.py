"""Unit tests for web/server.py's pure logic — mirrors tests/test_v2.cpp so the
web UI and the C++ CLI produce identical transcript documents.

Run:  python3 -m unittest discover -s tests -p 'test_web.py'
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from datetime import date
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "web"))
import browsers as b  # noqa: E402
import server as s  # noqa: E402


class ExtractIds(unittest.TestCase):
    def test_forms_dedupe_order(self):
        text = ("see https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=1 and youtu.be/abcdefghijk "
                "https://youtube.com/shorts/ZZZZZZZZZZZ, /embed/dQw4w9WgXcQ junk "
                "https://www.youtube.com/live/LLLLLLLLLLL")
        self.assertEqual(s.extract_video_ids(text),
                         ["dQw4w9WgXcQ", "abcdefghijk", "ZZZZZZZZZZZ", "LLLLLLLLLLL"])

    def test_none(self):
        self.assertEqual(s.extract_video_ids("no links here"), [])


class Json3(unittest.TestCase):
    def test_basic(self):
        cues = s.parse_json3_cues({"events": [
            {"tStartMs": 0, "segs": [{"utf8": "Hello "}, {"utf8": "world"}]},
            {"tStartMs": 2500, "segs": [{"utf8": "second\ncue"}]}]})
        self.assertEqual(cues, [(0.0, "Hello world"), (2.5, "second cue")])

    def test_skips_styling_empty_and_duplicates(self):
        cues = s.parse_json3_cues({"events": [
            {"tStartMs": 0},
            {"tStartMs": 100, "segs": [{"utf8": "\n"}]},
            {"tStartMs": 200, "segs": [{"utf8": "  same   line "}]},
            {"tStartMs": 300, "segs": [{"utf8": "same line"}]},
            {"tStartMs": 400, "segs": [{"utf8": "next line"}]}]})
        self.assertEqual([c[1] for c in cues], ["same line", "next line"])

    def test_missing_events(self):
        self.assertEqual(s.parse_json3_cues({}), [])


class Render(unittest.TestCase):
    def test_timestamps(self):
        self.assertEqual(s.format_timestamp(0), "[0:00]")
        self.assertEqual(s.format_timestamp(75.4), "[1:15]")
        self.assertEqual(s.format_timestamp(3675), "[1:01:15]")
        self.assertEqual(s.format_duration(3675), "1:01:15")

    def test_paragraph_interval(self):
        out = s.render_transcript([(0, "one"), (10, "two"), (50, "three"), (55, "four")], [])
        self.assertEqual(out, "[0:00] one two\n\n[0:50] three four")

    def test_chapter_break(self):
        out = s.render_transcript([(0, "intro words"), (10, "more intro"), (30, "chapter two starts")],
                                  [(0, "Intro"), (30, "Two")])
        self.assertEqual(out, "[0:00] intro words more intro\n\n[0:30] chapter two starts")


class Document(unittest.TestCase):
    INFO = {"id": "abc123def45", "title": "My Title", "channel": "Chan", "upload_date": "20260805",
            "duration": 75, "view_count": 10, "like_count": 2, "tags": ["a", "b"],
            "chapters": [{"start_time": 0, "title": "Intro"}], "description": "desc"}

    def test_full(self):
        d = s.compose_document(self.INFO, "[0:00] hi")
        for needle in ("TITLE:    My Title", "CHANNEL:  Chan", "DATE:     2026-08-05",
                       "DURATION: 1:15", "VIEWS:    10    LIKES: 2",
                       "URL:      https://www.youtube.com/watch?v=abc123def45",
                       "TAGS:     a, b", "CHAPTERS: [0:00] Intro", "\nDESCRIPTION:\ndesc\n"):
            self.assertIn(needle, d)
        self.assertTrue(d.startswith("=" * 80 + "\n"))
        self.assertTrue(d.endswith("=" * 80 + "\n\n[0:00] hi\n"))

    def test_nulls_and_header_roundtrip(self):
        d = s.compose_document({"id": "x" * 11, "title": "T", "channel": None,
                                "uploader": "Up", "like_count": None}, "")
        self.assertIn("CHANNEL:  Up", d)
        self.assertNotIn("LIKES", d)
        h = s.parse_header(d)
        self.assertEqual((h["title"], h["channel"], h["has_transcript"]), ("T", "Up", False))
        self.assertTrue(s.parse_header(s.compose_document(self.INFO, "[0:00] hi"))["has_transcript"])

    def test_classify(self):
        self.assertIn("429", s.classify_stderr("ERROR: HTTP Error 429: Too Many Requests"))
        self.assertEqual(s.classify_stderr("ERROR: Private video"), "video unavailable")

    def test_classify_403_points_at_yt_dlp(self):
        msg = s.classify_stderr("ERROR: unable to download video data: HTTP Error 403: Forbidden")
        self.assertIn("403", msg)
        self.assertIn("yt-dlp", msg)


class YtdlpVersionAge(unittest.TestCase):
    TODAY = date(2026, 10, 27)

    def test_release(self):
        self.assertEqual(s.version_age_days("2026.07.04", self.TODAY), 115)

    def test_nightly_suffix(self):
        self.assertEqual(s.version_age_days("2026.10.26.232843", self.TODAY), 1)

    def test_unreadable(self):
        self.assertIsNone(s.version_age_days(None, self.TODAY))
        self.assertIsNone(s.version_age_days("garbage", self.TODAY))
        self.assertIsNone(s.version_age_days("2026.13.40", self.TODAY))


class YtdlpInstallMethod(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.own = self.tmp / "cache" / "bin"
        self.uv_bin = self.tmp / "uvbin"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_own_copy(self):
        self.assertEqual(s.install_method(str(self.own / "yt-dlp"), self.own, None, False), "own")

    def test_uv_tool(self):
        path = str(self.uv_bin / "yt-dlp.EXE")
        self.assertEqual(s.install_method(path, self.own, str(self.uv_bin), True), "uv")

    def test_uv_bin_dir_without_uv_tool_is_not_uv(self):
        # A yt-dlp.exe dropped into ~/.local/bin by hand isn't uv's to upgrade.
        path = str(self.uv_bin / "yt-dlp.exe")
        self.assertEqual(s.install_method(path, self.own, str(self.uv_bin), False), "other")

    def test_brew(self):
        self.assertEqual(s.install_method("/opt/homebrew/bin/yt-dlp", self.own, None, False), "brew")

    def test_unknown(self):
        self.assertEqual(s.install_method(str(self.tmp / "x" / "yt-dlp"), self.own, None, False), "other")


class YtdlpUpdates(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.stamp = self.tmp / "bin" / ".last-update-check"
        self.calls = []
        self.logs = []
        self.version = "2026.07.04"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, cmd, **kw):
        """Stands in for subprocess.run: an upgrade bumps the version."""
        self.calls.append(cmd)
        if cmd[-1] == "--version":
            return subprocess.CompletedProcess(cmd, 0, self.version + "\n", "")
        self.version = "2026.10.26"
        return subprocess.CompletedProcess(cmd, 0, "Updated yt-dlp\n", "")

    def tool(self, method):
        t = s.Ytdlp("/x/yt-dlp", method, run=self.run_fake, log=self.logs.append)
        t.read_version()
        return t

    def test_update_is_logged(self):
        self.tool("uv").maybe_update(self.stamp, time.time())
        self.assertEqual(self.logs, ["yt-dlp updated: 2026.07.04 → 2026.10.26"])

    def test_waits_while_downloads_run(self):
        # Upgrading while yt-dlp runs can fail on Windows (files in use).
        t = self.tool("uv")
        self.assertFalse(t.maybe_update(self.stamp, time.time(), busy=True))
        self.assertEqual(t.version, "2026.07.04")

    def test_update_commands(self):
        self.assertEqual(s.update_command("uv", "/x/yt-dlp"), ["uv", "tool", "upgrade", "yt-dlp"])
        self.assertEqual(s.update_command("own", "/x/yt-dlp"), ["/x/yt-dlp", "-U"])
        self.assertIsNone(s.update_command("brew", "/x/yt-dlp"))
        self.assertIsNone(s.update_command("other", "/x/yt-dlp"))

    def test_uv_install_is_upgraded_when_never_checked(self):
        t = self.tool("uv")
        self.assertTrue(t.maybe_update(self.stamp, time.time()))
        self.assertIn(["uv", "tool", "upgrade", "yt-dlp"], self.calls)
        self.assertEqual(t.version, "2026.10.26")
        self.assertTrue(self.stamp.is_file())

    def test_checked_today_is_skipped(self):
        t = self.tool("uv")
        self.stamp.parent.mkdir(parents=True)
        self.stamp.touch()
        self.assertFalse(t.maybe_update(self.stamp, time.time()))
        self.assertEqual(t.version, "2026.07.04")

    def test_checked_yesterday_runs_again(self):
        t = self.tool("uv")
        self.stamp.parent.mkdir(parents=True)
        self.stamp.touch()
        self.assertTrue(t.maybe_update(self.stamp, time.time() + 86400 + 60))

    def test_brew_is_left_alone(self):
        t = self.tool("brew")
        self.assertFalse(t.maybe_update(self.stamp, time.time()))
        self.assertEqual(self.calls, [["/x/yt-dlp", "--version"]])

    def test_failed_update_is_reported_and_retried(self):
        def failing(cmd, **kw):
            if cmd[-1] == "--version":
                return subprocess.CompletedProcess(cmd, 0, "2026.07.04\n", "")
            return subprocess.CompletedProcess(cmd, 2, "", "error: failed to remove file: in use\n")
        t = s.Ytdlp("/x/yt-dlp", "uv", run=failing, log=self.logs.append)
        self.assertTrue(t.maybe_update(self.stamp, time.time()))
        self.assertIn("in use", t.error)
        self.assertIn("in use", self.logs[0])
        self.assertFalse(self.stamp.exists())  # no stamp, so the next check tries again

    def test_info_flags_old_version_with_fix(self):
        info = self.tool("uv").info(date(2026, 10, 27))
        self.assertEqual(info["version"], "2026.07.04")
        self.assertEqual(info["ageDays"], 115)
        self.assertTrue(info["old"])
        self.assertEqual(info["fix"], "uv tool upgrade yt-dlp")

    def test_info_recent_version_is_not_old(self):
        self.version = "2026.10.26"
        self.assertFalse(self.tool("uv").info(date(2026, 10, 27))["old"])

    def test_info_unknown_version_is_not_old(self):
        t = s.Ytdlp("/x/yt-dlp", "other", run=self.run_fake, log=self.logs.append)
        self.assertFalse(t.info(date(2026, 10, 27))["old"])


def snss(*tabs):
    """A minimal Chromium session file: one tab per (tab id, window id, url)."""
    out = bytearray(b"SNSS" + struct.pack("<i", 3))

    def cmd(cid, payload):
        out.extend(struct.pack("<H", len(payload) + 1))
        out.append(cid)
        out.extend(payload)

    for tid, win, url in tabs:
        u = url.encode()
        nav = struct.pack("<iii", tid, 0, len(u)) + u + b"\0" * (-len(u) % 4)
        cmd(b.CMD_SET_TAB_WINDOW, struct.pack("<ii", win, tid))
        cmd(b.CMD_UPDATE_NAVIGATION, struct.pack("<I", len(nav)) + nav)
        cmd(b.CMD_SELECTED_NAVIGATION, struct.pack("<ii", tid, 0))
    return bytes(out)


class SessionTabs(unittest.TestCase):
    """On Windows a running browser keeps its live session file locked, so
    read_tabs falls back to the newest older snapshot and says how old it is."""

    LIVE = "https://www.youtube.com/watch?v=liveliveliv"
    OLD = "https://www.youtube.com/watch?v=oldoldoldol"

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)
        self.old = self.dir / "Session_100"
        self.old.write_bytes(snss((1, 1, self.OLD)))
        os.utime(self.old, (1_000_000, 1_000_000))
        self.live = self.dir / "Session_200"
        self.live.write_bytes(snss((1, 1, self.LIVE), (2, 1, "https://example.com/")))
        os.utime(self.live, (2_000_000, 2_000_000))

    def locked(self, *paths):
        """Make reading these files fail the way a Windows sharing violation does."""
        real = Path.read_bytes

        def read_bytes(path):
            if path in paths:
                raise PermissionError(13, "Permission denied", str(path))
            return real(path)

        return mock.patch.object(Path, "read_bytes", autospec=True, side_effect=read_bytes)

    def test_parses_open_tabs(self):
        self.assertEqual(b.parse_snss(self.live.read_bytes()), [self.LIVE, "https://example.com/"])

    def test_live_file_readable(self):
        self.assertEqual(b.read_tabs(self.live), ([self.LIVE, "https://example.com/"], None))

    def test_locked_live_file_falls_back_to_newest_snapshot(self):
        with self.locked(self.live):
            self.assertEqual(b.read_tabs(self.live), ([self.OLD], 1_000_000))

    def test_everything_locked(self):
        with self.locked(self.live, self.old):
            with self.assertRaisesRegex(LookupError, "locked"):
                b.read_tabs(self.live)


if __name__ == "__main__":
    unittest.main()
