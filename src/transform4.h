// A 4x4 rigid transform as a flat array, split out from writeback.cpp so the
// conjugation it exists for can be checked without Fusion.
//
// Conventions are Matrix3D's, so an array can be handed straight to
// setWithArray: ROW-MAJOR storage, and a point is a COLUMN vector on the right.
// `multiply4(a, b)` is therefore the transform that applies b first, then a.
#pragma once

#include <array>

namespace fusenest {

/// Row-major: element (row, col) is at [row * 4 + col].
using Mat4 = std::array<double, 16>;

Mat4 identity4();

/// a * b -- b applied first.
Mat4 multiply4(const Mat4& a, const Mat4& b);

/// Whether the 3x3 part is orthonormal and the bottom row is [0 0 0 1], which
/// is what `rigidInverse4` needs to be allowed to transpose instead of solve.
///
/// The determinant's SIGN is deliberately not checked. A mirrored occurrence
/// has an orthogonal frame with determinant -1, and transposing still inverts
/// it exactly, so the conjugation below stays correct; refusing it would reject
/// a case that works.
bool isRigid4(const Mat4& m, double eps = 1e-9);

/// Inverse by transposing the rotation and negating the rotated translation.
/// False when `m` is not rigid, rather than returning a wrong inverse.
bool rigidInverse4(const Mat4& m, Mat4& out);

/// Re-expresses `motion` -- a rigid motion in some outer space -- in the space
/// that `frame` maps into that outer space: frame^-1 * motion * frame.
///
/// False when `frame` is not rigid.
bool conjugate4(const Mat4& motion, const Mat4& frame, Mat4& out);

/// Applies `m` to a point, for tests: the cheapest way to state what a matrix
/// means is where it sends a point.
void apply4(const Mat4& m, double& x, double& y, double& z);

}  // namespace fusenest
