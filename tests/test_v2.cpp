// test_v2.cpp — unit tests for the v2 yt-dlp pipeline's pure logic.
#include "doctest.h"

#include "../src/transcript.hpp"
#include "../src/doc.hpp"

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
