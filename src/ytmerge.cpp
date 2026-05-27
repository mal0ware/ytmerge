// ytmerge — YouTube URLs in → merged transcripts on the clipboard.
//
// Sources, in priority order:
//   1. CLI args:      ytmerge URL [URL ...]
//   2. Stdin:         cat urls.txt | ytmerge -
//   3. Clipboard:     ytmerge   (default; works from Shortcuts.app)
//
// Flags:
//   --proxy URL              single proxy for all requests
//   --proxy-list FILE        one proxy per line; round-robin, cooldown on 429
//   --workers N              parallel worker count (default: 4)
//   --no-cache               skip the disk cache for this run
//   -h, --help               print usage and exit
//
// Cache: ~/.cache/ytmerge/<video_id>.txt, evicted after CACHE_TTL_DAYS.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
  #include <windows.h>
  // MSVC names these with leading underscore; MSYS2/mingw uses POSIX names.
  #if !defined(__MINGW32__) && !defined(__MINGW64__)
    #define popen  _popen
    #define pclose _pclose
  #endif
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;
using clk    = std::chrono::steady_clock;

// ─── Constants ──────────────────────────────────────────────────────────────

constexpr int  DEFAULT_WORKERS  = 4;
constexpr int  CACHE_TTL_DAYS   = 7;
constexpr int  PROXY_COOLDOWN_S = 600;        // 10 min cooldown on 429
constexpr long HTTP_TIMEOUT_S   = 20;

constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

const std::string DIVIDER(80, '=');

// ─── Error model ────────────────────────────────────────────────────────────

enum class FetchError {
    None,
    IpBlocked,            // HTTP 429 on timedtext
    Network,              // libcurl error or non-2xx other than 429
    Unavailable,          // playabilityStatus != OK (private/age-gated/removed)
    TranscriptsDisabled,  // no captionTracks in player response
    NoEnglishTrack,       // captionTracks present but none for English
    ParseFailed,          // malformed response
};

const char* error_label(FetchError e) {
    switch (e) {
        case FetchError::None:                return "ok";
        case FetchError::IpBlocked:           return "ip blocked (429)";
        case FetchError::Network:             return "network error";
        case FetchError::Unavailable:         return "video unavailable";
        case FetchError::TranscriptsDisabled: return "no captions on video";
        case FetchError::NoEnglishTrack:      return "no english track";
        case FetchError::ParseFailed:         return "parse failed";
    }
    return "unknown";
}

// ─── HTTP client ────────────────────────────────────────────────────────────

struct HttpResponse {
    long        status = 0;
    std::string body;
    CURLcode    curl_code = CURLE_OK;
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// Per-thread libcurl easy handle. Each worker thread keeps one.
class HttpClient {
public:
    HttpClient() : handle_(curl_easy_init()) {}
    ~HttpClient() { if (handle_) curl_easy_cleanup(handle_); }
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse get(const std::string& url, const std::string& proxy = "") {
        HttpResponse r;
        if (!handle_) { r.curl_code = CURLE_FAILED_INIT; return r; }

        curl_easy_reset(handle_);
        curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle_, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle_, CURLOPT_TIMEOUT, HTTP_TIMEOUT_S);
        curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);  // thread safety
        curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(handle_, CURLOPT_ACCEPT_ENCODING, "");  // accept gzip
        if (!proxy.empty()) {
            curl_easy_setopt(handle_, CURLOPT_PROXY, proxy.c_str());
        }
        // libcurl reads HTTPS_PROXY/ALL_PROXY env vars when CURLOPT_PROXY unset.

        r.curl_code = curl_easy_perform(handle_);
        if (r.curl_code == CURLE_OK) {
            curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &r.status);
        }
        return r;
    }

private:
    CURL* handle_;
};

// ─── Proxy pool ─────────────────────────────────────────────────────────────
//
// Round-robin pool with per-proxy cooldown. On 429 we mark the proxy as
// cooling-down for PROXY_COOLDOWN_S seconds and pick the next live entry.
// An empty pool means "direct connection".

class ProxyPool {
public:
    explicit ProxyPool(std::vector<std::string> proxies)
        : proxies_(std::move(proxies)), cooldown_until_(proxies_.size()) {}

    bool empty() const { return proxies_.empty(); }

    // Returns next live proxy URL, or "" if pool is empty / all cooling down
    // (in which case caller can either wait or go direct).
    std::string next() {
        if (proxies_.empty()) return "";
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = clk::now();
        for (size_t tries = 0; tries < proxies_.size(); ++tries) {
            size_t i = cursor_++ % proxies_.size();
            if (cooldown_until_[i] <= now) {
                return proxies_[i];
            }
        }
        return "";  // everything cooling down
    }

