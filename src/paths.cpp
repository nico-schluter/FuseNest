#include "paths.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fusenest {

namespace {

/// Where this dylib lives, asked of the loader.
///
/// Fusion's own working directory is somewhere in the installation and has
/// nothing to do with the add-in, so a relative path to a shipped file is not
/// a path to anything. dladdr answers for any address inside the image, and
/// the address of a function in this file is one.
std::string resolveAddinDir() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&resolveAddinDir), &module))
        return {};
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        // Truncated. A path may legally exceed MAX_PATH.
        if (buffer.size() > 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
    std::error_code ec;
    const std::filesystem::path self(buffer);
    return self.parent_path().string();
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&resolveAddinDir), &info) == 0) return {};
    if (info.dli_fname == nullptr || *info.dli_fname == '\0') return {};
    return std::filesystem::path(info.dli_fname).parent_path().string();
#endif
}

}  // namespace
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

const std::string& addinDir() {
    // Cached: the answer cannot change while the image is loaded, and this is
    // asked once per dialog.
    static const std::string dir = resolveAddinDir();
    return dir;
}

std::string addinPath(const std::string& relative) {
    const std::string& dir = addinDir();
    if (dir.empty()) return {};
    return (std::filesystem::path(dir) / relative).string();
}

}  // namespace fusenest
