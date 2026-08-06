// ytmerge — YouTube URLs in → per-video mp4 + transcript docs, merged
// transcripts on the clipboard. Downloads and metadata are driven by yt-dlp
// (bootstrapped automatically); parsing/composition logic lives in the
// headers below so it can be unit-tested without the network.
//
// Sources, in priority order:
//   1. File:        ytmerge links.txt   (any text file containing URLs)
//   2. CLI args:     ytmerge URL [URL ...]
//   3. Stdin:        cat urls.txt | ytmerge -
//   4. Clipboard:    ytmerge   (default; auto-uses ./links.txt when the
//                    clipboard has no URLs)
//
// Flags:
//   --no-video                transcripts + metadata only (no download)
//   --audio                   audio-only download (best available format)
//   --mp3                     audio-only download, converted to mp3
//   --out DIR                 output directory (default: downloaded)
//   --proxy URL                single proxy for all requests
//   --proxy-list FILE          one proxy per line; round-robin, cooldown on 429
//   --workers N                parallel worker count (default: 4)
//   --no-cache                 skip the disk cache for this run
//   --update                   force a yt-dlp self-update check now
//   -h, --help                 print usage and exit
//
// Cache: <cache dir>/<video_id>.txt, evicted after CACHE_TTL_DAYS.

// Must precede ALL includes: curl/curl.h drags in windows.h on Win32, and its
// min/max macros would otherwise break std::min/std::max.
#if defined(_WIN32) && !defined(NOMINMAX)
  #define NOMINMAX
#endif

#include <algorithm>
#include <array>
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
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "core.hpp"
#include "cli.hpp"
#include "deps.hpp"
#include "doc.hpp"
#include "proc.hpp"
#include "transcript.hpp"
#include "ytdlp.hpp"

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

constexpr int  PROXY_COOLDOWN_S = 600;        // 10 min cooldown on 429
constexpr long HTTP_TIMEOUT_S   = 20;

constexpr const char* USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

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

std::string slurp_stdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// Execute a process, capture stdout. Used for pbpaste / pbcopy / osascript.
//
// Deliberately popen()-based: every argument is wrapped in single quotes with
// embedded quotes rewritten as '\'' — the canonical POSIX-shell escape, under
// which no other character is special — so the shell cannot reinterpret any
// argv element. All call sites pass fixed program names and flags (only
// notification text ever varies), so nothing user-controlled picks the
// command. Swapping in posix_spawn + explicit pipes would drop the shell
// entirely, but needs manual fd plumbing and waitpid handling on two
// platforms for no observable behavior change; not worth it at this size.
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

// ─── Disk cache v2 ──────────────────────────────────────────────────────────
// <cache>/<id>.txt now stores the entire composed document. v1 entries
// (title\n\ntranscript) simply miss extract_title and get refetched.

std::optional<std::string> cache_read_doc(const std::string& video_id) {
    auto dir = core::cache_dir();
    if (dir.empty()) return std::nullopt;
    fs::path p = core::cache_entry_path(dir, video_id);
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    if (decltype(ftime)::clock::now() - ftime >
        std::chrono::hours(24 * core::CACHE_TTL_DAYS)) {
        fs::remove(p, ec);
        return std::nullopt;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string d = ss.str();
    if (doc::extract_title(d).empty()) return std::nullopt;  // v1-format entry
    return d;
}

void cache_write_doc(const std::string& video_id, const std::string& document) {
    auto dir = core::cache_dir();
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(core::cache_entry_path(dir, video_id),
                    std::ios::trunc | std::ios::binary);
    if (f) f << document;
}

void cache_evict_old() {
    auto dir = core::cache_dir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    const auto now = decltype(fs::file_time_type::clock::now()) {fs::file_time_type::clock::now()};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ftime = fs::last_write_time(entry, ec);
        if (ec) continue;
        if ((now - ftime) > std::chrono::hours(24 * core::CACHE_TTL_DAYS)) {
            fs::remove(entry, ec);
        }
    }
}

// ─── yt-dlp bootstrap ───────────────────────────────────────────────────────

std::optional<fs::path> ensure_ytdlp() {
    if (auto p = deps::find_on_path("yt-dlp")) return p;
    const fs::path dir = deps::bin_dir();
    if (dir.empty()) return std::nullopt;
    const fs::path local = dir / deps::ytdlp_local_name();
    std::error_code ec;
    if (fs::is_regular_file(local, ec)) return local;

    std::cerr << "yt-dlp not found — downloading (one-time, ~15 MB)...\n";
    HttpClient http;
    auto resp = http.get(deps::ytdlp_download_url());
    if (resp.curl_code != CURLE_OK || resp.status / 100 != 2 || resp.body.empty()) {
        std::cerr << "error: could not download yt-dlp (HTTP "
                  << resp.status << ")\n";
        return std::nullopt;
    }
    fs::create_directories(dir, ec);
    {
        std::ofstream f(local, std::ios::trunc | std::ios::binary);
        if (!f) return std::nullopt;
        f.write(resp.body.data(),
                static_cast<std::streamsize>(resp.body.size()));
    }
#if !defined(_WIN32)
    fs::permissions(local,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
        fs::perms::others_read | fs::perms::others_exec,
        fs::perm_options::replace, ec);
#endif
    std::cerr << "yt-dlp installed to " << local.string() << '\n';
    return local;
}

