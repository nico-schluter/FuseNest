#include "triangulate.h"

#include <algorithm>
#include <cmath>

namespace fusenest {
namespace {

/// Twice the signed area of the triangle abc. Positive means counter-clockwise.
double cross(const Point2& a, const Point2& b, const Point2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// Twice the signed area of a ring. Positive means counter-clockwise.
double signedArea2(const std::vector<Point2>& ring) {
    double sum = 0.0;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        sum += ring[j].x * ring[i].y - ring[i].x * ring[j].y;
    return sum;
}

/// Cross products are areas, so they scale as the square of the coordinates.
/// Profiles are millimetres and a hole may be a fraction of one, hence a
/// tolerance well below any area we would call real.
constexpr double kEps = 1e-12;

int sign(double v) { return v > kEps ? 1 : (v < -kEps ? -1 : 0); }

bool coincident(const Point2& a, const Point2& b) {
    return std::fabs(a.x - b.x) <= 1e-9 && std::fabs(a.y - b.y) <= 1e-9;
}

bool pointInTriangle(const Point2& a, const Point2& b, const Point2& c, const Point2& p) {
    // A point sitting exactly ON an edge still blocks the ear -- but a point
    // coincident with a CORNER does not. Bridging deliberately duplicates two
    // vertices, so every bridged hole leaves a pair of exact duplicates in the
    // ring; treating those as blockers would leave no clippable ear anywhere
    // and the fill would come out empty.
    if (coincident(p, a) || coincident(p, b) || coincident(p, c)) return false;
    return cross(a, b, p) >= -kEps && cross(b, c, p) >= -kEps && cross(c, a, p) >= -kEps;
}

bool segmentsProperlyIntersect(const Point2& p1, const Point2& p2, const Point2& q1,
                               const Point2& q2) {
    // Three-state signs, and both segments must straddle the other STRICTLY.
    // A zero means touching or collinear, which is how a bridge is supposed to
    // meet the ring it joins: comparing `(d1 > 0) != (d2 > 0)` instead folds
    // zero in with "negative" and rejects every legal bridge.
    int d1 = sign(cross(q1, q2, p1)), d2 = sign(cross(q1, q2, p2));
    int d3 = sign(cross(p1, p2, q1)), d4 = sign(cross(p1, p2, q2));
    return d1 * d2 < 0 && d3 * d4 < 0;
}

/// Can `a` see `b` without the segment cutting through any ring?
bool bridgeIsClear(const Point2& a, const Point2& b, const std::vector<Point2>& outer,
                   const std::vector<std::vector<Point2>>& holes, std::size_t skipHole) {
    for (std::size_t i = 0, j = outer.size() - 1; i < outer.size(); j = i++)
        if (segmentsProperlyIntersect(a, b, outer[j], outer[i])) return false;
    for (std::size_t h = 0; h < holes.size(); ++h) {
        const std::vector<Point2>& ring = holes[h];
        if (ring.size() < 3) continue;
        for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
            if (h == skipHole) {
                // The hole being bridged touches its own bridge at one vertex;
                // only a genuine crossing of its other edges disqualifies it.
                if (segmentsProperlyIntersect(a, b, ring[j], ring[i])) return false;
            } else if (segmentsProperlyIntersect(a, b, ring[j], ring[i])) {
                return false;
            }
        }
    }
    return true;
}

double distance2(const Point2& a, const Point2& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

Triangulation triangulate(const std::vector<Point2>& outerIn,
                          const std::vector<std::vector<Point2>>& holesIn) {
    Triangulation out;
    if (outerIn.size() < 3) return out;

    // Normalise winding: outer counter-clockwise, holes clockwise. Ear clipping
    // below assumes a counter-clockwise polygon, and a hole walked the other
    // way is what makes the bridged ring stay simple.
    std::vector<Point2> outer = outerIn;
    if (signedArea2(outer) < 0.0) std::reverse(outer.begin(), outer.end());

    std::vector<std::vector<Point2>> holes;
    holes.reserve(holesIn.size());
    for (const std::vector<Point2>& h : holesIn) {
        if (h.size() < 3) continue;
        holes.push_back(h);
        if (signedArea2(holes.back()) > 0.0)
            std::reverse(holes.back().begin(), holes.back().end());
    }

    // Bridge holes in, rightmost first. Working from the right means each
    // bridge is cut by as few remaining holes as possible.
    std::vector<std::size_t> order(holes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        auto maxX = [&](std::size_t k) {
            double m = holes[k][0].x;
            for (const Point2& p : holes[k]) m = std::max(m, p.x);
            return m;
        };
        return maxX(a) > maxX(b);
    });

    std::vector<Point2> ring = outer;
    std::vector<bool> bridged(holes.size(), false);
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        std::size_t h = order[oi];
        const std::vector<Point2>& hole = holes[h];

        // Shortest clear bridge. Eberly's ray cast is the textbook answer and
        // is O(n); this is O(n*m) but runs once per profile at build time, and
        // being able to reason about it is worth more here than the exponent.
        std::size_t bestRing = ring.size(), bestHole = hole.size();
        double bestDist = 0.0;
        for (std::size_t a = 0; a < hole.size(); ++a) {
            for (std::size_t b = 0; b < ring.size(); ++b) {
                double d = distance2(hole[a], ring[b]);
                if (bestRing != ring.size() && d >= bestDist) continue;
                // Holes already bridged are part of `ring` and are checked by
                // the ring sweep, so only unbridged ones are passed here.
                std::vector<std::vector<Point2>> pending;
                std::size_t skip = 0;
                for (std::size_t k = 0; k < holes.size(); ++k)
                    if (!bridged[k]) {
                        if (k == h) skip = pending.size();
                        pending.push_back(holes[k]);
                    }
                if (!bridgeIsClear(hole[a], ring[b], ring, pending, skip)) continue;
                bestRing = b;
                bestHole = a;
                bestDist = d;
            }
        }
        if (bestRing == ring.size()) continue;  // unbridgeable; drop this hole

        std::vector<Point2> merged;
        merged.reserve(ring.size() + hole.size() + 2);
        merged.insert(merged.end(), ring.begin(), ring.begin() + bestRing + 1);
        for (std::size_t k = 0; k < hole.size(); ++k)
            merged.push_back(hole[(bestHole + k) % hole.size()]);
        merged.push_back(hole[bestHole]);          // close the hole walk
        merged.push_back(ring[bestRing]);          // and return to the ring
        merged.insert(merged.end(), ring.begin() + bestRing + 1, ring.end());
        ring.swap(merged);
        bridged[h] = true;
    }

