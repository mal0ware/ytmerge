// cli.hpp — argument parsing for ytmerge v2. Pure aside from the injectable
// file-existence probe (tests stub it; the app passes the real one).

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace cli {

struct Args {
    std::vector<std::string> urls;
    std::string input_file;
    std::string proxy, proxy_list_path;
    std::string out_dir = "downloaded";
    int  workers = 4;
    bool no_cache = false, from_stdin = false, help = false;
    bool no_video = false, audio = false, mp3 = false, do_update = false;
};

using FileExistsFn = bool (*)(const std::string&);

inline bool default_file_exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

inline const char* USAGE =
    "usage: ytmerge [links.txt | URL ...] | ytmerge - | ytmerge\n"
    "\n"
    "Sources (in priority order):\n"
    "  File            ytmerge links.txt        (any text file containing URLs)\n"
    "  CLI args        ytmerge \"URL1\" \"URL2\"    (quote URLs in zsh)\n"
    "  Stdin           cat urls.txt | ytmerge -\n"
    "  Clipboard       ytmerge                  (default; auto-uses ./links.txt\n"
    "                                            when the clipboard has no URLs)\n"
    "\n"
    "For each video: <out>/<title>.mp4 and <out>/<title>.txt (metadata header +\n"
    "timestamped transcript). Merged transcripts also go to the clipboard.\n"
    "\n"
    "Options:\n"
    "  --no-video           transcripts + metadata only (no download)\n"
    "  --audio              audio-only download (best available format)\n"
    "  --mp3                audio-only download, converted to mp3\n"
    "  --out DIR            output directory (default: downloaded)\n"
    "  --workers N          parallel workers (default 4)\n"
    "  --proxy URL          single proxy for all requests\n"
    "  --proxy-list FILE    one proxy per line; round-robin with cooldown on 429\n"
    "  --no-cache           skip disk cache for this run\n"
    "  --update             force a yt-dlp self-update check now\n"
    "  -h, --help           this message\n";

inline Args parse_args(int argc, char** argv,
                       FileExistsFn file_exists = &default_file_exists) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if      (s == "-h" || s == "--help") a.help = true;
        else if (s == "-")                   a.from_stdin = true;
        else if (s == "--no-cache")          a.no_cache = true;
        else if (s == "--no-video")          a.no_video = true;
        else if (s == "--audio")             a.audio = true;
        else if (s == "--mp3")               a.mp3 = true;
        else if (s == "--update")            a.do_update = true;
        else if (s == "--out" && i + 1 < argc)        a.out_dir = argv[++i];
        else if (s == "--proxy" && i + 1 < argc)      a.proxy = argv[++i];
        else if (s == "--proxy-list" && i + 1 < argc) a.proxy_list_path = argv[++i];
        else if (s == "--workers" && i + 1 < argc)
            a.workers = std::max(1, std::atoi(argv[++i]));
        else {
            const bool looks_like_url =
                s.find("youtube.com") != std::string::npos ||
                s.find("youtu.be")    != std::string::npos;
            if (!looks_like_url && a.input_file.empty() && file_exists(s))
                a.input_file = s;
            else
                a.urls.push_back(s);
        }
    }
    return a;
}

}  // namespace cli