    void mark_blocked(const std::string& proxy_url) {
        if (proxy_url.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t i = 0; i < proxies_.size(); ++i) {
            if (proxies_[i] == proxy_url) {
                cooldown_until_[i] = clk::now() + std::chrono::seconds(PROXY_COOLDOWN_S);
                return;
            }
        }
    }

private:
    std::vector<std::string>      proxies_;
    std::vector<clk::time_point>  cooldown_until_;
    std::mutex                    mu_;
    size_t                        cursor_ = 0;
};

// ─── Input ──────────────────────────────────────────────────────────────────

struct Args {
    std::vector<std::string> urls;
    std::string              proxy;
    std::string              proxy_list_path;
    int                      workers   = DEFAULT_WORKERS;
    bool                     no_cache  = false;
    bool                     from_stdin = false;
    bool                     help      = false;
};

const char* USAGE =
    "usage: ytmerge [URL ...] | ytmerge - | ytmerge\n"
    "\n"
    "Sources (in priority order):\n"
    "  CLI args        ytmerge \"URL1\" \"URL2\"   (quote URLs in zsh)\n"
    "  Stdin           cat urls.txt | ytmerge -\n"
    "  Clipboard       ytmerge                  (default)\n"
    "\n"
    "Options:\n"
    "  --proxy URL          single proxy for all requests\n"
    "  --proxy-list FILE    one proxy per line; round-robin with cooldown on 429\n"
    "  --workers N          parallel workers (default 4)\n"
    "  --no-cache           skip disk cache for this run\n"
    "  -h, --help           this message\n"
    "\n"
    "Output is merged transcripts on the clipboard.\n";

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "-h" || s == "--help") { a.help = true; }
        else if (s == "-")              { a.from_stdin = true; }
        else if (s == "--no-cache")     { a.no_cache = true; }
        else if (s == "--proxy" && i + 1 < argc)      { a.proxy = argv[++i]; }
        else if (s == "--proxy-list" && i + 1 < argc) { a.proxy_list_path = argv[++i]; }
        else if (s == "--workers" && i + 1 < argc)    { a.workers = std::max(1, std::atoi(argv[++i])); }
        else                                          { a.urls.push_back(std::move(s)); }
    }
    return a;
}

std::string slurp_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// Execute a process, capture stdout. Used for pbpaste / pbcopy / osascript.
std::string run_capture(const std::vector<std::string>& cmd, const std::string& stdin_data = "") {
    if (cmd.empty()) return "";
    std::ostringstream cmdline;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) cmdline << ' ';
        cmdline << '\'';
        for (char c : cmd[i]) {
            if (c == '\'') cmdline << "'\\''";
            else           cmdline << c;
        }
        cmdline << '\'';
    }
    const std::string mode = stdin_data.empty() ? "r" : "w";
    FILE* pipe = popen(cmdline.str().c_str(), mode.c_str());
    if (!pipe) return "";
    if (!stdin_data.empty()) {
        std::fwrite(stdin_data.data(), 1, stdin_data.size(), pipe);
        pclose(pipe);
        return "";
    }
    std::string out;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof(buf), pipe)) {
        out.append(buf, n);
    }
    pclose(pipe);
    return out;
}

// ─── Platform-specific I/O ─────────────────────────────────────────────────
//
// Clipboard and notifications are the only spots where this program isn't
// portable; everything else is plain C++20 + libcurl. We branch on compile
// flags rather than runtime probing because the implementations have nothing
// in common.

#if defined(__APPLE__)

std::string read_clipboard() { return run_capture({"/usr/bin/pbpaste"}); }
void write_clipboard(const std::string& s) { run_capture({"/usr/bin/pbcopy"}, s); }

void notify(const std::string& msg) {
    std::string safe = msg;
    std::replace(safe.begin(), safe.end(), '"', '\'');
    const std::string script =
        "display notification \"" + safe + "\" with title \"ytmerge\"";
    run_capture({"/usr/bin/osascript", "-e", script});
    std::cerr << msg << '\n';
}

#elif defined(__linux__)

// Try Wayland's wl-paste/wl-copy first if WAYLAND_DISPLAY is set, falling back
// to xclip (most common on X11) and then xsel. notify-send for the toast.
std::string read_clipboard() {
    if (std::getenv("WAYLAND_DISPLAY")) {
        std::string s = run_capture({"wl-paste", "--no-newline"});
        if (!s.empty()) return s;
    }
    std::string s = run_capture({"xclip", "-selection", "clipboard", "-o"});
    if (!s.empty()) return s;
    return run_capture({"xsel", "--clipboard", "--output"});
}

