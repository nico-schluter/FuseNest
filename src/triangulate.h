// Turning an outline with holes into triangles, so the preview can be filled.
//
// No Fusion headers: this is pure geometry, which is the point. Every other
// part of the preview can only be exercised by restarting Fusion and watching
// it, and that feedback loop is what made the preview work expensive. This
// part is testable from a terminal.
#pragma once

#include "point2.h"

#include <vector>

namespace fusenest {

/// The triangulation of one profile.
struct Triangulation {
    /// Every ring's points, outer first, then each hole. Bridging duplicates
    /// vertices, so this is NOT the input point list and indices below refer
    /// to this array only.
    std::vector<Point2> vertices;
    /// Three indices per triangle, counter-clockwise.
    std::vector<int> indices;
};

/// Ear-clips `outer` with `holes` cut out of it.
///
/// Holes are bridged into the outer ring first -- a hole is joined to the
/// outline by a pair of coincident edges, which turns the whole thing into one
/// simple polygon that ear clipping can handle. Winding of the inputs does not
/// matter; both are normalised.
///
/// Returns an empty triangulation if the outline is degenerate. A hole that
/// cannot be bridged is skipped rather than failing the whole profile: a
/// filled part with a missing hole still beats no part at all.
Triangulation triangulate(const std::vector<Point2>& outer,
                          const std::vector<std::vector<Point2>>& holes);

}  // namespace fusenest