    out.vertices = ring;

    // Ear clipping. O(n^2), build-time only.
    std::vector<int> live(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i) live[i] = static_cast<int>(i);

    while (live.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < live.size(); ++i) {
            const Point2& a = ring[live[(i + live.size() - 1) % live.size()]];
            const Point2& b = ring[live[i]];
            const Point2& c = ring[live[(i + 1) % live.size()]];
            if (cross(a, b, c) <= 0.0) continue;  // reflex or degenerate

            bool clean = true;
            for (std::size_t k = 0; k < live.size() && clean; ++k) {
                if (k == i || k == (i + live.size() - 1) % live.size() ||
                    k == (i + 1) % live.size())
                    continue;
                if (pointInTriangle(a, b, c, ring[live[k]])) clean = false;
            }
            if (!clean) continue;

            out.indices.push_back(live[(i + live.size() - 1) % live.size()]);
            out.indices.push_back(live[i]);
            out.indices.push_back(live[(i + 1) % live.size()]);
            live.erase(live.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        // No ear found on a full sweep means the polygon is self-intersecting
        // or numerically degenerate. Emit what we have rather than spin.
        if (!clipped) break;
    }
    if (live.size() == 3) {
        out.indices.push_back(live[0]);
        out.indices.push_back(live[1]);
        out.indices.push_back(live[2]);
    }
    return out;
}

}  // namespace fusenest
