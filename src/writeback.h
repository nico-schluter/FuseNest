// Applying a nesting result to the model.
//
// Three Fusion constraints shape this:
//
//  1. A Move feature takes a Matrix3D and Fusion requires it to be a RIGID
//     transform. A 2D reflection is not one, so a reflected placement is
//     refused rather than mangled. (It would be expressible as a 180-degree
//     rotation about the in-plane mirror axis -- rigid in 3D, flipping the part
//     over -- but that changes which face is up, so it is a deliberate feature
//     rather than a silent fallback. We never request reflection today.)
//
//  2. A move feature is a timeline entry, so a DIRECT design has nowhere to put
//     one: `MoveFeatures::add` applies the move and returns nothing. Success is
//     therefore read differently in the two design types -- see the add site --
//     and an instance's new transform needs no snapshot there, because in a
//     direct design the transform is the model rather than a pending change.
//
//  3. There is no undo-transaction API. Fusion groups whatever happens inside a
//     command's execute handler into one undo step, so this must be called from
//     there, not directly from the CustomEvent handler that receives the result.
//     This matters more now than it did: applying a layout deletes joints, and
//     one undo step is what makes that recoverable.
//
// A placement reaches the model one of two ways, decided by the profile (see
// WHAT MOVES in profile.h): a body moves by a move feature, an instance by its
// own occurrence transform. The two are not interchangeable -- a move feature
// takes no occurrence, and an occurrence transform cannot move a body -- and
// the instance path additionally breaks whatever constrained the instance's
// position and commits the result as a snapshot.
#pragma once

#include "nest_kernel.h"
#include "profile.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

/// Rotation about Z through the origin, then a translation in XY.
///
/// Valid precisely because the outline was projected onto XY along Z: the
/// body's XY silhouette transforms exactly as the polygon did, and Z is left
/// alone. Shared with the preview so a drawn layout and an applied one cannot
/// disagree about what a placement means.
adsk::core::Ptr<adsk::core::Matrix3D> placementMatrix(const NkPlacement& p);

struct WriteBackResult {
    int moved = 0;
    int skipped = 0;
    /// Grounds, joints and rigid groups removed so instances could be placed.
    /// Reported rather than counted internally: deleting a user's joints is a
    /// real change to their design, and it has to be said out loud even when
    /// everything else went perfectly.
    int constraintsBroken = 0;
    std::vector<std::string> warnings;
    bool grouped = false;
};

/// Moves each profile's body or instance to the position the solver chose.
///
/// Placements are passed in rather than read from the kernel, because a live
/// preview and a final apply must be able to use the SAME layout: the kernel's
/// published snapshot changes several times a second while a solve runs, so a
/// function that fetched its own would draw something subtly different each
/// time it was asked to draw the same thing.
///
/// `groupTimeline` collapses the moves into one timeline entry. Worth skipping
/// for a preview that is about to be rolled back.
WriteBackResult applyPlacements(const adsk::core::Ptr<adsk::core::Application>& app,
                                const std::vector<Profile>& profiles,
                                const std::vector<NkPlacement>& placements,
                                bool groupTimeline = true);

}  // namespace fusenest
