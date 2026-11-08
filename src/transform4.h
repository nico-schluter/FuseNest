// A 4x4 rigid transform as a flat array, split out from writeback.cpp so the
// composition write-back depends on can be checked without Fusion.
//
// What depends on it: moving a component instance means composing the
// placement with the transform the instance already has, and composing them
// the other way round is a plausible wrong answer rather than an obvious one.
// The order is pinned by transform4_test, which is the only place it can be.
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

/// Applies `m` to a point, for tests: the cheapest way to state what a matrix
/// means is where it sends a point.
void apply4(const Mat4& m, double& x, double& y, double& z);

}  // namespace fusenest
