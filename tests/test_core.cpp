// test_core.cpp — unit tests for the pure logic in src/core.hpp.
//
// Network-free by construction: core.hpp pulls in no libcurl, no clipboard,
// no threads, and no file I/O, so these tests run anywhere a C++20 compiler
// and the vendored nlohmann/json header are available. The cache-path tests
// inject a fake getenv so they exercise the env-var fallback chain without
// touching the real process environment.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../src/core.hpp"

#include <string>
#include <vector>

using core::json;

// ─── Video ID extraction ────────────────────────────────────────────────────

TEST_CASE("extract_video_ids: canonical watch?v= URL") {
    auto ids = core::extract_video_ids("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "dQw4w9WgXcQ");
}

TEST_CASE("extract_video_ids: youtu.be short link") {
    auto ids = core::extract_video_ids("check this https://youtu.be/dQw4w9WgXcQ out");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "dQw4w9WgXcQ");
}

TEST_CASE("extract_video_ids: /shorts/ form") {
    auto ids = core::extract_video_ids("https://www.youtube.com/shorts/abcdEFGH_-1");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "abcdEFGH_-1");
}

TEST_CASE("extract_video_ids: /embed/ form") {
    auto ids = core::extract_video_ids("<iframe src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\">");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "dQw4w9WgXcQ");
}

TEST_CASE("extract_video_ids: watch?v= with extra query params") {
    auto ids = core::extract_video_ids(
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLxyz&index=2&t=42s");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "dQw4w9WgXcQ");
}

TEST_CASE("extract_video_ids: ids with underscores and hyphens") {
    auto ids = core::extract_video_ids("https://youtu.be/_-Ab12CD_xy");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "_-Ab12CD_xy");
}

TEST_CASE("extract_video_ids: dedup preserves first-seen order") {
    const std::string blob =
        "https://www.youtube.com/watch?v=aaaaaaaaaaa "
        "https://youtu.be/bbbbbbbbbbb "
        "https://www.youtube.com/watch?v=aaaaaaaaaaa "   // dup of #1
        "https://www.youtube.com/shorts/ccccccccccc";
    auto ids = core::extract_video_ids(blob);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == "aaaaaaaaaaa");
    CHECK(ids[1] == "bbbbbbbbbbb");
    CHECK(ids[2] == "ccccccccccc");
}

TEST_CASE("extract_video_ids: mixed messy text with several forms") {
    const std::string blob =
        "watch this: https://www.youtube.com/watch?v=11111111111\n"
        "and this https://youtu.be/22222222222 plus an embed "
        "https://www.youtube.com/embed/33333333333 and a short "
        "youtube.com/shorts/44444444444 done";
    auto ids = core::extract_video_ids(blob);
    REQUIRE(ids.size() == 4);
    CHECK(ids[0] == "11111111111");
    CHECK(ids[1] == "22222222222");
    CHECK(ids[2] == "33333333333");
    CHECK(ids[3] == "44444444444");
}

TEST_CASE("extract_video_ids: no YouTube URLs yields empty") {
    CHECK(core::extract_video_ids("").empty());
    CHECK(core::extract_video_ids("just some plain text, no links").empty());
    CHECK(core::extract_video_ids("https://vimeo.com/123456789").empty());
}

TEST_CASE("extract_video_ids: rejects too-short / too-long candidates") {
    // 10 chars after v= is too short, 12 over-runs into a valid 11-char prefix.
    CHECK(core::extract_video_ids("https://www.youtube.com/watch?v=short").empty());
    // A 12-char run still yields a valid 11-char prefix (greedy regex grabs 11).
    auto ids = core::extract_video_ids("https://www.youtube.com/watch?v=abcdefghijkl");
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "abcdefghijk");
}

// ─── Balanced-brace JSON scanner ──────────────────────────────────────────────

TEST_CASE("extract_balanced_json: simple object after anchor") {
    auto r = core::extract_balanced_json(
        R"(var ytInitialPlayerResponse = {"a":1};)", "ytInitialPlayerResponse");
    REQUIRE(r.has_value());
    CHECK(*r == R"({"a":1})");
}

TEST_CASE("extract_balanced_json: nested objects") {
    auto r = core::extract_balanced_json(
        R"(x = {"a":{"b":{"c":2}},"d":3} trailing junk)", "x =");
    REQUIRE(r.has_value());
    CHECK(*r == R"({"a":{"b":{"c":2}},"d":3})");
}

TEST_CASE("extract_balanced_json: ignores braces inside strings") {
    auto r = core::extract_balanced_json(
        R"(anchor {"k":"a } } { string with braces","n":1})", "anchor");
    REQUIRE(r.has_value());
    CHECK(*r == R"({"k":"a } } { string with braces","n":1})");
}

TEST_CASE("extract_balanced_json: handles escaped quote inside string") {
    // The closing brace must not be swallowed by an escaped quote: "\"}" is a
    // literal quote followed by close-brace-inside-string, then real close.
    auto r = core::extract_balanced_json(
        R"(A {"k":"he said \"hi\" }","x":0})", "A");
    REQUIRE(r.has_value());
    CHECK(*r == R"({"k":"he said \"hi\" }","x":0})");
}