void maybe_update_ytdlp(const fs::path& ytdlp, bool force) {
    const fs::path stamp = deps::bin_dir() / ".last-update-check";
    if (!force && !deps::update_due(stamp)) return;
    std::cerr << "checking for yt-dlp update...\n";
    auto r = proc::run({ytdlp.string(), "-U"}, 300);
    if (!r.out.empty()) std::cerr << r.out;
    std::error_code ec;
    fs::create_directories(stamp.parent_path(), ec);
    std::ofstream f(stamp, std::ios::trunc);  // touch: mtime = now
    f << "";
}

// ─── Per-video pipeline ─────────────────────────────────────────────────────

struct VideoOutcome {
    ytdlp::Err  error = ytdlp::Err::None;  // fatal for this video
    std::string document;                  // composed .txt content (when ok)
    std::string title;
    std::string video_note;                // non-fatal video-download failure
};

// --print after_move:filepath → last non-empty stdout line. Used to derive
// the downloaded media's path (and hence the paired .txt's stem) from
// yt-dlp's own filename sanitization, which differs from sanitize_filename.
static std::string last_nonempty_stdout_line(const std::string& out) {
    std::istringstream ss(out);
    std::string line, last;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) last = line;
    }
    return last;
}

// Find the phase-1 subtitle file: <workdir>/<id>.<lang>.json3
std::optional<fs::path> find_subs_file(const fs::path& workdir,
                                       const std::string& video_id) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(workdir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind(video_id + ".", 0) == 0 &&
            name.size() > 6 && name.substr(name.size() - 6) == ".json3")
            return e.path();
    }
    return std::nullopt;
}

VideoOutcome process_video(const std::string& ytdlp_path,
                           const std::string& video_id,
                           const cli::Args& args,
                           ProxyPool& pool,
                           const std::string& fmt) {
    VideoOutcome out;
    const fs::path workdir = core::cache_dir() / "work";
    std::error_code ec;
    fs::create_directories(workdir, ec);

    // Phase 1: metadata + subs. Retry across proxies on 429 (v1 policy: one
    // attempt when there is no pool to rotate through).
    const int max_attempts = pool.empty() ? 1 : 3;
    proc::Result meta_run;
    std::string used_proxy;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        used_proxy = pool.next();
        meta_run = proc::run(
            ytdlp::metadata_cmd(ytdlp_path, video_id, workdir.string(), used_proxy),
            300);
        if (meta_run.timed_out) { out.error = ytdlp::Err::Timeout; return out; }
        if (meta_run.exit_code == 0) break;
        const auto err = ytdlp::classify_stderr(meta_run.err);
        if (err == ytdlp::Err::IpBlocked) { pool.mark_blocked(used_proxy); continue; }
        out.error = err;
        return out;
    }
    if (meta_run.exit_code != 0) { out.error = ytdlp::Err::IpBlocked; return out; }

    // Parse info.json.
    const fs::path info_path = workdir / (video_id + ".info.json");
    doc::VideoMeta meta;
    {
        std::ifstream f(info_path, std::ios::binary);
        if (!f) { out.error = ytdlp::Err::ParseFailed; return out; }
        json info;
        try { f >> info; }
        catch (...) { out.error = ytdlp::Err::ParseFailed; return out; }
        meta = doc::parse_info_json(info);
    }
    if (meta.id.empty()) meta.id = video_id;
    out.title = meta.title.empty() ? video_id : meta.title;
    // Keep meta.title in sync: an empty TITLE line makes cache_read_doc's
    // v1-sniff reject the cached entry forever.
    if (meta.title.empty()) meta.title = out.title;

    // Parse subtitles (absence is not fatal: doc says "no transcript available").
    std::string transcript_text;
    if (auto subs = find_subs_file(workdir, video_id)) {
        std::ifstream f(*subs, std::ios::binary);
        json j;
        try {
            f >> j;
            transcript_text = transcript::render_transcript(
                transcript::parse_json3_cues(j), meta.chapters);
        } catch (...) { /* leave empty */ }
        fs::remove(*subs, ec);
    }
    out.document = doc::compose_document(meta, transcript_text);

    // Phase 2: media download (skipped by --no-video). Reuses phase-1 metadata.
    fs::path media_path;
    if (!args.no_video) {
        auto dl = proc::run(
            ytdlp::download_cmd(ytdlp_path, video_id, info_path.string(),
                                args.out_dir, fmt,
                                args.audio || args.mp3,
                                args.mp3 ? "mp3" : "",
                                used_proxy),
            1800);
        if (dl.timed_out) {
            out.video_note = "video download timed out";
        } else if (dl.exit_code != 0) {
            out.video_note = std::string("video download failed: ")
                             + ytdlp::error_label(ytdlp::classify_stderr(dl.err));
        } else {
            std::string last = last_nonempty_stdout_line(dl.out);
            if (!last.empty()) media_path = last;
        }
    }

    // Write the .txt next to the media file (same stem) or standalone.
    fs::path doc_path;
    if (!media_path.empty()) {
        doc_path = media_path;
        doc_path.replace_extension(".txt");
    } else {
        fs::create_directories(args.out_dir, ec);
        doc_path = fs::path(args.out_dir)
                   / (ytdlp::sanitize_filename(out.title) + ".txt");
    }
    {
        std::ofstream f(doc_path, std::ios::trunc | std::ios::binary);
        if (f) f << out.document;
    }
    fs::remove(info_path, ec);
    return out;
}

