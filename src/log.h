// Fusion is a GUI process: there is no stdout, and an uncaught throw out of an
// add-in entry point is completely silent. So anything worth knowing goes to a
// log file, and anything the user must see goes to a message box.
#pragma once

#include <string>

namespace fusenest {

/// Truncates the log and writes a header. Call once from run().
void logBegin(const std::string& what);

/// Appends one line.
void logLine(const std::string& line);

/// Absolute path of the log file, for showing the user where to look.
const char* logPath();

/// Directory the log and any other dev artefacts are written to.
const char* outputDir();

}  // namespace fusenest
