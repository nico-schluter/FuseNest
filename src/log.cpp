#include "log.h"

#include "paths.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#ifndef FUSENEST_VERSION
#define FUSENEST_VERSION "0.0.0-dev"
#endif

namespace fusenest {
namespace {

const char* kLogName = "fusenest.log";

/// Sessions kept: the current one plus four. The question a log answers is
/// almost never about the run that is open -- that one is on screen -- so a
/// handful of previous sessions is the whole point, and five small text files
/// cost nothing.
constexpr int kKeptSessions = 5;

std::mutex g_mutex;
std::ofstream g_file;
std::string g_path;
std::atomic<bool> g_diagnostics{false};

/// The baseline for the relative timestamps, reset by logBegin.
///
/// Deliberately not a function-local static: linking Rust pulls in TLS, dyld
/// then refuses to unload the add-in, and statics survive Stop followed by Run.
/// A once-initialised baseline would date the second session's lines from the
/// first session's start.
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

/// Milliseconds since the session began. Relative rather than wall clock
/// because the questions being asked of this log are all about intervals --
/// how long a cancel took to drain, whether a gap was covered by work -- and
/// subtracting two clock times by eye is how those get misread. The header
/// carries one absolute timestamp to anchor them.
long elapsedMs() {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - g_start)
                                 .count());
}

std::string rotatedName(int n) {
    return n == 0 ? std::string(kLogName) : "fusenest." + std::to_string(n) + ".log";
}

/// Shifts fusenest.log -> fusenest.1.log -> ... and drops the oldest.
///
/// Errors are ignored by design: a rotation that cannot happen must not stop
/// the session from logging. The worst case is one long file, which is still
/// better than no file.
void rotate() {
    const std::string& dir = outputDir();
    if (dir.empty()) return;
    const std::filesystem::path base(dir);
    std::error_code ec;

    std::filesystem::remove(base / rotatedName(kKeptSessions - 1), ec);
    for (int n = kKeptSessions - 2; n >= 0; --n) {
        const std::filesystem::path from = base / rotatedName(n);
        if (!std::filesystem::exists(from, ec)) continue;
        std::filesystem::rename(from, base / rotatedName(n + 1), ec);
    }
}

std::string wallClockNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

const char* platformName() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

const char* architectureName() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

}  // namespace

const std::string& outputDir() { return userDataDir(); }

const std::string& logPath() { return g_path; }

std::string logLocation() {
    if (g_path.empty())
        return "No log could be written: FuseNest found nowhere writable to put one.";
    return "Log: " + g_path;
}

void logSetDiagnostics(bool on) { g_diagnostics = on; }

bool logDiagnostics() { return g_diagnostics; }

void logBegin(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_file.is_open()) g_file.close();
    g_start = std::chrono::steady_clock::now();
    g_path = userDataPath(kLogName);
    if (g_path.empty()) return;  // nowhere writable; every logLine is a no-op

    rotate();
    g_file.open(g_path, std::ios::trunc);
    if (!g_file) {
        g_path.clear();
        return;
    }

    // The header is what makes a log someone emails you usable on its own:
    // which build, which machine, when, and whether the quiet level was on.
    g_file << "=== FuseNest " << FUSENEST_VERSION << ": " << what << " ===\n"
           << "started:     " << wallClockNow() << "\n"
           << "platform:    " << platformName() << " " << architectureName() << "\n"
           << "data dir:    " << outputDir() << "\n"
           << "diagnostics: " << (g_diagnostics ? "capturing" : "off") << "\n"
           << "--\n";
    g_file.flush();
}

void logEnd() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
}

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file.is_open()) return;
    // Held open rather than reopened per line: with the solver narrative on,
    // the old open-append-close was thousands of opens per solve. Flushed per
    // line, so a crash still leaves the lines that led up to it.
    g_file << '[' << std::setw(7) << elapsedMs() << "] " << line << '\n';
    g_file.flush();
}

void logDiag(const std::string& line) {
    if (!g_diagnostics) return;
    logLine(line);
}

}  // namespace fusenest
