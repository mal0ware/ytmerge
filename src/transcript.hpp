// transcript.hpp — pure json3-subtitle → timestamped-transcript logic.
// No I/O, no network; unit-tested in tests/test_v2.cpp.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core.hpp"

namespace transcript {

struct Cue {
    double      start_s = 0.0;
    std::string text;
};

struct Chapter {
    double      start_s = 0.0;
    std::string title;
};

// Parse yt-dlp's json3 subtitle format: {"events":[{"tStartMs":N,"segs":[{"utf8":...}]}]}.
// Styling-only events (no segs), empty/whitespace cues, and consecutive
// duplicates (auto-caption rolling windows) are dropped.
inline std::vector<Cue> parse_json3_cues(const core::json& doc) {
    std::vector<Cue> cues;
    if (!doc.contains("events") || !doc["events"].is_array()) return cues;
    for (const auto& ev : doc["events"]) {
        if (!ev.contains("segs") || !ev["segs"].is_array()) continue;
        if (!ev.contains("tStartMs") || !ev["tStartMs"].is_number()) continue;
        std::string raw;
        for (const auto& seg : ev["segs"]) {
            if (seg.contains("utf8") && seg["utf8"].is_string())
                raw += seg["utf8"].get<std::string>();
        }
        for (char& c : raw)
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        std::string text;
        bool prev_space = false;
        for (char c : raw) {
            if (c == ' ') {
                if (!prev_space && !text.empty()) text += ' ';
                prev_space = true;
            } else {
                text += c;
                prev_space = false;
            }
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (text.empty()) continue;
        if (!cues.empty() && cues.back().text == text) continue;
        cues.push_back({ev["tStartMs"].get<double>() / 1000.0, std::move(text)});
    }
    return cues;
}

// "[m:ss]" below one hour, "[h:mm:ss]" at or above.
inline std::string format_timestamp(double seconds) {
    const long long s = static_cast<long long>(seconds);
    const long long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "[%lld:%02lld:%02lld]", h, m, sec);
    else       std::snprintf(buf, sizeof buf, "[%lld:%02lld]", m, sec);
    return buf;
}

// Cues joined into paragraphs. A new paragraph (blank line + timestamp) starts
// at every chapter boundary and whenever paragraph_interval_s seconds of
// speech have accumulated since the current paragraph began.
inline std::string render_transcript(const std::vector<Cue>& cues,
                                     const std::vector<Chapter>& chapters,
                                     double paragraph_interval_s = 45.0) {
    std::string out;
    size_t next_chapter = 0;
    double para_start   = 0.0;
    bool   first        = true;
    for (const auto& c : cues) {
        bool brk = first;
        if (!first && c.start_s - para_start >= paragraph_interval_s) brk = true;
        while (next_chapter < chapters.size() &&
               c.start_s >= chapters[next_chapter].start_s - 0.001) {
            if (!first) brk = true;
            ++next_chapter;
        }
        if (brk || first) {
            if (!first) out += "\n\n";
            out += format_timestamp(c.start_s);
            out += ' ';
            para_start = c.start_s;
            first = false;
        } else {
            out += ' ';
        }
        out += c.text;
    }
    return out;
}

}  // namespace transcript
