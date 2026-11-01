// The FuseNest command: a button in the Add-Ins panel, a dialog that gathers a
// nesting job, and an execute handler that turns it into solver input.
#pragma once

#include <Core/CoreAll.h>

namespace fusenest {

/// Creates the command definition and puts it in the Add-Ins panel.
/// Returns false and logs on failure.
bool registerNestCommand(const adsk::core::Ptr<adsk::core::Application>& app);

/// Removes the control and the definition. Not optional: a definition that
/// outlives the add-in makes the next Run fail with "id already exists".
void unregisterNestCommand(const adsk::core::Ptr<adsk::core::Application>& app);

}  // namespace fusenest
