#include "log.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef FUSENEST_LOG_DIR
#define FUSENEST_LOG_DIR "."
#endif

namespace fusenest {
namespace {
const char* kPath = FUSENEST_LOG_DIR "/fusenest.log";

/// Milliseconds since the first log line. Relative rather than wall clock
/// because the questions being asked of this log are all about intervals --
/// how long a cancel took to drain, whether a gap was covered by work -- and
/// subtracting two clock times by eye is how those get misread.
long elapsedMs() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
}
}

const char* logPath() { return kPath; }

const char* outputDir() { return FUSENEST_LOG_DIR; }

void logBegin(const std::string& what) {
    std::ofstream f(kPath, std::ios::trunc);
    if (f) f << "=== FuseNest: " << what << " ===\n";
}

void logLine(const std::string& line) {
    std::ofstream f(kPath, std::ios::app);
    if (!f) return;
    std::ostringstream stamp;
    stamp << '[' << std::setw(7) << elapsedMs() << "] ";
    f << stamp.str() << line << '\n';
}

}  // namespace fusenest
