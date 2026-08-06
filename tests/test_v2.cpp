// test_v2.cpp — unit tests for the v2 yt-dlp pipeline's pure logic.
#include "doctest.h"

#include "../src/transcript.hpp"

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
