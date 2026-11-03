#include "writeback.h"
#include "log.h"
#include "timing.h"
#include "nest_kernel.h"

#include <Fusion/FusionAll.h>

#include <cmath>
#include <sstream>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

constexpr double kMmToCm = 0.1;

/// Rotation about Z through the origin, then a translation in XY.
///
/// This is valid precisely because the outline was projected onto the XY plane
/// along Z: the body's XY silhouette transforms exactly as the polygon did, and
/// Z is left alone.
Ptr<Matrix3D> placementMatrix(const NkPlacement& p) {
    Ptr<Matrix3D> m = Matrix3D::create();
    if (!m) return nullptr;

    Ptr<Vector3D> zAxis = Vector3D::create(0.0, 0.0, 1.0);
    Ptr<Point3D> origin = Point3D::create(0.0, 0.0, 0.0);
    if (!m->setToRotation(p.rotation * M_PI / 180.0, zAxis, origin)) return nullptr;

    // Solver output is millimetres; everything in Fusion is centimetres.
    if (!m->translation(Vector3D::create(p.tx * kMmToCm, p.ty * kMmToCm, 0.0))) return nullptr;
    return m;
}

}  // namespace

WriteBackResult applyPlacements(const Ptr<Application>& app, const std::vector<Profile>& profiles,
                                const std::vector<NkPlacement>& placements, bool groupTimeline) {
    ScopedTime _total("writeback.total");
    WriteBackResult result;

    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    Ptr<Component> root = design ? design->rootComponent() : nullptr;
    if (!root) {
        result.warnings.push_back("no active design");
        return result;
    }

    Ptr<MoveFeatures> moves = root->features() ? root->features()->moveFeatures() : nullptr;
    if (!moves) {
        result.warnings.push_back("move features unavailable");
        return result;
    }

    const bool parametric = design->designType() == ParametricDesignType;
    Ptr<Timeline> timeline = parametric ? design->timeline() : nullptr;
    const int markerBefore = timeline ? timeline->markerPosition() : 0;

    if (placements.empty()) {
        result.warnings.push_back("no placements to apply");
        return result;
    }

    for (const NkPlacement& p : placements) {
        if (p.reflected) {
            // Refuse loudly. Fusion would reject the matrix anyway, and a
            // silent drop would leave the part in the wrong place.
            result.warnings.push_back(
                "item " + std::to_string(p.item_id) +
                " is reflected; Fusion move features require a rigid transform. "
                "Expressing it as a 180-degree rotation about the mirror axis would "
                "flip the part over, so it is skipped rather than guessed.");
            ++result.skipped;
            continue;
        }

        if (p.item_id >= profiles.size()) {
            result.warnings.push_back("placement references unknown item " +
                                      std::to_string(p.item_id));
            ++result.skipped;
            continue;
        }

        const Profile& profile = profiles[static_cast<std::size_t>(p.item_id)];
        Ptr<BRepBody> body = profile.source ? profile.source->cast<BRepBody>() : nullptr;
        if (!body) {
            result.warnings.push_back(profile.name + ": source body is gone");
            ++result.skipped;
            continue;
        }

        Ptr<Matrix3D> m;
        {
            ScopedTime _t("writeback.matrix");
            m = placementMatrix(p);
        }
        if (!m) {
            result.warnings.push_back(profile.name + ": could not build transform");
            ++result.skipped;
            continue;
        }

        Ptr<ObjectCollection> entities;
        {
            ScopedTime _t("writeback.collection");
            entities = ObjectCollection::create();
            entities->add(body);
        }

        Ptr<MoveFeatureInput> input;
        bool defined = false;
        {
            ScopedTime _t("writeback.createInput2");
            input = moves->createInput2(entities);
        }
        if (input) {
            ScopedTime _t("writeback.defineAsFreeMove");
            defined = input->defineAsFreeMove(m);
        }
        if (!input || !defined) {
            std::string err;
            app->getLastError(&err);
            result.warnings.push_back(profile.name + ": defineAsFreeMove failed (" + err + ")");
            ++result.skipped;
            continue;
        }

        bool added = false;
        {
            // The prime suspect: in a parametric design every move feature is a
            // timeline entry, and adding one can recompute.
            ScopedTime _t("writeback.movesAdd");
            added = moves->add(input);
        }
        if (!added) {
            std::string err;
            app->getLastError(&err);
            result.warnings.push_back(profile.name + ": move failed (" + err + ")");
            ++result.skipped;
            continue;
        }
        ++result.moved;
    }

    // One collapsible timeline entry instead of N. Only meaningful in a
    // parametric design; direct modelling has no timeline at all.
    {
    ScopedTime _group("writeback.timelineGroup");
    if (groupTimeline && timeline && result.moved > 1) {
        const int markerAfter = timeline->markerPosition();
        if (markerAfter - 1 > markerBefore) {
            Ptr<TimelineGroups> groups = timeline->timelineGroups();
            if (groups && groups->add(markerBefore, markerAfter - 1)) {
                result.grouped = true;
            } else {
                result.warnings.push_back("could not group the move features in the timeline");
            }
        }
    }
    }

    std::ostringstream os;
    os << "write-back: moved " << result.moved << ", skipped " << result.skipped
       << (result.grouped ? ", timeline grouped" : "");
    logLine(os.str());
    return result;
}

}  // namespace fusenest
