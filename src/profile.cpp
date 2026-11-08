#include "profile.h"
#include "log.h"

#include <Fusion/FusionAll.h>

#include <cmath>
#include <set>
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

/// One thing to nest, and what moving it will mean.
///
/// `geometry` is always a body already positioned in ROOT space -- the body
/// itself for a root body, a proxy inside the instance otherwise -- because
/// that is the space the outline is projected in and the space a placement is
/// expressed in.
struct NestUnit {
    std::string name;
    Ptr<BRepBody> geometry;
    /// Null to move `geometry` itself; the instance to move otherwise.
    Ptr<Occurrence> instance;
};

/// The units a selection list came to, and the selections refused on the way.
///
/// The dedup is half the point of collecting. A component and the body inside
/// it are the SAME unit -- the instance is what moves either way -- and picking
/// both is the natural thing to do in the browser, where a body sits one
/// disclosure triangle below the component holding it. Counted twice, the
/// solver would be asked for room for a part that does not exist and then two
/// placements would land on one instance.
struct Collected {
    std::vector<NestUnit> units;
    std::vector<NestIssue> rejected;
    /// Identity of every unit already in one of the lists above, so a second
    /// route to the same one is dropped rather than appended. An occurrence's
    /// full path name identifies it within the assembly, and no two bodies in
    /// the root component share a name.
    std::set<std::string> seen;
};

/// Classifies one component instance, which is also where a body inside one
/// ends up -- the two routes must agree, so there is only the one.
void classifyInstance(const Ptr<Occurrence>& occ, Collected& out) {
    if (!occ) return;

    const std::string name = occ->fullPathName();
    if (!out.seen.insert("instance:" + name).second) return;

    // Depth one only. A nested instance's transform is expressed in its
    // PARENT's space, not the root's, and the parent is free to be moved
    // afterwards -- so a layout accepted in root space would not survive. The
    // same path reaches here from a body picked inside a sub-component.
    if (occ->assemblyContext()) {
        out.rejected.push_back(
            NestIssue{name, "is a component inside another component, so its placement would "
                            "not hold; nest the top-level instance instead"});
        return;
    }

    // A child component moves with its parent but is not part of the outline
    // projected from the parent's own body, so the layout would reserve less
    // room than the instance actually takes up. One flat component, one shape.
    Ptr<OccurrenceList> children = occ->childOccurrences();
    const size_t childCount = children ? children->count() : 0;
    if (childCount > 0) {
        out.rejected.push_back(NestIssue{
            name, "contains " + std::to_string(childCount) + " more component" +
                      (childCount == 1 ? "" : "s") +
                      ", whose geometry would move with it but is not in its outline"});
        return;
    }

    // Own bodies only -- there are no children left to walk. Surface bodies are
    // skipped for the reason the selection filter excludes them: no thickness,
    // so a projected outline says nothing about material.
    std::vector<Ptr<BRepBody>> solids;
    if (Ptr<BRepBodies> bodies = occ->bRepBodies())
        for (size_t i = 0; i < bodies->count(); ++i)
            if (Ptr<BRepBody> b = bodies->item(i))
                if (b->isSolid()) solids.push_back(b);

    if (solids.empty()) {
        out.rejected.push_back(NestIssue{name, "has no solid body in it to nest"});
        return;
    }
    // Refused rather than nested as the union of its bodies, for the reason at
    // the top of profile.h: one unit can only carry one placement, so two
    // outlines moving with one instance would compose into neither of them.
    if (solids.size() > 1) {
        out.rejected.push_back(NestIssue{
            name, "holds " + std::to_string(solids.size()) +
                      " solid bodies, and an instance can only be nested when it holds one"});
        return;
    }

    // bRepBodies() on an occurrence returns proxies in assembly context, so the
    // geometry is already where the layout has to reason about it.
    out.units.push_back(NestUnit{name, solids[0], occ});
}

