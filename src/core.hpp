// core.hpp — pure, network-free logic shared by the ytmerge app and its tests.
//
// Everything in here is deterministic: string parsing, JSON walking, and path
// computation. No libcurl, no clipboard, no threads, no file I/O. The app
// (src/ytmerge.cpp) includes this header; so does tests/test_core.cpp.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace core {

using json = nlohmann::json;

// ─── Constants ──────────────────────────────────────────────────────────────

constexpr int CACHE_TTL_DAYS = 7;

// ─── Video ID extraction ────────────────────────────────────────────────────
//
// Forgiving by design: scans arbitrary text for anything that looks like a
// YouTube video reference (watch?v=, youtu.be/, /embed/, /shorts/) and pulls
// the 11-character ID. Deduplicates while preserving first-seen order.

inline std::vector<std::string> extract_video_ids(const std::string& text) {
    static const std::regex id_re(
        R"((?:v=|youtu\.be/|/embed/|/shorts/)([A-Za-z0-9_-]{11}))");
    std::vector<std::string>          ids;
    std::unordered_set<std::string>   seen;
    auto begin = std::sregex_iterator(text.begin(), text.end(), id_re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string id = (*it)[1].str();
        if (seen.insert(id).second) ids.push_back(id);
    }
    return ids;
}

// ─── JSON extraction from YouTube watch page ────────────────────────────────
//
// The watch HTML contains a script block:
//   var ytInitialPlayerResponse = {...};
// We locate that JSON object by anchor, then walk braces (tracking string
// context to ignore braces inside strings) until the matching close-brace.

inline std::optional<std::string> extract_balanced_json(std::string_view html,
                                                        std::string_view anchor) {
    auto pos = html.find(anchor);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = html.find('{', pos + anchor.size());
    if (pos == std::string_view::npos) return std::nullopt;

    int  depth     = 0;
    bool in_string = false;
    bool escape    = false;
    for (size_t i = pos; i < html.size(); ++i) {
        const char c = html[i];
        if (in_string) {
            if (escape)         escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"')  in_string = false;
        } else {
            if      (c == '"') in_string = true;
            else if (c == '{') ++depth;
            else if (c == '}') {
                if (--depth == 0) {
                    return std::string(html.substr(pos, i - pos + 1));
                }
            }
        }
    }
    return std::nullopt;
}

// ─── JSON3 transcript parsing ───────────────────────────────────────────────

// Parse the JSON3 caption format YouTube returns:
//   { "events": [ { "segs": [ { "utf8": "Hello" }, ... ], ... }, ... ] }
inline std::string parse_json3_transcript(const json& doc) {
    std::string out;
    if (!doc.contains("events") || !doc["events"].is_array()) return out;
    out.reserve(8 * 1024);
    for (const auto& ev : doc["events"]) {
        if (!ev.contains("segs") || !ev["segs"].is_array()) continue;
        for (const auto& seg : ev["segs"]) {
            if (!seg.contains("utf8") || !seg["utf8"].is_string()) continue;
            std::string text = seg["utf8"].get<std::string>();
            std::replace(text.begin(), text.end(), '\n', ' ');
            out += text;
        }
    }
    // Collapse runs of whitespace introduced by the segment joining.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        const bool is_space = (c == ' ' || c == '\t');
        if (is_space) {
            if (!prev_space && !collapsed.empty()) collapsed += ' ';
            prev_space = true;
        } else {
            collapsed += c;
            prev_space = false;
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return collapsed;
}

// Resolve the English caption track URL from a player response. Manual tracks
// are preferred over ASR; any "en*" language code qualifies.
inline std::optional<std::string> pick_english_track(const json& player) {
    if (!player.contains("captions")) return std::nullopt;
    const auto& tracklist = player["captions"]
        .value("playerCaptionsTracklistRenderer", json::object());
    if (!tracklist.contains("captionTracks") || !tracklist["captionTracks"].is_array())
        return std::nullopt;

    const json* manual = nullptr;
    const json* asr    = nullptr;
    for (const auto& t : tracklist["captionTracks"]) {
        const std::string lang = t.value("languageCode", "");
        if (lang.rfind("en", 0) != 0) continue;  // not English
        const bool is_asr = (t.value("kind", "") == "asr");
        if (is_asr) { if (!asr)    asr    = &t; }
        else        { if (!manual) manual = &t; }
    }
    const json* pick = manual ? manual : asr;
    if (!pick) return std::nullopt;
    if (!pick->contains("baseUrl")) return std::nullopt;
    std::string url = (*pick)["baseUrl"].get<std::string>();
    // Force JSON3 format — easier to parse and unicode-safe.
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "fmt=json3";
    return url;
}

// ─── Cache key / path logic ─────────────────────────────────────────────────
//
// Layout: <cache_dir>/<video_id>.txt
//   Windows: %LOCALAPPDATA%\ytmerge\cache (falls back to %USERPROFILE%)
//   POSIX:   $XDG_CACHE_HOME/ytmerge, else $HOME/.cache/ytmerge
// The env lookup is injectable so tests can exercise the fallback chain
// without mutating the process environment.

using GetEnvFn = const char* (*)(const char*);

inline const char* default_getenv(const char* name) { return std::getenv(name); }

inline std::filesystem::path cache_dir(GetEnvFn get_env = &default_getenv) {
#if defined(_WIN32)
    // Standard Windows app-data location for per-user caches.
    const char* base = get_env("LOCALAPPDATA");
    if (!base) base = get_env("USERPROFILE");
    if (!base) return {};
    return std::filesystem::path(base) / "ytmerge" / "cache";
#else
    // XDG_CACHE_HOME wins on Linux when set; macOS falls through to ~/.cache,
    // which keeps the path identical between the two POSIX targets.
    const char* xdg = get_env("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "ytmerge";
    const char* home = get_env("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".cache" / "ytmerge";
#endif
}

inline std::filesystem::path cache_entry_path(const std::filesystem::path& dir,
                                              const std::string& video_id) {
    return dir / (video_id + ".txt");
}

}  // namespace core
