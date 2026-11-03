#include "preview.h"

#include "log.h"
#include "timing.h"
#include "writeback.h"

#include <Fusion/FusionAll.h>

#include <sstream>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

constexpr double kMmToCm = 0.1;

Ptr<CustomGraphicsGroup> g_group;

/// One drawable per PROFILE INDEX, because a placement's item_id is that index.
/// A gap is left as a null rather than compacted, so the indexing survives a
/// profile that could not be drawn.
std::vector<Ptr<CustomGraphicsEntity>> g_entities;

Ptr<Component> rootOf(const Ptr<Application>& app) {
    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    return design ? design->rootComponent() : nullptr;
}

/// A copy of the real body, so the preview looks exactly like the result.
Ptr<CustomGraphicsEntity> buildTempBody(const Ptr<TemporaryBRepManager>& tbm,
                                        const Profile& profile) {
    Ptr<BRepBody> src = profile.source ? profile.source->cast<BRepBody>() : nullptr;
    if (!src || !tbm) return nullptr;
    // A temporary body belongs to no component and is never in the timeline,
    // which is the entire saving over a move feature.
    Ptr<BRepBody> temp = tbm->copy(src);
    return temp ? g_group->addBRepBody(temp) : nullptr;
}

/// The silhouette the solver actually reasoned about, as a closed line strip.
Ptr<CustomGraphicsEntity> buildOutline(const Profile& profile) {
    if (profile.outer.size() < 3) return nullptr;

    std::vector<double> coords;
    coords.reserve(profile.outer.size() * 3);
    for (const Point2& pt : profile.outer) {
        // Custom graphics are in internal units; profiles are millimetres.
        coords.push_back(pt.x * kMmToCm);
        coords.push_back(pt.y * kMmToCm);
        coords.push_back(0.0);
    }

    Ptr<CustomGraphicsCoordinates> c = CustomGraphicsCoordinates::create(coords);
    if (!c) return nullptr;

    // Outlines are implicitly closed -- the first point is not repeated -- so
    // the strip has to return to index 0 by hand.
    std::vector<int> index;
    index.reserve(profile.outer.size() + 1);
    for (std::size_t i = 0; i < profile.outer.size(); ++i) index.push_back(static_cast<int>(i));
    index.push_back(0);

    return g_group->addLines(c, index, /*isLineStrip=*/true);
}

/// Shared by the real build and the benchmark.
bool build(const Ptr<Application>& app, const std::vector<Profile>& profiles, PreviewMode mode,
           std::string& error) {
    previewClear();
    if (mode == PreviewMode::MoveFeatures) return true;

    Ptr<Component> root = rootOf(app);
    if (!root) {
        error = "no active design";
        return false;
    }
    Ptr<CustomGraphicsGroups> groups = root->customGraphicsGroups();
    g_group = groups ? groups->add() : nullptr;
    if (!g_group) {
        error = "could not create a custom graphics group";
        return false;
    }

    Ptr<TemporaryBRepManager> tbm =
        mode == PreviewMode::TempBodies ? TemporaryBRepManager::get() : nullptr;

    g_entities.resize(profiles.size());
    int built = 0;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        g_entities[i] = mode == PreviewMode::TempBodies ? buildTempBody(tbm, profiles[i])
                                                        : buildOutline(profiles[i]);
        if (g_entities[i]) ++built;
    }

    if (built == 0) {
        error = "nothing could be drawn";
        previewClear();
        return false;
    }
    return true;
}

}  // namespace

const char* previewModeLabel(PreviewMode mode) {
    switch (mode) {
        case PreviewMode::MoveFeatures: return "Move features (exact, slow)";
        case PreviewMode::TempBodies: return "Temporary bodies (graphics)";
        default: return "Outlines (graphics)";
    }
}

bool previewBuild(const Ptr<Application>& app, const std::vector<Profile>& profiles,
                  PreviewMode mode, std::string& error) {
    ScopedTime _t("preview.build");
    return build(app, profiles, mode, error);
}

int previewShow(const std::vector<NkPlacement>& placements) {
    ScopedTime _t("preview.show");
    int placed = 0;
    for (const NkPlacement& p : placements) {
        if (p.item_id >= g_entities.size()) continue;
        const Ptr<CustomGraphicsEntity>& e = g_entities[static_cast<std::size_t>(p.item_id)];
        if (!e) continue;
        Ptr<Matrix3D> m = placementMatrix(p);
        if (m && e->transform(m)) ++placed;
    }
    return placed;
}

int previewHideSources(const std::vector<Profile>& profiles, bool hidden) {
    ScopedTime _t("preview.hideSources");
    // Writes only where the state differs. This is worth doing on its own --
    // hiding is 21ms of a 25ms preview on a hundred bodies, seven times the
    // cost of the drawing it serves -- but it also settles an assumption: this
    // is re-done every preview because Fusion's rollback is believed to make
    // the bodies visible again before each regeneration, which was never
    // actually checked. The returned count says so directly. A hundred every
    // time means the rollback does restore visibility and the work is real; a
    // hundred once and then zero means it persists and the hiding belongs
    // outside the preview entirely.
    int changed = 0;
    for (const Profile& profile : profiles) {
        Ptr<BRepBody> body = profile.source ? profile.source->cast<BRepBody>() : nullptr;
        if (!body) continue;
        if (body->isLightBulbOn() == !hidden) continue;
        body->isLightBulbOn(!hidden);
        ++changed;
    }
    return changed;
}

void previewClear() {
    if (g_group) g_group->deleteMe();
    g_group = nullptr;
    g_entities.clear();
}

void previewBenchmark(const Ptr<Application>& app, const std::vector<Profile>& profiles,
                      const std::vector<NkPlacement>& placements) {
    std::ostringstream os;
    os << "preview benchmark: " << profiles.size() << " profiles, " << placements.size()
       << " placements";

    // Move features are not re-run here. They are already measured every redraw
    // as writeback.movesAdd in this same session on this same job, which is a
    // fairer comparison than a one-off, and re-running them would modify the
    // model outside a preview transaction.
    for (PreviewMode mode : {PreviewMode::TempBodies, PreviewMode::Outlines}) {
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = build(app, profiles, mode, error);
        const double buildMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (!ok) {
            os << "\n  " << previewModeLabel(mode) << ": build failed -- " << error;
            continue;
        }

        // Three passes: the per-redraw number is what matters and one sample of
        // a sub-millisecond operation is noise.
        double showMs = 0.0;
        int placed = 0;
        for (int pass = 0; pass < 3; ++pass) {
            const auto s0 = std::chrono::steady_clock::now();
            placed = previewShow(placements);
            showMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0)
                    .count();
        }

        os << "\n  " << previewModeLabel(mode) << ": build " << static_cast<long>(buildMs)
           << " ms once, show " << (showMs / 3.0) << " ms per redraw (" << placed << " placed)";
        previewClear();
    }

    logLine(os.str());
}

}  // namespace fusenest
