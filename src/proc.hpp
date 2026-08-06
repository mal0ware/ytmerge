// proc.hpp — spawn a process with argv semantics (no shell), capture
// stdout/stderr via temp files (immune to pipe-buffer deadlocks), enforce a
// wall-clock timeout. Win32 + POSIX.

#pragma once

#if defined(_WIN32) && !defined(NOMINMAX)
  #define NOMINMAX
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace proc {

struct Result {
    int         exit_code = -1;
    bool        timed_out = false;
    std::string out, err;
};

namespace detail {

inline std::string slurp_and_remove(const std::filesystem::path& p) {
    std::string s;
    {
        std::ifstream f(p, std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            s = ss.str();
        }
    }
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return s;
}

inline std::filesystem::path temp_base() {
    static std::atomic<unsigned> seq{0};
#if defined(_WIN32)
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    return dir / ("ytmerge_p" + std::to_string(pid) + "_" + std::to_string(seq++));
}

#if defined(_WIN32)
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Windows command-line quoting per the MSVCRT parsing rules.
inline std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') {
            out.append(bs * 2 + 1, L'\\');
            out += L'"';
            bs = 0;
        } else {
            out.append(bs, L'\\');
            out += c;
            bs = 0;
        }
    }
    out.append(bs * 2, L'\\');
    out += L'"';
    return out;
}
#endif

}  // namespace detail

inline Result run(const std::vector<std::string>& argv, int timeout_s = 1800) {
    Result r;
    if (argv.empty()) return r;
    namespace fs = std::filesystem;
    const fs::path base  = detail::temp_base();
    const fs::path out_p = base.string() + ".out";
    const fs::path err_p = base.string() + ".err";

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hOut = CreateFileW(out_p.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = CreateFileW(err_p.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        return r;
    }
    std::wstring cmdline;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += L' ';
        cmdline += detail::quote_arg(detail::widen(argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError  = hErr;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may modify the buffer; pass a mutable copy.
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);
    if (!ok) {
        r.exit_code = 127;
        r.err = "failed to launch: " + argv[0];
        std::error_code ec;
        fs::remove(out_p, ec);
        fs::remove(err_p, ec);
        return r;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess,
                                     static_cast<DWORD>(timeout_s) * 1000u);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        r.timed_out = true;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    int out_fd = ::open(out_p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int err_fd = ::open(err_p.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (out_fd < 0 || err_fd < 0) {
        if (out_fd >= 0) ::close(out_fd);
        if (err_fd >= 0) ::close(err_fd);
        return r;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(out_fd, 1);
        ::dup2(err_fd, 2);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }
    ::close(out_fd);
    ::close(err_fd);
    if (pid < 0) return r;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeout_s);
    while (true) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            r.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    r.out = detail::slurp_and_remove(out_p);
    r.err = detail::slurp_and_remove(err_p);
    return r;
}

}  // namespace proc