TEST_CASE("extract_balanced_json: escaped backslash before quote") {
    // "...\\" ends the string (the backslash is escaped, the quote is real),
    // so the following brace is structural.
    auto r = core::extract_balanced_json(
        R"(A {"k":"trailing slash \\","y":1})", "A");
    REQUIRE(r.has_value());
    CHECK(*r == R"({"k":"trailing slash \\","y":1})");
}

TEST_CASE("extract_balanced_json: anchor not present") {
    auto r = core::extract_balanced_json(R"({"a":1})", "missingAnchor");
    CHECK(!r.has_value());
}

TEST_CASE("extract_balanced_json: no open brace after anchor") {
    auto r = core::extract_balanced_json("anchor but no object here", "anchor");
    CHECK(!r.has_value());
}

TEST_CASE("extract_balanced_json: unbalanced (never closes) returns nullopt") {
    auto r = core::extract_balanced_json(R"(anchor {"a":{"b":1})", "anchor");
    CHECK(!r.has_value());
}

TEST_CASE("extract_balanced_json: result is valid parseable JSON") {
    const std::string html =
        R"(<script>var ytInitialPlayerResponse = )"
        R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"title":"Hi {there}"}};)"
        R"(</script>)";
    auto r = core::extract_balanced_json(html, "ytInitialPlayerResponse");
    REQUIRE(r.has_value());
    auto doc = json::parse(*r);
    CHECK(doc["playabilityStatus"]["status"] == "OK");
    CHECK(doc["videoDetails"]["title"] == "Hi {there}");
}

// ─── JSON3 transcript parsing ─────────────────────────────────────────────────

TEST_CASE("parse_json3_transcript: joins segments across events") {
    auto doc = json::parse(R"({
        "events": [
            {"segs": [{"utf8": "Hello"}, {"utf8": " world"}]},
            {"segs": [{"utf8": " again"}]}
        ]
    })");
    CHECK(core::parse_json3_transcript(doc) == "Hello world again");
}

TEST_CASE("parse_json3_transcript: collapses whitespace runs and trims") {
    auto doc = json::parse(R"({
        "events": [
            {"segs": [{"utf8": "a   "}, {"utf8": "  b"}]},
            {"segs": [{"utf8": "\n\nc \t "}]}
        ]
    })");
    // Newlines become spaces, runs collapse to single spaces, trailing trimmed.
    CHECK(core::parse_json3_transcript(doc) == "a b c");
}

TEST_CASE("parse_json3_transcript: skips events with no segs") {
    auto doc = json::parse(R"({
        "events": [
            {"tStartMs": 0},
            {"segs": [{"utf8": "kept"}]},
            {"segs": []}
        ]
    })");
    CHECK(core::parse_json3_transcript(doc) == "kept");
}

TEST_CASE("parse_json3_transcript: skips segs without utf8 or wrong type") {
    auto doc = json::parse(R"({
        "events": [
            {"segs": [{"acAsrConf": 0}, {"utf8": "real"}, {"utf8": 42}]}
        ]
    })");
    CHECK(core::parse_json3_transcript(doc) == "real");
}

TEST_CASE("parse_json3_transcript: missing events key yields empty") {
    auto doc = json::parse(R"({"wireMagic": "pb3"})");
    CHECK(core::parse_json3_transcript(doc).empty());
}

TEST_CASE("parse_json3_transcript: events not an array yields empty") {
    auto doc = json::parse(R"({"events": "nope"})");
    CHECK(core::parse_json3_transcript(doc).empty());
}

TEST_CASE("parse_json3_transcript: preserves unicode bytes") {
    auto doc = json::parse(R"({"events":[{"segs":[{"utf8":"café — naïve"}]}]})");
    CHECK(core::parse_json3_transcript(doc) == "café — naïve");
}

TEST_CASE("parse_json3_transcript: whitespace-only transcript collapses to empty") {
    auto doc = json::parse(R"({"events":[{"segs":[{"utf8":"   "},{"utf8":"\n"}]}]})");
    CHECK(core::parse_json3_transcript(doc).empty());
}

// ─── English caption-track selection ──────────────────────────────────────────

namespace {
json player_with_tracks(json tracks) {
    return json{{"captions",
                 {{"playerCaptionsTracklistRenderer", {{"captionTracks", tracks}}}}}};
}
}  // namespace

TEST_CASE("pick_english_track: single English manual track, fmt forced") {
    auto player = player_with_tracks(json::array(
        {{{"languageCode", "en"}, {"baseUrl", "https://x/timedtext?lang=en"}}}));
    auto url = core::pick_english_track(player);
    REQUIRE(url.has_value());
    CHECK(*url == "https://x/timedtext?lang=en&fmt=json3");
}

TEST_CASE("pick_english_track: adds ? when baseUrl has no query") {
    auto player = player_with_tracks(
        json::array({{{"languageCode", "en"}, {"baseUrl", "https://x/timedtext"}}}));
    auto url = core::pick_english_track(player);
    REQUIRE(url.has_value());
    CHECK(*url == "https://x/timedtext?fmt=json3");
}

