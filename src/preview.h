// Showing a proposed layout without waiting for the model to catch up.
//
// Profiling settled why this module exists. Moving bodies with move features
// costs 8ms PER BODY, essentially all of it inside MoveFeatures::add -- the
// timeline insert. Every other step of the write-back is free. At a hundred
// bodies that is ~800ms of frozen main thread per redraw, and it is
// irreducible: BRepBody has no settable transform, so a move feature is the
// only way to move a body in a parametric design, and defineAsFreeMove takes
// one matrix per collection so a hundred different transforms cannot share one
// feature.
//
// Custom graphics sidestep the model entirely. Geometry is built once and each
// redraw only assigns transforms, which is the whole point: the per-redraw cost
// stops scaling with the modelling engine and starts scaling with a matrix
// multiply.
#pragma once

#include "nest_kernel.h"
#include "profile.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

enum class PreviewMode {
    /// The model itself. Exact, and Fusion's rollback plus isValidResult make
    /// Apply free -- but one timeline insert per body, measured at 12ms each,
    /// so 104 parts froze the main thread for 17.4 seconds across one solve.
    /// Kept as a reference and an escape hatch, not as a default.
    MoveFeatures,
    /// TemporaryBRepManager copies of the real bodies, drawn as custom
    /// graphics. The default, and measured at 3.21ms per redraw for 104 parts
    /// against 1241ms for move features -- about 390x. Indistinguishable from
    /// Outlines per redraw, because the cost is the transform assignment rather
    /// than the geometry behind it, so there is no reason to draw anything less
    /// faithful than a copy of the real body.
    TempBodies,
    /// The extracted silhouettes drawn as closed line strips. 4ms to build
    /// against 24ms, which buys nothing worth having, but it shows what the
    /// solver actually reasoned about rather than what the body looks like --
    /// which is the better view when a silhouette looks wrong.
    Outlines,
};

const char* previewModeLabel(PreviewMode mode);

/// Builds the geometry a graphics mode needs. Once per job. No-op for
/// MoveFeatures, which has nothing to build.
bool previewBuild(const adsk::core::Ptr<adsk::core::Application>& app,
                  const std::vector<Profile>& profiles, PreviewMode mode, std::string& error);

/// Positions the built geometry. For graphics modes this is one transform
/// assignment per part. Returns how many parts were placed.
int previewShow(const std::vector<NkPlacement>& placements);

/// Hides the real bodies so the graphics are not drawn over unmoved originals.
///
/// Safe to leave hidden on cancel: visibility is a document change, so it goes
/// back with the preview transaction if the result is not accepted.
void previewHideSources(const std::vector<Profile>& profiles, bool hidden);

/// Drops the graphics. Must run before the dialog closes -- custom graphics are
/// NOT part of the command transaction and would survive a cancel.
void previewClear();

/// Times all three mechanisms on one layout and writes the result to the log.
/// Builds and tears down its own geometry, so it leaves nothing behind.
void previewBenchmark(const adsk::core::Ptr<adsk::core::Application>& app,
                      const std::vector<Profile>& profiles,
                      const std::vector<NkPlacement>& placements);

}  // namespace fusenest