void classifyBody(const Ptr<BRepBody>& body, Collected& out) {
    // The filter already excludes surface bodies; this guards the body a
    // selected occurrence is resolved to.
    if (!body || !body->isSolid()) return;

    // A body inside a component is not the unit: moving it would move it in
    // every instance of that component. Its instance is, and that is true
    // whether the user picked the body or the instance.
    if (Ptr<Occurrence> context = body->assemblyContext()) {
        classifyInstance(context, out);
        return;
    }

    const std::string name = body->name();
    if (!out.seen.insert("body:" + name).second) return;
    out.units.push_back(NestUnit{name, body, nullptr});
}

/// Turns what the dialog handed us into units to nest and selections refused.
///
/// Shared by the live check and extraction, so the dialog cannot accept a
/// selection extraction would then refuse -- and so both describe a refusal in
/// the same words.
Collected collectUnits(const std::vector<Ptr<Base>>& selections) {
    Collected out;
    for (const Ptr<Base>& sel : selections) {
        if (!sel) continue;
        if (Ptr<BRepBody> body = sel->cast<BRepBody>()) {
            classifyBody(body, out);
        } else if (Ptr<Occurrence> occ = sel->cast<Occurrence>()) {
            classifyInstance(occ, out);
        }
    }
    return out;
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

/// The one-outline-one-item rule. `faceCount` comes from the projected outline.
///
/// Both the live check and extraction decide here, so the dialog and the
/// refusal can never disagree about what is nestable, and the wording the user
/// reads is written once. It applies to an instance exactly as it does to a
/// body: the single body inside the instance is what was projected.
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
       << " and cannot be nested as one rigid piece";
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

    Collected collected = collectUnits(selections);
    // Classification refusals go straight through. They are decided before any
    // geometry is read -- they are about what a unit IS -- and extraction has
    // nothing to add to them.
    result.blockers = collected.rejected;
    result.unitsExamined = static_cast<int>(collected.units.size());

    const double toleranceCm = options.toleranceMm / kCmToMm;
    // Far below the chord tolerance, so real vertices are never merged -- this
    // only collapses the duplicate shared by two adjacent edges.
    const double epsMm = options.toleranceMm * 0.01;

    for (const NestUnit& unit : collected.units) {
        const std::string& name = unit.name;
        const Ptr<BRepBody>& body = unit.geometry;

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
            result.blockers.push_back(NestIssue{name, reason});
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
        profile.instance = unit.instance;
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
                (profile.silhouetteApproximated ? ", silhouette approximated" : "") +
                (unit.instance ? ", moves as a component" : ""));
        result.profiles.push_back(std::move(profile));
    }

    return result;
}

std::vector<NestIssue> validateSelections(const Ptr<Application>& app,
                                          const std::vector<Ptr<Base>>& selections,
                                          const ExtractOptions& options) {
    Collected collected = collectUnits(selections);
    std::vector<NestIssue> issues = std::move(collected.rejected);

    const ProjectionContext ctx = projectionContext(app);
    if (!ctx.ok()) return issues;  // the geometry half is extraction's to report

    const double toleranceCm = options.toleranceMm / kCmToMm;
    for (const NestUnit& unit : collected.units) {
        bool approximated = false;
        Ptr<BRepBody> outline = ctx.mgr->createProjectedBodyOutline(unit.geometry, ctx.plane,
                                                                    toleranceCm, approximated);
        // A projection that fails outright is left to extraction, which has the
        // error text and a warning channel for it. Saying nothing here keeps
        // this list to what it promises: selections that are definitely
        // unnestable.
        if (!outline) continue;

        Ptr<BRepFaces> faces = outline->faces();
        std::string reason;
        if (!nestableAsOneItem(unit.geometry, faces ? faces->count() : 1, reason))
            issues.push_back(NestIssue{unit.name, reason});
    }
    return issues;
}

}  // namespace fusenest
