// Running the nesting kernel without blocking Fusion.
//
// The architecture is the one validated in the C++ API investigation: the
// solve runs on a worker thread so the main thread is never blocked and no
// doEvents() pumping is needed, and the result is marshalled back with a
// Fusion CustomEvent -- the only legal way for a worker to reach the API.
// Progress is read from atomics inside the kernel, never pushed by a callback.
//
// Two events come back, both on the main thread:
//   onProgress  repeatedly while a solve runs, driven by a ticker thread
//   onFinished  once, however the solve ended
#pragma once

#include "nest_kernel.h"

#include <Core/CoreAll.h>

#include <functional>
#include <string>
#include <vector>

namespace fusenest {

/// Registers the progress and completion CustomEvents. Both callbacks are
/// invoked ON THE MAIN THREAD.
///
/// Note: a modal dialog blocks delivery -- fireCustomEvent queues, and Fusion
/// only drains the queue when the main thread returns to its event loop. A
/// command dialog is not modal in that sense, which is what makes a live
/// progress readout possible at all.
bool solverInit(const adsk::core::Ptr<adsk::core::Application>& app,
                std::function<void()> onProgress, std::function<void()> onFinished);

/// Cancels, joins and unregisters. Must run before Fusion unloads the add-in:
/// a thread outliving the image executes freed code.
void solverShutdown(const adsk::core::Ptr<adsk::core::Application>& app);

/// Starts a solve. Returns false if one is already running or the thread could
/// not start.
///
/// `sheets` may be null, which asks for the infinite strip; see NkSheets. It is
/// copied, so the caller need not keep it alive.
bool solverStart(const std::string& instanceJson, int budgetSeconds, int workers,
                 const NkSheets* sheets, std::string& error);

/// Moves everything the kernel has logged since the last call into the add-in's
/// log file. Cheap when there is nothing pending, so it is fine to call on every
/// progress tick.
void solverDrainKernelLog();

/// How much the kernel logs. See nk_log_set_level for the scale.
void solverSetLogLevel(int level);

void solverCancel();
bool solverBusy();
bool solverProgress(NkProgress& out);

/// The kernel's current best feasible layout. `out` is resized to fit.
/// Returns false when nothing has been published yet, which is the case for the
/// first moments of a solve and for one that failed outright.
bool solverSnapshot(std::vector<NkPlacement>& out, NkSnapshotInfo& info);

/// Just the generation, for deciding whether a redraw is needed at all.
std::uint64_t solverSnapshotGeneration();

int solverReturnCode();
std::string solverResultJson();
std::string solverLastError();
double solverElapsedMs();

}  // namespace fusenest
