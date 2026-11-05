// A point on the projection plane, split out from profile.h so that geometry
// code can be built and tested without the Fusion headers.
#pragma once

namespace fusenest {

/// A point on the projection plane, in MILLIMETRES. Fusion works in
/// centimetres; the conversion happens once, inside extractProfiles.
struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

}  // namespace fusenest
