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

struct ExtractResult {
    std::vector<Profile> profiles;
    std::vector<std::string> warnings;
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

}  // namespace fusenest
