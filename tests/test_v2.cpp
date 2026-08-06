// test_v2.cpp — unit tests for the v2 yt-dlp pipeline's pure logic.
#include "doctest.h"

#include "../src/transcript.hpp"
#include "../src/doc.hpp"
#include "../src/ytdlp.hpp"
#include "../src/proc.hpp"

#include <string>
#include <vector>

using core::json;

TEST_CASE("parse_json3_cues: basic events with timestamps") {
    json doc = json::parse(R"({
      "events": [
        {"tStartMs": 0,    "segs": [{"utf8": "Hello "}, {"utf8": "world"}]},
        {"tStartMs": 2500, "segs": [{"utf8": "second cue"}]}
      ]
    })");
    auto cues = transcript::parse_json3_cues(doc);
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].start_s == doctest::Approx(0.0));
    CHECK(cues[0].text == "Hello world");
    CHECK(cues[1].start_s == doctest::Approx(2.5));
    CHECK(cues[1].text == "second cue");
}

TEST_CASE("parse_json3_cues: skips styling events, newline-only segs, empties") {
    json doc = json::parse(R"({
      "events": [
        {"tStartMs": 0, "wpWinPosId": 1},
        {"tStartMs": 100, "segs": [{"utf8": "\n"}]},
        {"tStartMs": 200, "segs": [{"utf8": "  real   text\n"}]}
      ]
    })");
    auto cues = transcript::parse_json3_cues(doc);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "real text");
}

TEST_CASE("parse_json3_cues: consecutive duplicate text collapsed (auto-caption roll)") {
    json doc = json::parse(R"({
      "events": [
        {"tStartMs": 0,   "segs": [{"utf8": "same line"}]},
        {"tStartMs": 500, "segs": [{"utf8": "same line"}]},
        {"tStartMs": 900, "segs": [{"utf8": "next line"}]}
      ]
    })");
    auto cues = transcript::parse_json3_cues(doc);
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].text == "same line");
    CHECK(cues[1].text == "next line");
}

TEST_CASE("parse_json3_cues: missing/empty events array") {
    CHECK(transcript::parse_json3_cues(json::object()).empty());
    CHECK(transcript::parse_json3_cues(json::parse(R"({"events": []})")).empty());
}

TEST_CASE("format_timestamp: under and over an hour") {
    CHECK(transcript::format_timestamp(0.0)    == "[0:00]");
    CHECK(transcript::format_timestamp(75.4)   == "[1:15]");
    CHECK(transcript::format_timestamp(600.0)  == "[10:00]");
    CHECK(transcript::format_timestamp(3675.0) == "[1:01:15]");
}

TEST_CASE("render_transcript: paragraph break at interval") {
    std::vector<transcript::Cue> cues = {
        {0.0,  "one"}, {10.0, "two"}, {50.0, "three"}, {60.0, "four"},
    };
    // interval 45s: cue at 50s starts a new paragraph (50-0 >= 45)
    auto out = transcript::render_transcript(cues, {}, 45.0);
    CHECK(out == "[0:00] one two\n\n[0:50] three four");
}

TEST_CASE("render_transcript: chapter boundary forces paragraph break") {
    std::vector<transcript::Cue> cues = {
        {0.0, "intro words"}, {5.0, "more intro"}, {30.0, "chapter two starts"},
    };
    std::vector<transcript::Chapter> ch = {{0.0, "Intro"}, {30.0, "Part Two"}};
    auto out = transcript::render_transcript(cues, ch, 4500.0);
    CHECK(out == "[0:00] intro words more intro\n\n[0:30] chapter two starts");
}

TEST_CASE("render_transcript: empty cues") {
    CHECK(transcript::render_transcript({}, {}, 45.0).empty());
}

