// Where FuseNest keeps the things it writes: the journal, the settings file,
// and anything a diagnostic run dumps.
//
// This used to be a compile-time define pointing at the source tree, which
// meant that on any machine other than the one that built the add-in every
// write failed and the log silently went nowhere -- the worst possible failure
// for the one file support depends on.
//
// Nothing in here throws or reports an error to the user. A machine with no
// writable directory at all is a machine FuseNest still has to start on, so
// the directory comes back empty and every writer treats that as "write
// nothing" rather than as a reason to refuse.
#pragma once

#include <string>

namespace fusenest {

/// The per-user directory FuseNest writes into, created on first call.
///
/// Empty when no writable location could be found. Resolved once and cached,
/// so the answer cannot change under a running session.
///
///   macOS    ~/Library/Application Support/FuseNest
///   Windows  %LOCALAPPDATA%\FuseNest  (then %APPDATA%)
///
/// FUSENEST_DATA_DIR overrides it, which is how the dev loop and the tests
/// keep out of the real one.
const std::string& userDataDir();

/// Joins a file name onto userDataDir(). Empty when the directory is.
std::string userDataPath(const std::string& name);

/// Creates `dir` and its parents. False on any failure, including a path that
/// exists as something other than a directory. Never throws.
bool ensureDirectory(const std::string& dir);

/// The directory this dylib was loaded from -- the add-in folder, where the
/// manifest, the icon and resources/ sit. Resolved from the loaded image
/// rather than from the working directory, which belongs to Fusion and is not
/// ours to reason about. Empty if the platform will not say.
const std::string& addinDir();

/// A path inside the add-in folder, for a file shipped beside the binary.
/// Empty when addinDir() is, so a caller can pass the result straight on to
/// an API that treats empty as "not set".
std::string addinPath(const std::string& relative);

}  // namespace fusenest
