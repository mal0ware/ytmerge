// doc.hpp — video metadata model + transcript-document composition. Pure.

#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "core.hpp"
#include "transcript.hpp"

namespace doc {

struct VideoMeta {
    std::string id, title, channel, upload_date, description;
    long long   duration_s = 0, view_count = -1, like_count = -1;
    std::vector<std::string>         tags;
    std::vector<transcript::Chapter> chapters;
};

// nlohmann's .value() throws on present-but-null keys; these helpers don't.
inline std::string jstr(const core::json& j, const char* k) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
}
inline long long jint(const core::json& j, const char* k, long long def) {
    return (j.contains(k) && j[k].is_number()) ? j[k].get<long long>() : def;
}

inline VideoMeta parse_info_json(const core::json& info) {
    VideoMeta m;
    m.id      = jstr(info, "id");
    m.title   = jstr(info, "title");
    m.channel = jstr(info, "channel");
    if (m.channel.empty()) m.channel = jstr(info, "uploader");
    const std::string d = jstr(info, "upload_date");  // "YYYYMMDD"
    if (d.size() == 8)
        m.upload_date = d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2);
    m.duration_s  = jint(info, "duration", 0);
    m.view_count  = jint(info, "view_count", -1);
    m.like_count  = jint(info, "like_count", -1);
    m.description = jstr(info, "description");
    if (info.contains("tags") && info["tags"].is_array())
        for (const auto& t : info["tags"])
            if (t.is_string()) m.tags.push_back(t.get<std::string>());
    if (info.contains("chapters") && info["chapters"].is_array())
        for (const auto& c : info["chapters"]) {
            transcript::Chapter ch;
            ch.start_s = (c.contains("start_time") && c["start_time"].is_number())
                             ? c["start_time"].get<double>() : 0.0;
            ch.title   = jstr(c, "title");
            m.chapters.push_back(std::move(ch));
        }
    return m;
}

inline std::string format_duration(long long s) {
    const long long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, sec);
    else       std::snprintf(buf, sizeof buf, "%lld:%02lld", m, sec);
    return buf;
}

}  // namespace doc
