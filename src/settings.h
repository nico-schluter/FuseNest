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
    /// Position in the dialog's dropdown, NOT a RotationMode: 0 free,
    /// 1 ninety-degree steps, 2 one-eighty, 3 fixed. The order was reversed
    /// and the default moved from half to free without renaming this key,
    /// which is only safe because no shipped build ever wrote the file -- the
    /// settings file postdates 1.0.0. A later reorder needs a new key.
    int rotationIndex = 0;
    /// Wall-clock ceiling on a solve, in seconds. NOT a budget and not in the
    /// dialog: both solver phases stop on convergence -- exploration on three
    /// consecutive failed separations, compression on its step decaying below
    /// the floor -- so a normal run ends long before this. Measured on the
    /// 29-part instance with the clock set to an hour: 22s, ended by
    /// exploration's convergence stop. This is the backstop for the case that
    /// convergence is bounded in cycles rather than time, which no instance
    /// here has yet produced.
    ///
    /// Here rather than in the dialog for the reason traceLog is: a value no
    /// normal run reaches is not a decision to put in front of anyone, but it
    /// is still worth being able to pin for a reproducible run or a bug report.
    ///
    /// The key is deliberately NOT the "timeBudgetSeconds" that used to back
    /// the dialog's spinner. That key held values as low as 1, chosen when it
    /// was a budget that shaped the answer; read as a ceiling it would cut a
    /// solve off mid-exploration with no dialog left to fix it. An old file's
    /// key is simply unknown now, and unknown keys are ignored -- so every
    /// stale value is dropped without a line of migration code.
    int budgetCeilingSeconds = 600;
    /// Solver workers, or 0 to derive them from the machine. Not in the dialog,
    /// for the reason budgetCeilingSeconds is not: the derived value is right on
    /// every machine this has been run on, and a spinner would only invite
    /// someone to make it worse. See cpu.h for the policy.
    ///
    /// Overriding it is still worth being able to do, and for a better reason
    /// than the ceiling: worker count changes the LAYOUT, not just the time, so
    /// one worker is the only way to reproduce a nest exactly -- which is what
    /// the kernel's own tests pin.
    ///
    /// Renamed from "workers" rather than migrated, exactly as the ceiling was.
    /// An old file's value was written by a spinner whose default happened to
    /// match this machine's core count; carried onto a 24-core box it would pin
    /// four workers forever and auto would never run.
    int workerOverride = 0;
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