// ─── Worker pool ────────────────────────────────────────────────────────────

struct Job {
    size_t      index;
    std::string video_id;
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
    cli::Args args = cli::parse_args(argc, argv);
    if (args.help) {
        std::cout << cli::USAGE;
        return 0;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto ytdlp_path = ensure_ytdlp();
    if (!ytdlp_path) {
        notify("ytmerge: yt-dlp unavailable — cannot continue");
        curl_global_cleanup();
        return 1;
    }

    // --update with no other input is a valid standalone invocation.
    const bool update_only =
        args.do_update && args.urls.empty() && args.input_file.empty() &&
        !args.from_stdin;

    // Gather raw input text.
    std::string raw, source;
    if (!update_only) {
        if (!args.input_file.empty()) {
            std::ifstream f(args.input_file, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            raw = ss.str();
            source = args.input_file;
        } else if (!args.urls.empty()) {
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
            // Auto-fallback: clipboard empty of URLs but ./links.txt exists.
            if (core::extract_video_ids(raw).empty() &&
                cli::default_file_exists("links.txt")) {
                std::ifstream f("links.txt", std::ios::binary);
                std::ostringstream ss;
                ss << f.rdbuf();
                raw = ss.str();
                source = "links.txt";
            }
        }
    }

    auto ids = core::extract_video_ids(raw);
    if (ids.empty() && !update_only) {
        notify("no YouTube URLs found in " + source);
        curl_global_cleanup();
        return 1;
    }

    // Proxy pool (v1 semantics: --proxy wins over --proxy-list).
    std::vector<std::string> proxy_list;
    if (!args.proxy.empty())                proxy_list.push_back(args.proxy);
    else if (!args.proxy_list_path.empty()) proxy_list = load_proxy_list(args.proxy_list_path);
    ProxyPool pool(std::move(proxy_list));

    // ffmpeg decides merged vs single-file format (spec fallback rule).
    std::string fmt = ytdlp::FORMAT_MERGED;
    if (!args.no_video && !args.audio && !args.mp3 &&
        !deps::find_on_path("ffmpeg")) {
        std::cerr << "warning: ffmpeg not found — falling back to single-file "
                     "format (lower max quality)\n";
        fmt = ytdlp::FORMAT_SINGLE;
    }

    if (!args.no_cache) cache_evict_old();

    // Slots + cache pass. A cache hit still needs the media file (unless
    // --no-video) and the .txt must be re-materialized into args.out_dir
    // every run, so every video is queued; process_video is skipped for
    // cached docs.
    struct Slot { std::string id; VideoOutcome outcome; bool from_cache = false; };
    std::vector<Slot> results(ids.size());
    WorkQueue q;
    size_t queued = 0;
    std::cerr << "found " << ids.size() << " videos. fetching...\n";
    for (size_t i = 0; i < ids.size(); ++i) {
        results[i].id = ids[i];
        if (!args.no_cache) {
            if (auto d = cache_read_doc(ids[i])) {
                results[i].outcome.document = std::move(*d);
                results[i].outcome.title = doc::extract_title(results[i].outcome.document);
                results[i].from_cache = true;
                std::cerr << "  [" << (i + 1) << "/" << ids.size() << "] "
                          << ids[i] << "  (cache)\n";
            }
        }
        q.push({i, ids[i]});
        ++queued;
    }

    const int n_workers = std::max(
        1, std::min<int>(args.workers, static_cast<int>(std::max<size_t>(queued, 1))));
    std::vector<std::thread> workers;
    std::mutex log_mu;
    for (int w = 0; w < n_workers; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                auto job = q.pop();
                if (!job) return;
                Slot& slot = results[job->index];
                if (slot.from_cache) {
                    // Cached doc: only the media download remains (skipped
                    // under --no-video). No info.json on disk — download_cmd
                    // falls back to the URL.
                    fs::path media_path;
                    if (!args.no_video) {
                        auto dl = proc::run(
                            ytdlp::download_cmd(ytdlp_path->string(), slot.id, "",
                                                args.out_dir, fmt,
                                                args.audio || args.mp3,
                                                args.mp3 ? "mp3" : "",
                                                pool.next()),
                            1800);
                        if (dl.timed_out) {
                            slot.outcome.video_note = "video download timed out";
                        } else if (dl.exit_code != 0) {
                            slot.outcome.video_note = std::string("video download failed: ")
                                             + ytdlp::error_label(ytdlp::classify_stderr(dl.err));
                        } else {
                            std::string last = last_nonempty_stdout_line(dl.out);
                            if (!last.empty()) media_path = last;
                        }
                    }
                    // Write the .txt next to the media file (same stem, using
                    // yt-dlp's own sanitization) — mirroring process_video —
                    // or fall back to the standalone name when there is no
                    // media path (--no-video, download failed, or yt-dlp
                    // printed nothing on a skip).
                    std::error_code ec;
                    fs::path doc_path;
                    if (!media_path.empty()) {
                        doc_path = media_path;
                        doc_path.replace_extension(".txt");
                    } else {
                        fs::create_directories(args.out_dir, ec);
                        doc_path = fs::path(args.out_dir)
                            / (ytdlp::sanitize_filename(slot.outcome.title.empty() ? slot.id
                                                                                   : slot.outcome.title)
                               + ".txt");
                    }
                    std::ofstream f(doc_path, std::ios::trunc | std::ios::binary);
                    if (f) f << slot.outcome.document;
                } else {
                    slot.outcome = process_video(ytdlp_path->string(), slot.id,
                                                 args, pool, fmt);
                    if (slot.outcome.error == ytdlp::Err::None && !args.no_cache)
                        cache_write_doc(slot.id, slot.outcome.document);
                }
                std::lock_guard l(log_mu);
                std::cerr << "  [" << (job->index + 1) << "/" << ids.size()
                          << "] " << slot.id;
                if (slot.outcome.error == ytdlp::Err::None) {
                    std::cerr << "  ok";
                    if (!slot.outcome.video_note.empty())
                        std::cerr << "  (" << slot.outcome.video_note << ")";
                    std::cerr << '\n';
                } else {
                    std::cerr << "  FAIL: "
                              << ytdlp::error_label(slot.outcome.error) << '\n';
                }
            }
        });
    }
    q.close();
    for (auto& t : workers) t.join();

    // Merged clipboard blob + summary, input order preserved.
    std::string blob;
    std::unordered_map<std::string, int> failures;
    int ok = 0;
    int media_failed = 0;
    bool ip_blocked = false;
    for (const auto& slot : results) {
        if (slot.outcome.error != ytdlp::Err::None) {
            failures[ytdlp::error_label(slot.outcome.error)]++;
            if (slot.outcome.error == ytdlp::Err::IpBlocked) ip_blocked = true;
            continue;
        }
        if (!slot.outcome.video_note.empty()) ++media_failed;
        if (!blob.empty()) blob += "\n\n";
        blob += slot.outcome.document;
        ++ok;
    }

    if (ok > 0) write_clipboard(blob);

    std::ostringstream summary;
    if (ids.empty())      summary << "update check only";
    else if (ok == 0)     summary << "all " << ids.size() << " failed";
    else                  summary << ok << "/" << ids.size() << " ok — files in "
                                  << args.out_dir << ", transcripts on clipboard";
    if (media_failed > 0) summary << ", " << media_failed << " media failed";
    if (!failures.empty()) {
        summary << "  [";
        bool first = true;
        for (const auto& [label, n] : failures) {
            if (!first) summary << ", ";
            summary << n << " " << label;
            first = false;
        }
        summary << "]";
    }
    notify(summary.str());
    if (ip_blocked) {
        std::cerr <<
            "  hint: YouTube has rate-limited this IP. Either:\n"
            "    - switch network (phone hotspot, VPN, different wifi)\n"
            "    - wait a few hours\n"
            "    - re-run with --proxy URL or --proxy-list FILE\n";
    }

    // Daily (or forced) yt-dlp self-update — after the batch, never before.
    maybe_update_ytdlp(*ytdlp_path, args.do_update);

    curl_global_cleanup();
    return (ok > 0 || (update_only && ids.empty())) ? 0 : 1;
}