void write_clipboard(const std::string& s) {
    if (std::getenv("WAYLAND_DISPLAY")) {
        run_capture({"wl-copy"}, s);
        return;
    }
    run_capture({"xclip", "-selection", "clipboard", "-i"}, s);
}

void notify(const std::string& msg) {
    run_capture({"notify-send", "ytmerge", msg});
    std::cerr << msg << '\n';
}

#elif defined(_WIN32)

// Win32 clipboard API — needs UTF-16 marshalling because Windows clipboard
// works in wide characters for text.
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), out.data(), wlen);
    return out;
}

static std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string read_clipboard() {
    if (!OpenClipboard(nullptr)) return "";
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return ""; }
    auto* wide = static_cast<const wchar_t*>(GlobalLock(h));
    std::string out = wide ? wide_to_utf8(wide) : "";
    if (wide) GlobalUnlock(h);
    CloseClipboard();
    return out;
}

void write_clipboard(const std::string& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    std::wstring wide = utf8_to_wide(s);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        auto* dst = static_cast<wchar_t*>(GlobalLock(h));
        if (dst) {
            std::memcpy(dst, wide.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

void notify(const std::string& msg) {
    // No native toast without UWP/COM glue, which isn't worth carrying.
    // Status goes to stderr; users see it in the terminal or Shortcuts log.
    std::cerr << msg << '\n';
}

#else
  #error "Unsupported platform — add clipboard/notify for this OS in src/ytmerge.cpp"
#endif

// ─── Video ID extraction ────────────────────────────────────────────────────

std::vector<std::string> extract_video_ids(const std::string& text) {
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

std::optional<std::string> extract_balanced_json(std::string_view html, std::string_view anchor) {
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

// ─── YouTube transcript fetch ───────────────────────────────────────────────

struct FetchResult {
    FetchError  error = FetchError::None;
    std::string title;
    std::string transcript;
};

// Parse the JSON3 caption format YouTube returns:
//   { "events": [ { "segs": [ { "utf8": "Hello" }, ... ], ... }, ... ] }
std::string parse_json3_transcript(const json& doc) {
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

// Resolve the English caption track URL from a player response.
std::optional<std::string> pick_english_track(const json& player) {
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

FetchResult fetch_one(HttpClient& http, ProxyPool& pool, const std::string& video_id) {
    FetchResult fr;

    // Only retry when there's a non-empty proxy pool to rotate through; with
    // no proxies, retrying just bangs the same rate-limited IP repeatedly.
    const int max_attempts = pool.empty() ? 1 : 3;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const std::string proxy = pool.next();

        // 1) Watch page → ytInitialPlayerResponse
        const std::string watch_url =
            "https://www.youtube.com/watch?v=" + video_id;
        auto watch = http.get(watch_url, proxy);
        if (watch.curl_code != CURLE_OK || watch.status / 100 != 2) {
            if (watch.status == 429) { pool.mark_blocked(proxy); continue; }
            fr.error = FetchError::Network;
            return fr;
        }

        auto player_json_text = extract_balanced_json(
            watch.body, "ytInitialPlayerResponse");
        if (!player_json_text) {
            fr.error = FetchError::ParseFailed;
            return fr;
        }

        json player;
        try { player = json::parse(*player_json_text); }
        catch (...) { fr.error = FetchError::ParseFailed; return fr; }

        // playabilityStatus.status == "OK" means the video is accessible.
        if (player.contains("playabilityStatus")) {
            const std::string status = player["playabilityStatus"]
                .value("status", "");
            if (!status.empty() && status != "OK") {
                fr.error = FetchError::Unavailable;
                return fr;
            }
        }

        if (player.contains("videoDetails")) {
            fr.title = player["videoDetails"].value("title", "");
        }

        auto track_url = pick_english_track(player);
        if (!track_url) {
            // Distinguish: no captions at all vs. captions but none in English.
            const bool has_any_tracks =
                player.contains("captions") &&
                player["captions"]
                    .value("playerCaptionsTracklistRenderer", json::object())
                    .contains("captionTracks");
            fr.error = has_any_tracks ? FetchError::NoEnglishTrack
                                      : FetchError::TranscriptsDisabled;
            return fr;
        }

        // 2) Fetch the timedtext JSON3 — this is the rate-limited endpoint.
        auto caps = http.get(*track_url, proxy);
        if (caps.curl_code != CURLE_OK || caps.status / 100 != 2) {
            if (caps.status == 429) { pool.mark_blocked(proxy); continue; }
            fr.error = FetchError::Network;
            return fr;
        }

        json doc;
        try { doc = json::parse(caps.body); }
        catch (...) { fr.error = FetchError::ParseFailed; return fr; }

        fr.transcript = parse_json3_transcript(doc);
        if (fr.transcript.empty()) { fr.error = FetchError::ParseFailed; return fr; }

        if (fr.title.empty()) fr.title = video_id;
        return fr;
    }

    fr.error = FetchError::IpBlocked;
    return fr;
}

// ─── Disk cache ─────────────────────────────────────────────────────────────
//
// Layout: $HOME/.cache/ytmerge/<video_id>.txt
// Format:
//   first line: title
//   second line: blank
//   remainder:  transcript
// We use file mtime for TTL eviction.

fs::path cache_dir() {
#if defined(_WIN32)
    // Standard Windows app-data location for per-user caches.
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base) base = std::getenv("USERPROFILE");
    if (!base) return {};
    return fs::path(base) / "ytmerge" / "cache";
#else
    // XDG_CACHE_HOME wins on Linux when set; macOS falls through to ~/.cache,
    // which keeps the path identical between the two POSIX targets.
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "ytmerge";
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) / ".cache" / "ytmerge";
#endif
}

std::optional<FetchResult> cache_read(const std::string& video_id) {
    auto dir = cache_dir();
    if (dir.empty()) return std::nullopt;
    fs::path p = dir / (video_id + ".txt");
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;

    // TTL check via mtime.
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    const auto age =
        decltype(ftime)::clock::now() - ftime;
    if (age > std::chrono::hours(24 * CACHE_TTL_DAYS)) {
        fs::remove(p, ec);
        return std::nullopt;
    }

    std::ifstream f(p);
    if (!f) return std::nullopt;
    FetchResult r;
    std::getline(f, r.title);
    std::string blank;
    std::getline(f, blank);
    std::ostringstream rest;
    rest << f.rdbuf();
    r.transcript = rest.str();
    if (r.transcript.empty()) return std::nullopt;
    return r;
}

void cache_write(const std::string& video_id, const FetchResult& r) {
    auto dir = cache_dir();
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / (video_id + ".txt"), std::ios::trunc);
    if (!f) return;
    f << r.title << "\n\n" << r.transcript;
}

void cache_evict_old() {
    auto dir = cache_dir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    const auto now = decltype(fs::file_time_type::clock::now()) {fs::file_time_type::clock::now()};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ftime = fs::last_write_time(entry, ec);
        if (ec) continue;
        if ((now - ftime) > std::chrono::hours(24 * CACHE_TTL_DAYS)) {
            fs::remove(entry, ec);
        }
    }
}

// ─── Worker pool ────────────────────────────────────────────────────────────

struct Job {
    size_t      index;
    std::string video_id;
};

struct OutputSlot {
    std::string video_id;
    FetchResult result;
};

class WorkQueue {
public:
    void push(Job j)                 { { std::lock_guard l(m_); q_.push(std::move(j)); } cv_.notify_one(); }
    void close()                     { { std::lock_guard l(m_); closed_ = true;       } cv_.notify_all(); }
    std::optional<Job> pop() {
        std::unique_lock l(m_);
        cv_.wait(l, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        Job j = std::move(q_.front());
        q_.pop();
        return j;
    }
private:
    std::queue<Job>          q_;
    std::mutex               m_;
    std::condition_variable  cv_;
    bool                     closed_ = false;
};

// ─── Proxy list loader ──────────────────────────────────────────────────────

std::vector<std::string> load_proxy_list(const std::string& path) {
    std::vector<std::string> out;
    if (path.empty()) return out;
    std::ifstream f(path);
    if (!f) {
        std::cerr << "warning: could not open proxy list: " << path << '\n';
        return out;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Strip whitespace + skip blanks and #comments.
        size_t a = line.find_first_not_of(" \t\r\n");
        size_t b = line.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        std::string s = line.substr(a, b - a + 1);
        if (s.empty() || s[0] == '#') continue;
        out.push_back(s);
    }
    return out;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    if (args.help) {
        std::cout << USAGE;
        return 0;
    }

    // Gather raw input text.
    std::string raw;
    std::string source;
    if (!args.urls.empty()) {
        for (size_t i = 0; i < args.urls.size(); ++i) {
            if (i) raw += ' ';
            raw += args.urls[i];
        }
        source = "args";
    } else if (args.from_stdin) {
        raw = slurp_stdin();
        source = "stdin";
    } else {
        raw = read_clipboard();
        source = "clipboard";
    }

    if (raw.empty() || raw.find_first_not_of(" \t\r\n") == std::string::npos) {
        notify(source + " is empty");
        return 1;
    }

    auto ids = extract_video_ids(raw);
    if (ids.empty()) {
        notify("no YouTube URLs found in " + source);
        return 1;
    }

    // Build proxy pool. CLI --proxy is preferred over --proxy-list (use one or
    // the other; a single proxy is just a pool of size 1 for our purposes).
    std::vector<std::string> proxy_list;
    if (!args.proxy.empty())                proxy_list.push_back(args.proxy);
    else if (!args.proxy_list_path.empty()) proxy_list = load_proxy_list(args.proxy_list_path);
    ProxyPool pool(std::move(proxy_list));

    if (!args.no_cache) cache_evict_old();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Per-id slots, keyed by index so output order matches input order.
    std::vector<OutputSlot> results(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) results[i].video_id = ids[i];

    // Cache pass first — fills slots that hit, queues the rest.
    WorkQueue q;
    std::atomic<size_t> remaining{0};
    std::cerr << "found " << ids.size() << " videos. fetching...\n";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (!args.no_cache) {
            if (auto cached = cache_read(ids[i])) {
                results[i].result = std::move(*cached);
                std::cerr << "  [" << (i + 1) << "/" << ids.size()
                          << "] " << ids[i] << "  (cache)\n";
                continue;
            }
        }
        q.push({i, ids[i]});
        ++remaining;
    }

    // Worker pool — workers default to args.workers, but capped at remaining
    // job count (no point spawning 4 threads for 1 video).
    const int n_workers =
        std::max(1, std::min<int>(args.workers, static_cast<int>(remaining.load())));
    std::vector<std::thread> workers;
    std::mutex log_mu;
    for (int w = 0; w < n_workers; ++w) {
        workers.emplace_back([&]() {
            HttpClient http;
            while (true) {
                auto job = q.pop();
                if (!job) return;
                FetchResult fr = fetch_one(http, pool, job->video_id);
                {
                    std::lock_guard l(log_mu);
                    std::cerr << "  [" << (job->index + 1) << "/" << ids.size()
                              << "] " << job->video_id;
                    if (fr.error == FetchError::None) std::cerr << "  ok\n";
                    else std::cerr << "  FAIL: " << error_label(fr.error) << '\n';
                }
                results[job->index].result = std::move(fr);
                if (results[job->index].result.error == FetchError::None && !args.no_cache) {
                    cache_write(job->video_id, results[job->index].result);
                }
            }
        });
    }
    q.close();
    for (auto& t : workers) t.join();
    curl_global_cleanup();

    // Build output blob in original input order.
    std::vector<std::string> blocks;
    std::unordered_map<std::string, int> failure_counts;
    int ok = 0;
    for (const auto& slot : results) {
        const auto& r = slot.result;
        if (r.error != FetchError::None) {
            failure_counts[error_label(r.error)]++;
            continue;
        }
        const std::string url = "https://www.youtube.com/watch?v=" + slot.video_id;
        std::ostringstream blk;
        blk << DIVIDER << '\n'
            << "TITLE: " << (r.title.empty() ? slot.video_id : r.title) << '\n'
            << "URL:   " << url << '\n'
            << DIVIDER << "\n\n"
            << r.transcript << '\n';
        blocks.push_back(blk.str());
        ++ok;
    }

    if (blocks.empty()) {
        std::string detail;
        for (const auto& [label, n] : failure_counts) {
            if (!detail.empty()) detail += ", ";
            detail += std::to_string(n) + " " + label;
        }
        notify("all " + std::to_string(ids.size()) + " failed — " + detail);
        // Surface the IP-block remediation hint when that's the cause.
        bool ip_blocked = false;
        for (const auto& slot : results)
            if (slot.result.error == FetchError::IpBlocked) { ip_blocked = true; break; }
        if (ip_blocked) {
            std::cerr <<
                "  hint: YouTube has rate-limited this IP. Either:\n"
                "    - switch network (phone hotspot, VPN, different wifi)\n"
                "    - wait a few hours\n"
                "    - re-run with --proxy URL or --proxy-list FILE\n";
        }
        return 1;
    }

    std::string out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out += "\n\n";
        out += blocks[i];
    }
    write_clipboard(out);

    std::ostringstream summary;
    summary << "ok " << ok << " transcripts copied (" << out.size() << " chars)";
    if (!failure_counts.empty()) {
        summary << "  [";
        bool first = true;
        for (const auto& [label, n] : failure_counts) {
            if (!first) summary << ", ";
            summary << n << " " << label;
            first = false;
        }
        summary << "]";
    }
    notify(summary.str());
    return 0;
}
