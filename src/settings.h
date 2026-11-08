// settings.json: what the user chose last time, and whether to record
// diagnostics.
//
// Two reasons this is a file rather than a static. The dialog's values used to
// live in process memory, so they reset every time Fusion restarted. And the
// diagnostics choice has to be readable BEFORE any dialog exists: registration
// failures, solverInit failures and the ABI check all happen inside run(),
// where a checkbox the user has not seen yet cannot help.
//
// RECOVERY IS THE WHOLE CONTRACT. A settings file is a debug aid, and a debug
// aid that can stop the add-in from loading is worse than no settings file at
// all. So: nothing here throws, every field falls back to its default on its
// own, an unreadable or nonsense file is equivalent to no file, unknown keys
// are ignored (an older build must tolerate a newer one's file), and every
// number is clamped to a range the rest of the add-in can accept. A file that
// had to be recovered from is moved aside as settings.json.bad rather than
// silently overwritten, so a hand-edit mistake is still there to look at.
#pragma once

#include <string>
#include <vector>

namespace fusenest {

/// Everything FuseNest remembers between sessions.
///
/// The defaults are the shipped ones: these exact values are what a first run,
/// a missing file and an unreadable file all produce.
struct Settings {
    double sheetHeightMm = 1000.0;
    double sheetWidthMm = 2000.0;
    double partSpacingMm = 2.0;
    int rotationIndex = 1;  // 0 fixed, 1 half, 2 quarter, 3 continuous
    int timeBudgetSeconds = 10;
    int workers = 4;
    /// Adds the solver narrative, the timing report and the geometry dumps.
    bool captureDiagnostics = false;
    /// The development escape hatch ("logLevel": "trace"): everything
    /// captureDiagnostics gives, plus the kernel's per-iteration chatter.
    /// Deliberately not in the dialog.
    bool traceLog = false;
};

/// What a load produced, and what was wrong with the file if anything was.
struct SettingsLoad {
    Settings values;
    /// False when there was no file to read -- a first run, not a problem.
    bool fileFound = false;
    /// True when something in the file could not be used and a default or a
    /// clamp stood in for it. Worth a line in the journal; never worth a
    /// message box.
    bool recovered = false;
    /// One short note per thing that was wrong, for the journal.
    std::vector<std::string> notes;
};

/// Reads settings from `text`. Pure, total, and the only part worth testing:
/// every malformed input has to land on defaults rather than anywhere else.
SettingsLoad parseSettings(const std::string& text);

/// Serialises to the on-disk form. Locale-independent.
std::string formatSettings(const Settings& s);

/// Loads from the user data directory. A missing directory, a missing file, an
/// unreadable file and a corrupt file all come back as defaults.
SettingsLoad loadSettings();

/// Writes atomically (temp file, then rename), so a crash mid-write cannot
/// leave a truncated file behind. False if it could not be written, which is
/// not an error anyone needs to see.
bool saveSettings(const Settings& s);

/// The values this session is running with. Defaults until loadSettings has
/// been applied, so a caller that runs before run() still gets sane values.
const Settings& settings();

/// Replaces the session's values. Does not write anything.
void applySettings(const Settings& s);

}  // namespace fusenest
