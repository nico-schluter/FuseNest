// Applying a nesting result to the model.
//
// Two Fusion constraints shape this:
//
//  1. A Move feature takes a Matrix3D and Fusion requires it to be a RIGID
//     transform. A 2D reflection is not one, so a reflected placement is
//     refused rather than mangled. (It would be expressible as a 180-degree
//     rotation about the in-plane mirror axis -- rigid in 3D, flipping the part
//     over -- but that changes which face is up, so it is a deliberate feature
//     rather than a silent fallback. We never request reflection today.)
//
//  2. There is no undo-transaction API. Fusion groups whatever happens inside a
//     command's execute handler into one undo step, so this must be called from
//     there, not directly from the CustomEvent handler that receives the result.
#pragma once

#include "profile.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

struct WriteBackResult {
    int moved = 0;
    int skipped = 0;
    std::vector<std::string> warnings;
    bool grouped = false;
};

/// Moves each profile's source body to the position the solver chose.
/// Reads placements straight from the kernel, so it must run after a solve.
WriteBackResult applyPlacements(const adsk::core::Ptr<adsk::core::Application>& app,
                                const std::vector<Profile>& profiles);

}  // namespace fusenest
