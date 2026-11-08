#include "paths.h"

#include <cstdlib>
#include <filesystem>

namespace fusenest {
namespace {

const char* kFolderName = "FuseNest";

/// getenv, but an empty variable counts as unset. An empty HOME is the shape
/// a sandboxed or service-launched process arrives in, and treating it as a
/// path produces "/Library/Application Support/FuseNest".
const char* envOrNull(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

/// The candidates in preference order. The first one that can be created wins,
/// so a locked-down %LOCALAPPDATA% falls through to %APPDATA% rather than
/// taking the whole log with it.
std::string resolve() {
    if (const char* override_ = envOrNull("FUSENEST_DATA_DIR")) {
        return ensureDirectory(override_) ? std::string(override_) : std::string();
    }

    std::filesystem::path candidates[3];
    int n = 0;
#ifdef _WIN32
    if (const char* local = envOrNull("LOCALAPPDATA")) candidates[n++] = local;
    if (const char* roaming = envOrNull("APPDATA")) candidates[n++] = roaming;
    if (const char* temp = envOrNull("TEMP")) candidates[n++] = temp;
#else
    if (const char* home = envOrNull("HOME"))
        candidates[n++] = std::filesystem::path(home) / "Library" / "Application Support";
    if (const char* tmp = envOrNull("TMPDIR")) candidates[n++] = tmp;
    candidates[n++] = "/tmp";
#endif

    for (int i = 0; i < n; ++i) {
        const std::string dir = (candidates[i] / kFolderName).string();
        if (ensureDirectory(dir)) return dir;
    }
    return std::string();
}

}  // namespace

bool ensureDirectory(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    // The error_code overloads throughout: the throwing ones would abort the
    // add-in from inside run(), which is exactly the failure this file exists
    // to avoid. create_directories reports false-with-no-error for a directory
    // that already existed, so existence is checked separately rather than
    // inferred from the return value.
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec);
}

const std::string& userDataDir() {
    // Resolved once: a path that changed mid-session would split one run's
    // journal across two files.
    static const std::string dir = resolve();
    return dir;
}

std::string userDataPath(const std::string& name) {
    const std::string& dir = userDataDir();
    if (dir.empty()) return std::string();
    std::error_code ec;
    return (std::filesystem::path(dir) / name).string();
}

}  // namespace fusenest