TEST_CASE("pick_english_track: prefers manual over ASR") {
    auto player = player_with_tracks(json::array({
        {{"languageCode", "en"}, {"kind", "asr"}, {"baseUrl", "https://x/asr"}},
        {{"languageCode", "en"}, {"baseUrl", "https://x/manual"}},
    }));
    auto url = core::pick_english_track(player);
    REQUIRE(url.has_value());
    CHECK(*url == "https://x/manual?fmt=json3");
}

TEST_CASE("pick_english_track: falls back to ASR when no manual English") {
    auto player = player_with_tracks(json::array({
        {{"languageCode", "fr"}, {"baseUrl", "https://x/fr"}},
        {{"languageCode", "en"}, {"kind", "asr"}, {"baseUrl", "https://x/asr"}},
    }));
    auto url = core::pick_english_track(player);
    REQUIRE(url.has_value());
    CHECK(*url == "https://x/asr?fmt=json3");
}

TEST_CASE("pick_english_track: en-US / en-GB region variants qualify") {
    auto player = player_with_tracks(
        json::array({{{"languageCode", "en-GB"}, {"baseUrl", "https://x/gb"}}}));
    auto url = core::pick_english_track(player);
    REQUIRE(url.has_value());
    CHECK(*url == "https://x/gb?fmt=json3");
}

TEST_CASE("pick_english_track: no English track yields nullopt") {
    auto player = player_with_tracks(json::array({
        {{"languageCode", "fr"}, {"baseUrl", "https://x/fr"}},
        {{"languageCode", "de"}, {"baseUrl", "https://x/de"}},
    }));
    CHECK(!core::pick_english_track(player).has_value());
}

TEST_CASE("pick_english_track: no captions key yields nullopt") {
    CHECK(!core::pick_english_track(json::object()).has_value());
}

TEST_CASE("pick_english_track: English track missing baseUrl yields nullopt") {
    auto player = player_with_tracks(json::array({{{"languageCode", "en"}}}));
    CHECK(!core::pick_english_track(player).has_value());
}

// ─── Cache key / path logic ───────────────────────────────────────────────────

namespace {
// A swappable fake environment for the cache_dir tests. Set the static map
// before constructing the function pointer; core::cache_dir reads through it.
std::unordered_map<std::string, std::string> g_env;

const char* fake_getenv(const char* name) {
    auto it = g_env.find(name);
    return it == g_env.end() ? nullptr : it->second.c_str();
}
}  // namespace

TEST_CASE("cache_entry_path: appends <id>.txt under the dir") {
    namespace fs = std::filesystem;
    auto p = core::cache_entry_path(fs::path("base") / "ytmerge", "dQw4w9WgXcQ");
    CHECK(p.filename().string() == "dQw4w9WgXcQ.txt");
    CHECK(p.parent_path().filename().string() == "ytmerge");
}

#if !defined(_WIN32)
TEST_CASE("cache_dir: XDG_CACHE_HOME wins when set (POSIX)") {
    g_env = {{"XDG_CACHE_HOME", "/custom/cache"}, {"HOME", "/home/u"}};
    auto p = core::cache_dir(&fake_getenv);
    CHECK(p == std::filesystem::path("/custom/cache") / "ytmerge");
}

TEST_CASE("cache_dir: falls back to HOME/.cache when XDG unset (POSIX)") {
    g_env = {{"HOME", "/home/u"}};
    auto p = core::cache_dir(&fake_getenv);
    CHECK(p == std::filesystem::path("/home/u") / ".cache" / "ytmerge");
}

TEST_CASE("cache_dir: empty XDG_CACHE_HOME is treated as unset (POSIX)") {
    g_env = {{"XDG_CACHE_HOME", ""}, {"HOME", "/home/u"}};
    auto p = core::cache_dir(&fake_getenv);
    CHECK(p == std::filesystem::path("/home/u") / ".cache" / "ytmerge");
}

TEST_CASE("cache_dir: no HOME yields empty path (POSIX)") {
    g_env = {};
    CHECK(core::cache_dir(&fake_getenv).empty());
}
#else
TEST_CASE("cache_dir: LOCALAPPDATA wins when set (Windows)") {
    g_env = {{"LOCALAPPDATA", "C:/Users/u/AppData/Local"},
             {"USERPROFILE", "C:/Users/u"}};
    auto p = core::cache_dir(&fake_getenv);
    CHECK(p == std::filesystem::path("C:/Users/u/AppData/Local") / "ytmerge" / "cache");
}

TEST_CASE("cache_dir: falls back to USERPROFILE (Windows)") {
    g_env = {{"USERPROFILE", "C:/Users/u"}};
    auto p = core::cache_dir(&fake_getenv);
    CHECK(p == std::filesystem::path("C:/Users/u") / "ytmerge" / "cache");
}

TEST_CASE("cache_dir: no base env yields empty path (Windows)") {
    g_env = {};
    CHECK(core::cache_dir(&fake_getenv).empty());
}
#endif
