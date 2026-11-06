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

/// Plane and manager every projection needs. Both callers fail the same way
/// when either is missing, so the check lives with the lookup.
struct ProjectionContext {
    Ptr<Plane> plane;
    Ptr<TemporaryBRepManager> mgr;
    bool ok() const { return plane && mgr; }
};

ProjectionContext projectionContext(const Ptr<Application>& app) {
    ProjectionContext ctx;
    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    Ptr<Component> root = design ? design->rootComponent() : nullptr;
    Ptr<ConstructionPlane> xy = root ? root->xYConstructionPlane() : nullptr;
    ctx.plane = xy ? xy->geometry() : nullptr;
    ctx.mgr = TemporaryBRepManager::get();
    return ctx;
}

/// The one-body-one-item rule. `faceCount` comes from the projected outline.
///
/// Both the live check and extraction decide here, so the dialog and the
/// refusal can never disagree about what is nestable, and the wording the user
/// reads is written once.
bool nestableAsOneItem(const Ptr<BRepBody>& body, size_t faceCount, std::string& reason) {
    if (faceCount <= 1) return true;

    // A connected solid reaches here too: its silhouette only has to pinch to a
    // point for the projection to come back as faces meeting at that vertex. So
    // ask the body how many lumps it has instead of inferring disconnection
    // from the face count, which is what an earlier comment here got wrong.
    Ptr<BRepLumps> lumps = body ? body->lumps() : nullptr;
    const size_t lumpCount = lumps ? lumps->count() : 0;

    std::ostringstream os;
    os << "projects to " << faceCount << " separate regions "
       << (lumpCount > 1 ? "(the body is in " + std::to_string(lumpCount) + " lumps)"
                         : "(the silhouette pinches to a point)")
       << " and cannot be nested as one rigid body";
    reason = os.str();
    return false;
}

}  // namespace

ExtractResult extractProfiles(const Ptr<Application>& app,
                              const std::vector<Ptr<Base>>& selections,
                              const ExtractOptions& options) {
    ExtractResult result;

    const ProjectionContext ctx = projectionContext(app);
    if (!ctx.ok()) {
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
        Ptr<BRepBody> outline =
            ctx.mgr->createProjectedBodyOutline(body, ctx.plane, toleranceCm, approximated);
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
        // Refuse the body rather than split it. Splitting was the old behaviour
        // and it is silently wrong: each region nests as its own item, then
        // write-back issues one move feature per placement against the SAME
        // body, and they compose. Two placements at opposite rotations cancel to
        // no rotation and a fraction of the intended travel, so the part reads
        // as one that simply refused to move -- with a success report next to it.
        std::string reason;
        if (!nestableAsOneItem(body, faceCount, reason)) {
            result.blockers.push_back(BodyIssue{name, reason});
            continue;
        }

        // One face, guaranteed by the refusal above: a body that projected to
        // more than one region never reaches here.
        Ptr<BRepFace> face = faces->item(0);
        Ptr<BRepLoops> loops = face ? face->loops() : nullptr;
        if (!loops || loops->count() == 0) {
            result.warnings.push_back(name + ": face has no loops");
            continue;
        }

        Profile profile;
        profile.name = name;
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

    return result;
}

std::vector<BodyIssue> validateBodies(const Ptr<Application>& app,
                                      const std::vector<Ptr<Base>>& selections,
                                      const ExtractOptions& options) {
    std::vector<BodyIssue> issues;

    const ProjectionContext ctx = projectionContext(app);
    if (!ctx.ok()) return issues;  // nothing to say; extraction reports it

    std::vector<std::pair<std::string, Ptr<BRepBody>>> bodies;
    for (const auto& sel : selections) collectBodies(sel, "", bodies);

    const double toleranceCm = options.toleranceMm / kCmToMm;
    for (const auto& [name, body] : bodies) {
        bool approximated = false;
        Ptr<BRepBody> outline =
            ctx.mgr->createProjectedBodyOutline(body, ctx.plane, toleranceCm, approximated);
        // A projection that fails outright is left to extraction, which has the
        // error text and a warning channel for it. Saying nothing here keeps
        // this list to what it promises: bodies that are definitely unnestable.
        if (!outline) continue;

        Ptr<BRepFaces> faces = outline->faces();
        std::string reason;
        if (!nestableAsOneItem(body, faces ? faces->count() : 1, reason))
            issues.push_back(BodyIssue{name, reason});
    }
    return issues;
}

}  // namespace fusenest
