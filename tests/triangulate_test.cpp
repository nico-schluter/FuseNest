// Checks the triangulator against outlines whose area we know in advance.
//
// Area is the test that matters: a triangulation that covers the right area
// with the right winding and never overlaps is a correct fill, and all three
// of those are visible in the numbers without rendering anything.
#include <numbers>
#include "triangulate.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace fusenest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::vector<Point2> rect(double x, double y, double w, double h) {
    return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
}

std::vector<Point2> circle(double cx, double cy, double r, int n) {
    std::vector<Point2> out;
    for (int i = 0; i < n; ++i) {
        double a = 2.0 * std::numbers::pi * i / n;
        out.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return out;
}

double ringArea(const std::vector<Point2>& r) {
    double s = 0.0;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        s += r[j].x * r[i].y - r[i].x * r[j].y;
    return std::fabs(s) * 0.5;
}

/// Summed area of the emitted triangles. Signed, so a triangle wound the wrong
/// way subtracts -- which is how a flipped winding shows up as a wrong total
/// rather than passing silently.
double meshArea(const Triangulation& t) {
    double s = 0.0;
    for (std::size_t i = 0; i + 2 < t.indices.size(); i += 3) {
        const Point2& a = t.vertices[t.indices[i]];
        const Point2& b = t.vertices[t.indices[i + 1]];
        const Point2& c = t.vertices[t.indices[i + 2]];
        s += ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5;
    }
    return s;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void expectArea(const char* what, const std::vector<Point2>& outer,
                const std::vector<std::vector<Point2>>& holes, double expected, double tol) {
    Triangulation t = triangulate(outer, holes);
    double got = meshArea(t);
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s: area %.4f, expected %.4f (%zu triangles)", what, got,
                  expected, t.indices.size() / 3);
    check(near(got, expected, tol), msg);
}

}  // namespace

int main() {
    expectArea("square, no holes", rect(0, 0, 10, 10), {}, 100.0, 1e-9);

    expectArea("square with one square hole", rect(0, 0, 10, 10), {rect(3, 3, 4, 4)},
               100.0 - 16.0, 1e-9);

    expectArea("square with two holes", rect(0, 0, 10, 10), {rect(1, 1, 2, 2), rect(6, 6, 2, 2)},
               100.0 - 8.0, 1e-9);

    // The shape in the screenshot: a washer. Curves arrive already tessellated.
    {
        std::vector<Point2> outer = circle(0, 0, 5, 64);
        std::vector<Point2> hole = circle(0, 0, 2, 32);
        expectArea("washer", outer, {hole}, ringArea(outer) - ringArea(hole), 1e-9);
    }

    // A hole near the boundary: the shortest bridge is not the obvious one.
    expectArea("off-centre hole", rect(0, 0, 10, 10), {rect(8.5, 4, 1, 1)}, 100.0 - 1.0, 1e-9);

    // Concave outline, which is where naive ear clipping goes wrong.
    {
        std::vector<Point2> l = {{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}};
        expectArea("L-shape", l, {}, ringArea(l), 1e-9);
        expectArea("L-shape with hole", l, {rect(1, 1, 2, 2)}, ringArea(l) - 4.0, 1e-9);
    }

    // Winding must not matter to the caller.
    {
        std::vector<Point2> cw = rect(0, 0, 10, 10);
        std::reverse(cw.begin(), cw.end());
        expectArea("clockwise input is normalised", cw, {}, 100.0, 1e-9);
    }

    // Many holes, the perforated-sheet case.
    {
        std::vector<std::vector<Point2>> holes;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) holes.push_back(circle(1.5 + i * 1.75, 1.5 + j * 1.75, 0.5, 16));
        double expected = 100.0;
        for (const auto& h : holes) expected -= ringArea(h);
        expectArea("25 round holes", rect(0, 0, 10, 10), holes, expected, 1e-6);
    }

    // Degenerate input must not crash or emit anything.
    check(triangulate({{0, 0}, {1, 1}}, {}).indices.empty(), "two points produce no triangles");
    check(triangulate({}, {}).indices.empty(), "empty outline produces no triangles");

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all pass");
    return g_failures ? 1 : 0;
}