TEST_CASE("parse_info_json: full metadata") {
    json info = json::parse(R"({
      "id": "dQw4w9WgXcQ", "title": "A Video", "channel": "Some Channel",
      "upload_date": "20260805", "duration": 3675,
      "view_count": 1000, "like_count": 50,
      "tags": ["a", "b"], "description": "line1\nline2",
      "chapters": [{"start_time": 0.0, "title": "Intro"},
                   {"start_time": 60.5, "title": "Main"}]
    })");
    auto m = doc::parse_info_json(info);
    CHECK(m.id == "dQw4w9WgXcQ");
    CHECK(m.title == "A Video");
    CHECK(m.channel == "Some Channel");
    CHECK(m.upload_date == "2026-08-05");
    CHECK(m.duration_s == 3675);
    CHECK(m.view_count == 1000);
    CHECK(m.like_count == 50);
    REQUIRE(m.tags.size() == 2);
    REQUIRE(m.chapters.size() == 2);
    CHECK(m.chapters[1].start_s == doctest::Approx(60.5));
    CHECK(m.chapters[1].title == "Main");
}

TEST_CASE("parse_info_json: nulls and missing fields tolerated") {
    json info = json::parse(R"({
      "id": "x", "title": "T", "uploader": "Fallback Uploader",
      "duration": null, "like_count": null
    })");
    auto m = doc::parse_info_json(info);
    CHECK(m.channel == "Fallback Uploader");   // "channel" absent -> "uploader"
    CHECK(m.duration_s == 0);
    CHECK(m.like_count == -1);
    CHECK(m.upload_date.empty());
    CHECK(m.chapters.empty());
}

TEST_CASE("format_duration") {
    CHECK(doc::format_duration(75)   == "1:15");
    CHECK(doc::format_duration(3675) == "1:01:15");
}

TEST_CASE("compose_document: full header, chapters line, transcript body") {
    doc::VideoMeta m;
    m.id = "abc123def45"; m.title = "My Title"; m.channel = "Chan";
    m.upload_date = "2026-08-05"; m.duration_s = 75;
    m.view_count = 10; m.like_count = 2;
    m.tags = {"x", "y"};
    m.chapters = {{0.0, "Intro"}, {30.0, "End"}};
    m.description = "desc here";
    auto d = doc::compose_document(m, "[0:00] hello");
    CHECK(d.find("TITLE:    My Title") != std::string::npos);
    CHECK(d.find("CHANNEL:  Chan") != std::string::npos);
    CHECK(d.find("DATE:     2026-08-05") != std::string::npos);
    CHECK(d.find("DURATION: 1:15") != std::string::npos);
    CHECK(d.find("VIEWS:    10    LIKES: 2") != std::string::npos);
    CHECK(d.find("URL:      https://www.youtube.com/watch?v=abc123def45") != std::string::npos);
    CHECK(d.find("TAGS:     x, y") != std::string::npos);
    CHECK(d.find("CHAPTERS: [0:00] Intro | [0:30] End") != std::string::npos);
    CHECK(d.find("DESCRIPTION:\ndesc here") != std::string::npos);
    CHECK(d.find("[0:00] hello") != std::string::npos);
}

TEST_CASE("compose_document: minimal metadata, missing transcript") {
    doc::VideoMeta m;
    m.id = "abc123def45"; m.title = "T";
    auto d = doc::compose_document(m, "");
    CHECK(d.find("(no transcript available)") != std::string::npos);
    CHECK(d.find("DATE:") == std::string::npos);
    CHECK(d.find("VIEWS:") == std::string::npos);
    CHECK(d.find("TAGS:") == std::string::npos);
    CHECK(d.find("CHAPTERS:") == std::string::npos);
    CHECK(d.find("DESCRIPTION:") == std::string::npos);
}

TEST_CASE("extract_title round-trips compose_document") {
    doc::VideoMeta m;
    m.id = "abc123def45"; m.title = "Round Trip";
    CHECK(doc::extract_title(doc::compose_document(m, "t")) == "Round Trip");
    CHECK(doc::extract_title("no header here").empty());
}

