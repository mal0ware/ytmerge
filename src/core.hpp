// core.hpp — pure, network-free logic shared by the ytmerge app and its tests.
//
// Everything in here is deterministic: string parsing, JSON walking, and path
// computation. No libcurl, no clipboard, no threads, no file I/O. The app
// (src/ytmerge.cpp) includes this header; so does tests/test_core.cpp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>
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
