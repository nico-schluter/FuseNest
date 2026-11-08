// Fusion is a GUI process: there is no stdout, and an uncaught throw out of an
// add-in entry point is completely silent. So anything worth knowing goes to a
// log file, and anything the user must see goes to a message box.
//
// The journal is ALWAYS on and bounded: it holds one line per meaningful event
// -- what was asked for, what the solver came back with, what was changed in
// the model, and every refusal with its reason -- across the last few sessions.
// It is not truncated at startup, because the log worth reading is usually the
// one from the session before last.
//
// The volume lives behind logDiag(): the solver's own narrative, timings and
// per-tick accounting only reach the file when the user has asked to capture
// diagnostics. Part and component names appear at both levels; geometry never
// appears in the log at all.
#pragma once

#include <string>

namespace fusenest {

/// Rotates the previous session's file out of the way, opens a new one and
/// writes the session header. Call once from run(), after settings are loaded
/// so the header can state whether diagnostics are on.
void logBegin(const std::string& what);

/// Closes the file. Call from stop(): a dylib that is about to be unloaded
/// must not leave a stream pointing into freed static storage.
void logEnd();

/// Appends one line. Always recorded. Safe from any thread, and flushed on
/// every line -- a log that loses its tail to a crash is a log that is missing
/// exactly the part being looked for.
void logLine(const std::string& line);

/// Appends one line only when diagnostics are being captured.
void logDiag(const std::string& line);

/// Turns the diagnostic level on or off for the rest of the session.
void logSetDiagnostics(bool on);
bool logDiagnostics();

/// Absolute path of the current log file, for showing the user where to look.
/// Empty when no writable location was found.
const std::string& logPath();

/// One line naming where to look, for the bottom of a message box. Says so
/// plainly when there is nowhere to write rather than printing an empty path.
std::string logLocation();

/// Directory the log, the settings file and any diagnostic dumps live in.
/// Empty when no writable location was found -- callers then write nothing.
const std::string& outputDir();

}  // namespace fusenest
