// Checks the conjugation write-back depends on, stated as where it sends points.
//
// The property that matters is a round trip: taking a point into root space,
// moving it there, and coming back must land where the conjugated motion sends
// it directly. That is the whole claim the fix rests on, and it also pins the
// multiply ORDER -- the order this file would still pass with reversed is the
// one where frame and motion commute, so the tests below deliberately use a
// frame and a motion that do not.
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
/// placementMatrix, which is the only kind of motion the solver produces.
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

void roundTripHolds(const char* what, const Mat4& frame, const Mat4& motion, P local) {
    Mat4 conjugated{};
    if (!conjugate4(motion, frame, conjugated)) {
        check(false, what);
        return;
    }
    // Moved inside the component, then viewed from the root...
    const P viaLocal = sent(frame, sent(conjugated, local));
    // ...must match being viewed from the root, then moved there.
    const P viaRoot = sent(motion, sent(frame, local));
    check(samePoint(viaLocal, viaRoot), what);
}

}  // namespace

int main() {
    const Mat4 id = identity4();

    // A component that is rotated as well as offset: with a frame like this a
    // reversed conjugation produces a plausible but wrong answer, which is
    // exactly the failure mode the live bug had.
    const Mat4 tilted = multiply4(motionZ(30.0, 100.0, -40.0), identity4());
    const Mat4 move = motionZ(90.0, 250.0, 10.0);

    roundTripHolds("round trip: tilted frame, rotate and translate", tilted, move, P{7, 3, 2});
    roundTripHolds("round trip: tilted frame, pure translation", tilted, motionZ(0.0, 5.0, 0.0),
                   P{-2, 11, 0});
    roundTripHolds("round trip: offset-only frame", motionZ(0.0, 60.0, 60.0), move, P{1, 1, 1});
    roundTripHolds("round trip: identity frame", id, move, P{4, 5, 6});

    // A body in the root component must be left exactly as it was: this is the
    // path that already worked, and the fix must not perturb it.
    {
        Mat4 out{};
        const bool ok = conjugate4(move, id, out);
        check(ok && out == move, "identity frame leaves the motion untouched");
    }

    // The bug, stated directly. A pure +X translation inside a component turned
    // 90 degrees has to become a translation along the component's own -Y, or
    // the part slides the wrong way.
    {
        Mat4 out{};
        const bool ok = conjugate4(motionZ(0.0, 10.0, 0.0), motionZ(90.0, 0.0, 0.0), out);
        const P p = sent(out, P{0, 0, 0});
        check(ok && samePoint(p, P{0, -10, 0}), "a root +X move becomes -Y inside a turned component");
    }

    {
        Mat4 inverse{};
        const bool ok = rigidInverse4(tilted, inverse);
        const P there_and_back = sent(multiply4(tilted, inverse), P{3, 4, 5});
        check(ok && samePoint(there_and_back, P{3, 4, 5}), "rigid inverse undoes the frame");
    }

    // Refused rather than approximated: a non-rigid frame would make the
    // transpose shortcut silently wrong.
    {
        Mat4 scaled = identity4();
        scaled[0] = 2.0;
        Mat4 out{};
        check(!isRigid4(scaled) && !conjugate4(move, scaled, out), "a scaled frame is refused");

        Mat4 sheared = identity4();
        sheared[1] = 0.5;
        check(!isRigid4(sheared), "a sheared frame is refused");
    }

    // Mirrored frames are allowed on purpose -- orthogonal with determinant -1,
    // which the transpose still inverts exactly.
    {
        Mat4 mirrored = identity4();
        mirrored[0] = -1.0;
        check(isRigid4(mirrored), "a mirrored frame is accepted");
        roundTripHolds("round trip: mirrored frame", mirrored, move, P{2, 9, 1});
    }

    // The nesting bug, as maths. occurrenceFrame walks leaf-first and
    // left-multiplies each enclosing transform; this is that loop, checked
    // against applying the two frames one after the other by hand. Reading only
    // the leaf -- the original mistake -- lands the point somewhere else
    // entirely, which the second check pins down.
    {
        const Mat4 outer = motionZ(25.0, 300.0, 0.0);   // component A under the root
        const Mat4 inner = motionZ(-40.0, 0.0, 80.0);   // component B inside A

        Mat4 chain = identity4();
        for (const Mat4& t : {inner, outer}) chain = multiply4(t, chain);

        const P local{6, -2, 3};
        check(samePoint(sent(chain, local), sent(outer, sent(inner, local))),
              "a two-level chain composes leaf-first");
        check(!samePoint(sent(inner, local), sent(chain, local)),
              "the leaf transform alone is not the chain");

        roundTripHolds("round trip: two-level chain", chain, move, local);
    }

    std::printf(g_failures ? "\n%d failure(s)\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
