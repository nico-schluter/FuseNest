#include "profile.h"
#include "log.h"

#include <Fusion/FusionAll.h>

#include <cmath>
#include <sstream>
#include <utility>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

constexpr double kCmToMm = 10.0;
constexpr double kCm2ToMm2 = 100.0;

/// The projection plane is the root XY plane, so the in-plane coordinates are
/// simply x and y. A general plane would need its u/v axes instead.
Point2 toMm(const Ptr<Point3D>& p) {
    return Point2{p->x() * kCmToMm, p->y() * kCmToMm};
}

bool samePoint(const Point2& a, const Point2& b, double epsMm) {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return (dx * dx + dy * dy) <= epsMm * epsMm;
}

/// Walks one loop in order and tessellates it into a closed ring.
///
/// Order comes from coEdges()->next() rather than the collection's index order,
/// and each edge's point list is reversed when isOpposedToEdge() is true --
/// without that the ring comes out with inconsistent direction and the polygon
/// self-intersects.
bool ringFromLoop(const Ptr<BRepLoop>& loop, double toleranceCm, double epsMm,
                  std::vector<Point2>& out, std::string& error) {
    out.clear();

    Ptr<BRepCoEdges> coEdges = loop ? loop->coEdges() : nullptr;
    if (!coEdges || coEdges->count() == 0) {
        error = "loop has no co-edges";
        return false;
    }

    Ptr<BRepCoEdge> start = coEdges->item(0);
    Ptr<BRepCoEdge> ce = start;
    const size_t guard = coEdges->count() + 1;

    for (size_t step = 0; step < guard && ce; ++step) {
        Ptr<BRepEdge> edge = ce->edge();
        Ptr<CurveEvaluator3D> ev = edge ? edge->evaluator() : nullptr;
        if (!ev) {
            error = "edge has no evaluator";
            return false;
        }

        double from = 0.0, to = 0.0;
        if (!ev->getParameterExtents(from, to)) {
            error = "getParameterExtents failed";
            return false;
        }

        std::vector<Ptr<Point3D>> pts;
        if (!ev->getStrokes(from, to, toleranceCm, pts) || pts.size() < 2) {
            error = "getStrokes failed or returned a degenerate edge";
            return false;
        }

        if (ce->isOpposedToEdge())
            for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
                const Point2 p = toMm(*it);
                if (out.empty() || !samePoint(out.back(), p, epsMm)) out.push_back(p);
            }
        else
            for (const auto& raw : pts) {
                const Point2 p = toMm(raw);
                if (out.empty() || !samePoint(out.back(), p, epsMm)) out.push_back(p);
            }

        ce = ce->next();
        if (ce == start) break;
    }

    // jagua-rs strips a repeated closing vertex, but dropping it here keeps our
    // own area and point counts honest.
    if (out.size() > 1 && samePoint(out.front(), out.back(), epsMm)) out.pop_back();

    if (out.size() < 3) {
        error = "ring collapsed to fewer than 3 points";
        return false;
    }
    return true;
}

void collectBodies(const Ptr<Base>& sel, const std::string& prefix,
                   std::vector<std::pair<std::string, Ptr<BRepBody>>>& out) {
    if (!sel) return;

    if (Ptr<BRepBody> body = sel->cast<BRepBody>()) {
        // A surface body has no thickness, so its projected outline says
        // nothing useful about material consumption. The selection filter
        // already excludes them; this guards the occurrence path below.
        if (!body->isSolid()) return;
        out.emplace_back(prefix.empty() ? body->name() : prefix + "/" + body->name(), body);
        return;
    }

    if (Ptr<Occurrence> occ = sel->cast<Occurrence>()) {
        const std::string name = prefix.empty() ? occ->name() : prefix + "/" + occ->name();
        // bRepBodies() on an occurrence returns proxies in assembly context, so
        // the geometry is already positioned.
        if (Ptr<BRepBodies> bodies = occ->bRepBodies())
            for (size_t i = 0; i < bodies->count(); ++i)
                if (Ptr<BRepBody> b = bodies->item(i))
                    if (b->isSolid()) out.emplace_back(name + "/" + b->name(), b);

        if (Ptr<OccurrenceList> children = occ->childOccurrences())
            for (size_t i = 0; i < children->count(); ++i) collectBodies(children->item(i), name, out);
    }
}

}  // namespace

