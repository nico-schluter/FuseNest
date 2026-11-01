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
        fusenest::logBegin("run");
        fusenest::logLine(std::string("context: ") + (context ? context : "<null>"));

        if (!fusenest::registerNestCommand(app)) {
            ui->messageBox(std::string("FuseNest failed to register its command.\n\nLog: ") +
                               fusenest::logPath(),
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

    ui = nullptr;
    app = nullptr;
    return true;
}
