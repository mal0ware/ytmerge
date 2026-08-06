// ytdlp.hpp — pure helpers for driving yt-dlp: command construction, stderr
// classification, filename sanitization. No process spawning here (proc.hpp).

#pragma once

#include <string>
#include <vector>

namespace ytdlp {

enum class Err {
    None,
    IpBlocked,    // HTTP 429
    Unavailable,  // private / deleted / geo-blocked / bad URL
    Network,      // anything else nonzero-exit
    Timeout,      // proc-level timeout (set by caller, not classify_stderr)
    NoYtdlp,      // binary missing and download failed (set by caller)
    ParseFailed,  // info.json / json3 unreadable (set by caller)
};

inline const char* error_label(Err e) {
    switch (e) {
        case Err::None:        return "ok";
        case Err::IpBlocked:   return "ip blocked (429)";
        case Err::Unavailable: return "video unavailable";
        case Err::Network:     return "network error";
        case Err::Timeout:     return "timed out";
        case Err::NoYtdlp:     return "yt-dlp missing";
        case Err::ParseFailed: return "parse failed";
    }
    return "unknown";
}

inline Err classify_stderr(const std::string& e) {
    auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
    if (has("HTTP Error 429") || has("Too Many Requests")) return Err::IpBlocked;
    if (has("Private video") || has("Video unavailable") ||
        has("This video is unavailable") || has("members-only") ||
        has("removed by the uploader") || has("is not a valid URL") ||
        has("Unsupported URL") || has("not available in your country"))
        return Err::Unavailable;
    return Err::Network;
}

// For naming .txt docs when there is no mp4 path to mirror (--no-video).
// Mirrors yt-dlp --windows-filenames closely enough for standalone docs.
inline std::string sanitize_filename(std::string s) {
    static const std::string bad = "<>:\"/\\|?*";
    for (char& c : s)
        if (bad.find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    if (s.empty()) s = "untitled";
    return s;
}

inline const char* FORMAT_MERGED =
    "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best";
inline const char* FORMAT_SINGLE = "best[ext=mp4]/best";

// Phase 1: metadata + subtitles only, keyed by video id in workdir.
// Produces <workdir>/<id>.info.json and <workdir>/<id>.<lang>.json3.
inline std::vector<std::string> metadata_cmd(const std::string& ytdlp_path,
                                             const std::string& video_id,
                                             const std::string& workdir,
                                             const std::string& proxy) {
    std::vector<std::string> c = {
        ytdlp_path, "https://www.youtube.com/watch?v=" + video_id,
        "--no-playlist", "--no-progress", "--no-warnings", "--skip-download",
        "--write-info-json", "--write-subs", "--write-auto-subs",
        "--sub-langs", "en.*,en", "--sub-format", "json3",
        "--paths", workdir, "--output", "%(id)s.%(ext)s",
    };
    if (!proxy.empty()) { c.push_back("--proxy"); c.push_back(proxy); }
    return c;
}

// Phase 2: the actual download. Reuses phase-1 metadata via --load-info-json
// (no second webpage fetch); falls back to the URL when info_json_path is "".
// Prints the final media file path on stdout (last line).
inline std::vector<std::string> download_cmd(const std::string& ytdlp_path,
                                             const std::string& video_id,
                                             const std::string& info_json_path,
                                             const std::string& outdir,
                                             const std::string& fmt,
                                             bool audio_only,
                                             const std::string& audio_format,
                                             const std::string& proxy) {
    std::vector<std::string> c = {ytdlp_path};
    if (!info_json_path.empty()) {
        c.push_back("--load-info-json");
        c.push_back(info_json_path);
    } else {
        c.push_back("https://www.youtube.com/watch?v=" + video_id);
    }
    c.insert(c.end(), {
        "--no-playlist", "--no-progress", "--no-warnings",
        "--windows-filenames", "--no-overwrites",
        "--paths", outdir, "--output", "%(title)s.%(ext)s",
        "--print", "filename",
        "--no-simulate", "--print", "after_move:filepath",
    });
    if (audio_only) {
        c.push_back("--extract-audio");
        if (!audio_format.empty())
            c.insert(c.end(), {"--audio-format", audio_format, "--audio-quality", "0"});
    } else {
        c.insert(c.end(), {"--format", fmt});
    }
    if (!proxy.empty()) { c.push_back("--proxy"); c.push_back(proxy); }
    return c;
}

}  // namespace ytdlp
