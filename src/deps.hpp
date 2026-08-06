// deps.hpp — locate external binaries (yt-dlp, ffmpeg) and decide when the
// daily self-update check is due. Pure path/env logic; the actual download
// (libcurl) lives in ytmerge.cpp.

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core.hpp"

namespace deps {

inline std::filesystem::path bin_dir(core::GetEnvFn get_env = &core::default_getenv) {
    auto d = core::cache_dir(get_env);
    if (d.empty()) return {};
    return d / "bin";
}

inline const char* ytdlp_local_name() {
#if defined(_WIN32)
    return "yt-dlp.exe";
#else
    return "yt-dlp";
#endif
}

inline std::string ytdlp_download_url() {
#if defined(_WIN32)
    const char* asset = "yt-dlp.exe";
#elif defined(__APPLE__)
    const char* asset = "yt-dlp_macos";
#else
    const char* asset = "yt-dlp_linux";   // standalone build, no system python needed
#endif
    return std::string("https://github.com/yt-dlp/yt-dlp/releases/latest/download/")
           + asset;
}

inline std::optional<std::filesystem::path>
find_on_path(const std::string& exe, core::GetEnvFn get_env = &core::default_getenv) {
    const char* path = get_env("PATH");
    if (!path) return std::nullopt;
#if defined(_WIN32)
    const char sep = ';';
    const std::vector<std::string> names = {exe + ".exe", exe};
#else
    const char sep = ':';
    const std::vector<std::string> names = {exe};
#endif
    const std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(sep, start);
        const std::string dir =
            p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (const auto& n : names) {
                std::filesystem::path cand = std::filesystem::path(dir) / n;
                std::error_code ec;
                if (std::filesystem::is_regular_file(cand, ec)) return cand;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

// The daily-update stamp is an empty file whose mtime records the last check.
inline bool update_due(const std::filesystem::path& stamp_file) {
    std::error_code ec;
    if (!std::filesystem::exists(stamp_file, ec)) return true;
    const auto t = std::filesystem::last_write_time(stamp_file, ec);
    if (ec) return true;
    return (std::filesystem::file_time_type::clock::now() - t)
           > std::chrono::hours(24);
}

}  // namespace deps
