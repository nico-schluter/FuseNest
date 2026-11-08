// Showing a proposed layout without waiting for the model to catch up.
//
// The preview is custom graphics: a filled triangle mesh per part with its
// outline drawn on top, positioned by assigning one transform per part per
// redraw. Geometry is built once per pass and triangulation is cached across
// passes, so a redraw costs a matrix multiply and not a model edit.
//
// WHY NOT THE MODEL ITSELF. Moving a body with a move feature costs ~8ms,
// essentially all of it the timeline insert inside MoveFeatures::add. It is
// irreducible: BRepBody has no settable transform, so a move feature is the
// only way to move a body in a parametric design, and defineAsFreeMove takes
// one matrix per collection, so a hundred different transforms cannot share
// one feature. Measured at 12ms each, 104 parts froze the main thread for
// 17.4 seconds in a single solve. Base features fare no better: Fusion rolls
// a preview back before regenerating, so the feature is discarded and has to
// be rebuilt every redraw, which puts a model edit back on the hot path.
//
// WHY NOT SOLID GEOMETRY IN THE GRAPHICS. Drawing TemporaryBRepManager copies
// via addBRepBody crashed Fusion. Four of five dumps shared one stack: undo
// replaying its transaction stream, rebuilding an ASM::safe_entity_ptr, and
// faulting in share_entity_status(ENTITY*) on a dead pointer. addBRepBody
// copies its input, so the group held copied ASM bodies, and the undo stream
// was left referencing entities destroyed behind its back. Meshes and line
// strips are plain doubles and ints -- no ASM entity can reach the data model
// at all -- which is why this approach is the answer to that crash and not
// merely a cheaper option. It costs nothing to prefer: 3.19ms per redraw
// against the solid copies' 3.21ms, because the cost is the transform
// assignment and not the geometry behind it.
//
// THE TRANSACTION RULE, which is general and was learned twice. A custom
// graphics group hangs off the Component, so creating or deleting one is a
// document write -- as are body visibility changes and anything touching a
// base feature. Every document write must happen inside executePreview or
// execute, where Fusion owns a transaction. Writes from commandCreated or
// destroy own no transaction: they land in the undo stream as entries of
// their own, which showed up first as intermittent crashes when hiding was
// done there, and later as a spare undo step after Apply when the graphics
// group was still being built there.
#pragma once

#include "nest_kernel.h"
#include "profile.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

/// The sheet boundaries to draw behind the parts.
///
/// `pitch` comes from the kernel's snapshot rather than being derived from
/// `widthMm` here: a request may leave the inter-sheet gap at zero, which asks
/// the kernel for its own default, so the kernel is the only thing that knows
/// where the boundaries are. Drawing them anywhere else would show a part
/// sitting outside a sheet it is in fact inside, or the reverse.
///
/// A `count` of zero draws nothing, which is the unbounded-strip case.
struct PreviewSheets {
    double widthMm = 0.0;
    double heightMm = 0.0;
    /// Sheet-to-sheet distance. The kernel resolves the gap, so this comes from
    /// the snapshot at draw time rather than from the request.
    double pitchMm = 0.0;
    /// How many sheets to draw: the ones the layout is ON. Also from the
    /// snapshot, for the same reason -- the kernel allocates the sheets.
    int count = 0;
};

/// Builds the geometry for one preview pass.
///
/// Must be called from inside executePreview -- see the transaction rule
/// above. Fusion rolls the group back with the pass that created it, so this
/// runs once per redraw; triangulation is cached internally and is NOT redone
/// unless the profiles themselves change.
bool previewBuild(const adsk::core::Ptr<adsk::core::Application>& app,
                  const std::vector<Profile>& profiles, const PreviewSheets& sheets,
                  std::string& error);

/// Whether geometry is currently live in the document.
///
/// Read from the component rather than from our own pointer: when Fusion rolls
/// a pass back it takes the group with it, and CustomGraphicsGroup has no
/// isValid with which to test the dangling Ptr that leaves behind.
bool previewHasGeometry(const adsk::core::Ptr<adsk::core::Application>& app);

/// Positions the built geometry: one transform assignment per part. Returns
/// how many parts were placed.
int previewShow(const std::vector<NkPlacement>& placements);

/// Hides the real bodies so the graphics are not drawn over unmoved originals,
/// and puts them back on the way out.
///
/// Always the body, never the instance, even for a profile that moves as a
/// component: an instance is only nestable with exactly one body in it, so
/// hiding that body hides all of it, and the body proxy's light bulb is
/// per-occurrence -- hiding it leaves the other instances of the same component
/// visible, which is what someone nesting one of them expects to see.
///
/// Re-done every pass, because visibility is a document change and Fusion's
/// rollback makes the bodies visible again before each regeneration. Returns
/// how many bodies actually changed.
int previewHideSources(const std::vector<Profile>& profiles, bool hidden);

/// Drops the graphics. A document write, so it may only be called from inside
/// executePreview or execute.
void previewClear();

}  // namespace fusenest
