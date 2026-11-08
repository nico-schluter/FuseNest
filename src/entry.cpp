// FuseNest -- 2D nesting for Fusion, built on sparrow.
//
// This file is only the exported surface of the dylib -- run() and stop(), and
// nothing else. Everything real lives alongside it in src/.
//
// Error handling note: the Fusion C++ API reports failure through return values
// (null objects, false) plus app->getLastError(), NOT exceptions -- a try/catch
// will not see an API error. The try/catch here exists only to stop our own C++
// from throwing out of run(), which would kill the add-in with no message at all.

#include "log.h"
#include "nest_command.h"
#include "settings.h"

#include <Core/CoreAll.h>
#include <Fusion/FusionAll.h>

using namespace adsk::core;
using namespace adsk::fusion;

namespace {
Ptr<Application> app;
Ptr<UserInterface> ui;
}  // namespace

extern "C" XI_EXPORT bool run(const char* context) {
    app = Application::get();
    if (!app) return false;
    ui = app->userInterface();
    if (!ui) return false;

    try {
        // Settings first, and inside the try: the file decides whether this
        // session captures diagnostics, and the log header should say so. It
        // cannot refuse to load -- a missing, truncated or hand-mangled file
        // comes back as defaults with a note (see settings.h) -- but a debug
        // aid gets no chance to take the add-in down with it either way.
        const fusenest::SettingsLoad loaded = fusenest::loadSettings();
        fusenest::applySettings(loaded.values);
        fusenest::logSetDiagnostics(loaded.values.captureDiagnostics || loaded.values.traceLog);

        fusenest::logBegin("run");
        if (!loaded.fileFound) {
            fusenest::logLine("no settings file yet; using defaults");
        } else if (loaded.recovered) {
            fusenest::logLine("settings partly unusable; defaults stood in:");
            for (const std::string& note : loaded.notes) fusenest::logLine("  " + note);
        } else {
            fusenest::logLine("settings loaded");
        }
        fusenest::logLine(std::string("context: ") + (context ? context : "<null>"));

        if (!fusenest::registerNestCommand(app)) {
            ui->messageBox("FuseNest failed to register its command.\n\n" +
                               fusenest::logLocation(),
                           "FuseNest");
            return false;
        }
    } catch (const std::exception& e) {
        if (ui) ui->messageBox(std::string("FuseNest threw: ") + e.what(), "FuseNest");
        return false;
    } catch (...) {
        if (ui) ui->messageBox("FuseNest threw an unknown exception", "FuseNest");
        return false;
    }

    return true;
}

extern "C" XI_EXPORT bool stop(const char* context) {
    try {
        // Must happen before Fusion unloads us: UI objects outlive the add-in
        // and a leftover command definition breaks the next Run.
        fusenest::unregisterNestCommand(app);
    } catch (...) {
    }

    // After the unregister, so anything that happened on the way out is in the
    // file: the stream lives in static storage this dylib is about to lose.
    fusenest::logEnd();

    ui = nullptr;
    app = nullptr;
    return true;
}
