"""Unit tests for web/server.py's pure logic — mirrors tests/test_v2.cpp so the
web UI and the C++ CLI produce identical transcript documents.

Run:  python3 -m unittest discover -s tests -p 'test_web.py'
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "web"))
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


if __name__ == "__main__":
    unittest.main()
