// Checks the composition write-back depends on, stated as where it sends points.
//
// Moving a component instance means re-seating its transform: the instance sits
// at T, the solver asks for a motion M in root space, and the new transform is
// M * T. The property that matters is that the body inside then lands exactly
// where applying M to its old root position would put it -- and that pins the
// multiply ORDER, which is the one thing here that can be wrong while looking
// right. T * M is a rigid transform too, and it puts the part somewhere
// plausible.
//
// So every test below uses a frame and a motion that do NOT commute. Reversed
// multiplication passes trivially on the cases where they do, which is why a
// rotation-free component or an offset-free motion proves nothing.
#include "transform4.h"

#include <cmath>
#include <cstdio>

using namespace fusenest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

/// Rotation about Z by `deg`, then a translation -- the same shape as
/// placementMatrix, which is the only kind of motion the solver produces, and
/// the same shape as an occurrence transform that has only ever been moved in
/// the plane.
Mat4 motionZ(double deg, double tx, double ty) {
    const double r = deg * M_PI / 180.0;
    const double c = std::cos(r), s = std::sin(r);
    return Mat4{c, -s, 0, tx,
                s,  c, 0, ty,
                0,  0, 1, 0,
                0,  0, 0, 1};
}

struct P {
    double x, y, z;
};

P sent(const Mat4& m, P p) {
    apply4(m, p.x, p.y, p.z);
    return p;
}

bool samePoint(P a, P b, double eps = 1e-9) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

/// The instance claim: a point of the component, carried into root space by the
/// RE-SEATED transform, lands where the motion sends the point's old root
/// position. `local` is in the component's own space, which is where its bodies
/// are defined.
void placementHolds(const char* what, const Mat4& frame, const Mat4& motion, P local) {
    const Mat4 reseated = multiply4(motion, frame);
    check(samePoint(sent(reseated, local), sent(motion, sent(frame, local))), what);
}

}  // namespace

int main() {
    // A component that is rotated as well as offset. With a frame like this,
    // composing the other way round produces a plausible but wrong answer --
    // the failure this file exists to catch.
    const Mat4 tilted = motionZ(30.0, 100.0, -40.0);
    const Mat4 move = motionZ(90.0, 250.0, 10.0);

    placementHolds("re-seating: tilted instance, rotate and translate", tilted, move, P{7, 3, 2});
    placementHolds("re-seating: tilted instance, pure translation", tilted, motionZ(0.0, 5.0, 0.0),
                   P{-2, 11, 0});
    placementHolds("re-seating: offset-only instance", motionZ(0.0, 60.0, 60.0), move, P{1, 1, 1});
    placementHolds("re-seating: instance at the origin", identity4(), move, P{4, 5, 6});

    // An instance sitting at the identity must be moved by the placement
    // itself, bit for bit: that is the case where the component's own space IS
    // root space, and anything but equality means the composition is adding
    // something of its own.
    {
        const Mat4 reseated = multiply4(move, identity4());
        check(reseated == move, "an instance at the identity takes the placement unchanged");
    }

    // The order, stated as the thing that goes wrong. Reversing it is not a
    // small error: the same +X placement on a component turned 90 degrees ends
    // up 250mm along the component's own Y instead of the root's X.
    {
        const Mat4 turned = motionZ(90.0, 0.0, 0.0);
        const P origin{0, 0, 0};
        check(samePoint(sent(multiply4(move, turned), origin), P{250, 10, 0}),
              "placement * transform moves the instance in ROOT space");
        check(!samePoint(sent(multiply4(turned, move), origin), P{250, 10, 0}),
              "transform * placement moves it somewhere else");
    }

    // Composition is associative and multiply4 has to be: write-back composes
    // once per instance, but a second nest re-seats an already re-seated
    // transform, and two placements applied in turn must equal their product.
    {
        const Mat4 second = motionZ(-15.0, -30.0, 12.0);
        const P local{6, -2, 3};
        check(samePoint(sent(multiply4(second, multiply4(move, tilted)), local),
                        sent(second, sent(move, sent(tilted, local)))),
              "two placements in turn equal their composition");
    }

    // Confirming that an instance took its new transform is a cell-wise
    // comparison, and write-back reads a false from it as "the move did not
    // happen". So it has to be tight enough to catch a position that did not
    // take, and loose enough that a matrix which merely went through Fusion and
    // came back still compares equal to itself.
    {
        const Mat4 seated = multiply4(move, tilted);
        check(approxEqual4(seated, seated, 1e-6), "a transform equals itself");

        Mat4 rounded = seated;
        rounded[3] += 1e-9;   // translation, centimetres
        rounded[0] += 1e-9;   // rotation
        check(approxEqual4(rounded, seated, 1e-6),
              "a transform that only rounded still compares equal");

        Mat4 unmoved = tilted;
        check(!approxEqual4(unmoved, seated, 1e-6),
              "an instance left where it was does NOT compare equal");

        Mat4 offBy10um = seated;
        offBy10um[7] += 1e-3;  // 10 microns in centimetres
        check(!approxEqual4(offBy10um, seated, 1e-6),
              "ten microns out of position does not compare equal");
    }

    std::printf(g_failures ? "\n%d failure(s)\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
