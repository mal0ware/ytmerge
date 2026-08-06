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
