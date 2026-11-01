#include "log.h"

#include <fstream>

#ifndef FUSENEST_LOG_DIR
#define FUSENEST_LOG_DIR "."
#endif

namespace fusenest {
namespace {
const char* kPath = FUSENEST_LOG_DIR "/fusenest.log";
}

const char* logPath() { return kPath; }

const char* outputDir() { return FUSENEST_LOG_DIR; }

void logBegin(const std::string& what) {
    std::ofstream f(kPath, std::ios::trunc);
    if (f) f << "=== FuseNest: " << what << " ===\n";
}

void logLine(const std::string& line) {
    std::ofstream f(kPath, std::ios::app);
    if (f) f << line << '\n';
}

}  // namespace fusenest