TEST_CASE("classify_stderr") {
    using E = ytdlp::Err;
    CHECK(ytdlp::classify_stderr("ERROR: ... HTTP Error 429: Too Many Requests") == E::IpBlocked);
    CHECK(ytdlp::classify_stderr("ERROR: [youtube] xyz: Private video.") == E::Unavailable);
    CHECK(ytdlp::classify_stderr("ERROR: [youtube] xyz: Video unavailable") == E::Unavailable);
    CHECK(ytdlp::classify_stderr("ERROR: unable to download webpage (timed out)") == E::Network);
    CHECK(ytdlp::classify_stderr("something novel") == E::Network);
}

TEST_CASE("sanitize_filename strips path-illegal characters") {
    CHECK(ytdlp::sanitize_filename("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
    CHECK(ytdlp::sanitize_filename("trailing dots.. ") == "trailing dots");
    CHECK(ytdlp::sanitize_filename("") == "untitled");
}

TEST_CASE("metadata_cmd: skip-download subs+info fetch keyed by id") {
    auto c = ytdlp::metadata_cmd("/bin/yt-dlp", "abc123def45", "/tmp/work", "");
    CHECK(c[0] == "/bin/yt-dlp");
    CHECK(c[1] == "https://www.youtube.com/watch?v=abc123def45");
    auto has = [&](const std::string& s) {
        for (const auto& a : c) if (a == s) return true;
        return false;
    };
    CHECK(has("--skip-download"));
    CHECK(has("--write-info-json"));
    CHECK(has("--write-subs"));
    CHECK(has("--write-auto-subs"));
    CHECK(has("en.*,en"));
    CHECK(has("json3"));
    CHECK(has("%(id)s.%(ext)s"));
    CHECK(!has("--proxy"));
}

TEST_CASE("download_cmd: load-info-json + print filepath, proxy passthrough") {
    auto c = ytdlp::download_cmd("yt-dlp", "abc123def45", "/w/abc.info.json",
                                 "downloaded", ytdlp::FORMAT_MERGED,
                                 false, "", "http://p:1");
    auto has = [&](const std::string& s) {
        for (const auto& a : c) if (a == s) return true;
        return false;
    };
    CHECK(has("--load-info-json"));
    CHECK(has("/w/abc.info.json"));
    CHECK(has("--no-simulate"));           // --print alone implies simulate
    CHECK(has("after_move:filepath"));
    CHECK(has(ytdlp::FORMAT_MERGED));
    CHECK(has("--windows-filenames"));
    CHECK(has("--proxy"));
    CHECK(has("http://p:1"));
    CHECK(!has("--extract-audio"));
}

TEST_CASE("download_cmd: audio mode replaces format with extract-audio") {
    auto c = ytdlp::download_cmd("yt-dlp", "abc123def45", "", "out",
                                 ytdlp::FORMAT_MERGED, true, "mp3", "");
    auto has = [&](const std::string& s) {
        for (const auto& a : c) if (a == s) return true;
        return false;
    };
    CHECK(has("--extract-audio"));
    CHECK(has("mp3"));
    CHECK(!has("--format"));
    // no info.json given -> falls back to the URL
    CHECK(has("https://www.youtube.com/watch?v=abc123def45"));
}

TEST_CASE("proc::run captures stdout, stderr, and exit code") {
    auto r = proc::run({"sh", "-c", "echo out-line; echo err-line 1>&2; exit 3"}, 30);
    CHECK(r.timed_out == false);
    CHECK(r.exit_code == 3);
    CHECK(r.out.find("out-line") != std::string::npos);
    CHECK(r.err.find("err-line") != std::string::npos);
}

TEST_CASE("proc::run: nonexistent binary yields nonzero exit") {
    auto r = proc::run({"definitely-not-a-real-binary-xyz"}, 30);
    CHECK(r.exit_code != 0);
}

TEST_CASE("proc::run: timeout kills the child") {
    auto r = proc::run({"sh", "-c", "sleep 30"}, 1);
    CHECK(r.timed_out == true);
}