ExtractResult extractProfiles(const Ptr<Application>& app,
                              const std::vector<Ptr<Base>>& selections,
                              const ExtractOptions& options) {
    ExtractResult result;

    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    Ptr<Component> root = design ? design->rootComponent() : nullptr;
    Ptr<ConstructionPlane> xy = root ? root->xYConstructionPlane() : nullptr;
    Ptr<Plane> plane = xy ? xy->geometry() : nullptr;
    Ptr<TemporaryBRepManager> mgr = TemporaryBRepManager::get();

    if (!plane || !mgr) {
        result.warnings.push_back("no active design, or the temporary BRep manager is unavailable");
        return result;
    }

    std::vector<std::pair<std::string, Ptr<BRepBody>>> bodies;
    for (const auto& sel : selections) collectBodies(sel, "", bodies);
    result.bodiesExamined = static_cast<int>(bodies.size());

    const double toleranceCm = options.toleranceMm / kCmToMm;
    // Far below the chord tolerance, so real vertices are never merged -- this
    // only collapses the duplicate shared by two adjacent edges.
    const double epsMm = options.toleranceMm * 0.01;

    for (const auto& [name, body] : bodies) {
        bool approximated = false;
        Ptr<BRepBody> outline = mgr->createProjectedBodyOutline(body, plane, toleranceCm, approximated);
        if (!outline) {
            std::string err;
            app->getLastError(&err);
            result.warnings.push_back(name + ": createProjectedBodyOutline failed (" + err + ")");
            continue;
        }

        Ptr<BRepFaces> faces = outline->faces();
        const size_t faceCount = faces ? faces->count() : 0;
        if (faceCount == 0) {
            result.warnings.push_back(name + ": outline has no faces");
            continue;
        }
        if (faceCount > 1) {
            // Only reachable for a multi-lump body: the projection of a single
            // connected solid is always connected. Split rather than fail, and
            // say so.
            std::ostringstream os;
            os << name << ": projects to " << faceCount
               << " disconnected regions (multi-lump body) -- split into that many profiles";
            result.warnings.push_back(os.str());
        }

        for (size_t f = 0; f < faceCount; ++f) {
            Ptr<BRepFace> face = faces->item(f);
            Ptr<BRepLoops> loops = face ? face->loops() : nullptr;
            if (!loops || loops->count() == 0) {
                result.warnings.push_back(name + ": face has no loops");
                continue;
            }

            Profile profile;
            profile.name = faceCount > 1 ? name + " [" + std::to_string(f + 1) + "]" : name;
            profile.source = body;
            profile.silhouetteApproximated = approximated;
            profile.areaMm2 = face->area() * kCm2ToMm2;

            bool ok = true;
            for (size_t l = 0; l < loops->count(); ++l) {
                Ptr<BRepLoop> loop = loops->item(l);
                std::vector<Point2> ring;
                std::string error;
                if (!ringFromLoop(loop, toleranceCm, epsMm, ring, error)) {
                    result.warnings.push_back(profile.name + ": " + error);
                    ok = false;
                    break;
                }
                // isOuter() tells us outright which ring is the boundary -- no
                // containment test or area-sign heuristic needed.
                if (loop->isOuter())
                    profile.outer = std::move(ring);
                else
                    profile.holes.push_back(std::move(ring));
            }

            if (!ok) continue;
            if (profile.outer.empty()) {
                result.warnings.push_back(profile.name + ": no outer loop found");
                continue;
            }

            result.holesDropped += static_cast<int>(profile.holes.size());
            logLine("profile " + profile.name + ": " + std::to_string(profile.outer.size()) +
                    " outer points, " + std::to_string(profile.holes.size()) + " holes, " +
                    std::to_string(profile.areaMm2) + " mm2" +
                    (profile.silhouetteApproximated ? ", silhouette approximated" : ""));
            result.profiles.push_back(std::move(profile));
        }
    }

    return result;
}

}  // namespace fusenest
