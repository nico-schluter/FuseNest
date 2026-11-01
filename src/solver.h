// Running the nesting kernel without blocking Fusion.
//
// The architecture is the one validated in the C++ API investigation: the
// solve runs on a worker thread so the main thread is never blocked and no
// doEvents() pumping is needed, and the result is marshalled back with a
// Fusion CustomEvent -- the only legal way for a worker to reach the API.
// Progress is read from atomics inside the kernel, never pushed by a callback.
#pragma once

#include "nest_kernel.h"

#include <Core/CoreAll.h>

#include <functional>
#include <string>

namespace fusenest {

/// Registers the completion CustomEvent. `onFinished` is invoked ON THE MAIN
/// THREAD once a solve ends, however it ended.
///
/// Note: a modal dialog blocks delivery -- fireCustomEvent queues, and Fusion
/// only drains the queue when the main thread returns to its event loop.
bool solverInit(const adsk::core::Ptr<adsk::core::Application>& app,
                std::function<void()> onFinished);

/// Cancels, joins and unregisters. Must run before Fusion unloads the add-in:
/// a thread outliving the image executes freed code.
void solverShutdown(const adsk::core::Ptr<adsk::core::Application>& app);

/// Starts a solve. Returns false if one is already running or the thread could
/// not start.
bool solverStart(const std::string& instanceJson, int budgetSeconds, int workers,
                 std::string& error);

void solverCancel();
bool solverBusy();
bool solverProgress(NkProgress& out);

int solverReturnCode();
std::string solverResultJson();
std::string solverLastError();
double solverElapsedMs();

}  // namespace fusenest
