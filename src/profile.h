// Turning Fusion geometry into flat 2D profiles.
//
// The hard constraint comes from the far end: a sparrow/jagua-rs *item* must be
// a single simple polygon. `import_item` rejects holes outright
// ("item N has unsupported holes") and multipolygons ("No support for
// multipolygons yet"). Containers may have holes -- items may not.
//
// So holes are captured here but dropped by the adapter that builds solver
// input. They are still worth having: honest area reporting, preview drawing,
// and eventually container keep-out zones.
//
// The second constraint comes from the near end: write-back moves BODIES, so a
// body must yield exactly one profile. Emitting several and letting them nest
// apart produces one move feature per placement on the same body, and those
// compose -- two opposed placements cancel to almost no movement at all. A body
// that cannot be reduced to one outline is therefore refused, not split.
#pragma once

#include "point2.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

/// One nestable outline. `outer` is closed implicitly -- the first point is not
/// repeated at the end, which is what jagua-rs expects (it strips a repeated
/// closing vertex anyway).
struct Profile {
    std::string name;
    /// The BRepBody this outline came from, held as Base to keep the Fusion
    /// modelling headers out of this one. Cast at the point of use.
    adsk::core::Ptr<adsk::core::Base> source;
    std::vector<Point2> outer;
    std::vector<std::vector<Point2>> holes;
    double areaMm2 = 0.0;          ///< true material area, holes excluded
    bool silhouetteApproximated = false;
};

struct ExtractOptions {
    /// Chord tolerance for turning curves into line segments. Lines and arcs
    /// come out exact or near-exact; splines -- lettering -- are approximated
    /// twice over, once in the silhouette and once here.
    double toleranceMm = 0.05;
};

/// One reason a selected body cannot be nested at all.
///
/// Name and reason are kept apart rather than pre-joined because the two
/// consumers phrase them differently: the setup dialog leads with the names so
/// the user can see at a glance which selection to undo, while the log and the
/// refusal want whole sentences. `reason` is a fragment that reads correctly
/// after the name and a colon, with no leading capital and no trailing period.
struct BodyIssue {
    std::string name;
    std::string reason;
};

struct ExtractResult {
    std::vector<Profile> profiles;
    std::vector<std::string> warnings;
    /// Why the job must not run, one entry per offending body.
    /// Kept apart from `warnings` because these decide control flow rather than
    /// describe it: nesting the remaining bodies would leave a body the user
    /// explicitly selected out of the layout without saying so.
    std::vector<BodyIssue> blockers;
    int bodiesExamined = 0;
    int holesDropped = 0;          ///< holes found; none of them reach the solver
};

/// Projects each selected body onto the root XY plane and reads its outline.
///
/// Accepts BRepBody and Occurrence selections; occurrences are expanded to
/// their bodies, recursively through child occurrences.
ExtractResult extractProfiles(const adsk::core::Ptr<adsk::core::Application>& app,
                              const std::vector<adsk::core::Ptr<adsk::core::Base>>& selections,
                              const ExtractOptions& options);

/// Applies the same nestability rule as `extractProfiles` without reading any
/// outlines, so the setup dialog can say no before the user commits.
///
/// It still has to project each body -- a pinched silhouette is only visible in
/// the projection, and the body's own lump count cannot reveal it -- but it
/// stops at the face count instead of walking loops, tessellating curves and
/// building rings. Both paths decide through the same predicate, so a body the
/// dialog accepts cannot be refused by extraction afterwards.
std::vector<BodyIssue> validateBodies(
    const adsk::core::Ptr<adsk::core::Application>& app,
    const std::vector<adsk::core::Ptr<adsk::core::Base>>& selections,
    const ExtractOptions& options = {});

}  // namespace fusenest
