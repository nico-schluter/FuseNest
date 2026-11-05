#include "preview.h"

#include "timing.h"
#include "triangulate.h"
#include "writeback.h"

#include <Fusion/FusionAll.h>

#include <algorithm>

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

/// Triangles per PROFILE INDEX, outliving the geometry they were built for.
///
/// Measured, not assumed: with the graphics group created inside the command
/// transaction, Fusion rolls it back between passes, so previewBuild runs once
/// per preview (log line "previews 13 (13 applied), builds 13"). Rebuilding the
/// Fusion entities is unavoidable; re-running ear clipping for an unchanged
/// outline is not. Triangulation is pure and deterministic, so the answer keeps.
std::vector<Triangulation> g_triCache;

/// What g_triCache was built from. Compared rather than trusted, because the
/// cache has to survive a rebuild but must NOT survive a new set of profiles.
struct CacheKey {
    const void* source = nullptr;
    std::size_t outer = 0;
    std::size_t holes = 0;
    bool operator!=(const CacheKey& o) const {
        return source != o.source || outer != o.outer || holes != o.holes;
    }
};
std::vector<CacheKey> g_triKey;

Ptr<Component> rootOf(const Ptr<Application>& app) {
    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    return design ? design->rootComponent() : nullptr;
}

/// Outline styling. Custom graphics default to a thin dark line, which is
/// close to invisible against Fusion's dark viewport, which made the first
/// filled previews unreadable.
Ptr<CustomGraphicsColorEffect> outlineColor() {
    // Cached: the effect is immutable and shared by every entity in the group.
    static Ptr<CustomGraphicsColorEffect> effect;
    if (!effect) effect = CustomGraphicsSolidColorEffect::create(Color::create(255, 170, 40, 255));
    return effect;
}

/// Appends one closed ring to a flat coordinate array, returning its length.
///
/// Rings are implicitly closed -- the first point is not repeated -- so the
/// closing vertex is emitted here rather than carried in the Profile.
int appendRing(const std::vector<Point2>& ring, std::vector<double>& coords) {
    if (ring.size() < 3) return 0;
    for (const Point2& pt : ring) {
        // Custom graphics are in internal units; profiles are millimetres.
        coords.push_back(pt.x * kMmToCm);
        coords.push_back(pt.y * kMmToCm);
        coords.push_back(0.0);
    }
    coords.push_back(ring[0].x * kMmToCm);
    coords.push_back(ring[0].y * kMmToCm);
    coords.push_back(0.0);
    return static_cast<int>(ring.size()) + 1;
}

/// The silhouette the solver actually reasoned about, outer ring and holes
/// together, as one entity.
///
/// One entity and not one per ring, because previewShow assigns a transform
/// per slot: lineStripLengths lets a single addLines call carry every ring, so
/// a part with a dozen holes still costs exactly one matrix assignment per
/// redraw. Holes are drawn purely because they were already extracted and
/// dropping them made parts unrecognisable -- the solver still never sees them.
Ptr<CustomGraphicsEntity> addOutlineLines(const Ptr<CustomGraphicsGroup>& group,
                                          const Profile& profile) {
    std::vector<double> coords;
    std::vector<int> stripLengths;
    std::size_t points = profile.outer.size() + 1;
    for (const std::vector<Point2>& hole : profile.holes) points += hole.size() + 1;
    coords.reserve(points * 3);

    if (int n = appendRing(profile.outer, coords)) stripLengths.push_back(n);
    for (const std::vector<Point2>& hole : profile.holes)
        if (int n = appendRing(hole, coords)) stripLengths.push_back(n);

    Ptr<CustomGraphicsCoordinates> c = CustomGraphicsCoordinates::create(coords);
    if (!c) return nullptr;

    // An empty index list means "use the coordinates in order", which is
    // exactly the order appendRing emitted them in.
    Ptr<CustomGraphicsLines> lines =
        group->addLines(c, std::vector<int>(), /*isLineStrip=*/true, stripLengths);
    if (!lines) return nullptr;

    lines->weight(2.0);
    lines->color(outlineColor());
    // Above the fill. Without this the two coincide on the Z=0 plane and the
    // edges z-fight with the faces they bound.
    lines->depthPriority(1);
    return lines;
}

/// Fill styling. Translucent, so overlapping parts are visible as overlaps --
/// which is the one thing a nesting preview must never hide.
Ptr<CustomGraphicsColorEffect> fillColor() {
    static Ptr<CustomGraphicsColorEffect> effect;
    if (!effect) {
        Ptr<Color> diffuse = Color::create(80, 140, 220, 255);
        effect = CustomGraphicsBasicMaterialColorEffect::create(
            diffuse, Color::create(40, 70, 110, 255), Color::create(20, 20, 20, 255),
            Color::create(0, 0, 0, 255), /*glossiness=*/5.0, /*opacity=*/0.65);
    }
    return effect;
}

/// The part as a filled surface.
///
/// Triangles are plain coordinates and indices -- no BRepBody, so nothing here
/// can put an ASM entity into the data model. That is the property that made
/// Outlines the answer to the undo crash, and filling the shape does not give
/// it up.
/// Fills g_triCache if these profiles are not the ones already in it.
void ensureTriangulated(const std::vector<Profile>& profiles) {
    std::vector<CacheKey> key(profiles.size());
    for (std::size_t i = 0; i < profiles.size(); ++i)
        key[i] = {profiles[i].source.get(), profiles[i].outer.size(), profiles[i].holes.size()};
    if (key.size() == g_triKey.size() &&
        std::equal(key.begin(), key.end(), g_triKey.begin(),
                   [](const CacheKey& a, const CacheKey& b) { return !(a != b); }))
        return;

    ScopedTime _t("preview.triangulate");
    g_triCache.clear();
    g_triCache.reserve(profiles.size());
    for (const Profile& p : profiles) g_triCache.push_back(triangulate(p.outer, p.holes));
    g_triKey.swap(key);
}

Ptr<CustomGraphicsEntity> addFillMesh(const Ptr<CustomGraphicsGroup>& group,
                                      const Profile& profile, std::size_t index) {
    if (index >= g_triCache.size()) return nullptr;
    const Triangulation& tri = g_triCache[index];
    if (tri.indices.empty()) return nullptr;

    std::vector<double> coords;
    coords.reserve(tri.vertices.size() * 3);
    for (const Point2& pt : tri.vertices) {
        coords.push_back(pt.x * kMmToCm);
        coords.push_back(pt.y * kMmToCm);
        coords.push_back(0.0);
    }
    Ptr<CustomGraphicsCoordinates> c = CustomGraphicsCoordinates::create(coords);
    if (!c) return nullptr;

    // One normal per vertex and an empty index list, which is the form addMesh
    // reads as "normals match the coordinates one-to-one". The profile is flat
    // on Z=0, so every normal is +Z.
    std::vector<double> normals;
    normals.reserve(tri.vertices.size() * 3);
    for (std::size_t i = 0; i < tri.vertices.size(); ++i) {
        normals.push_back(0.0);
        normals.push_back(0.0);
        normals.push_back(1.0);
    }

    Ptr<CustomGraphicsMesh> mesh = group->addMesh(c, tri.indices, normals, std::vector<int>());
    if (!mesh) return nullptr;
    mesh->color(fillColor());
    return mesh;
}

/// One part: a filled surface with its outline drawn on top.
///
/// Returns a SUBGROUP rather than either entity, because previewShow assigns
/// one transform per slot and a group is itself a transformable entity. Fill
/// and outline therefore move together at the cost of a single matrix
/// assignment -- the per-redraw cost this whole module is built around is
/// unchanged by filling.
Ptr<CustomGraphicsEntity> buildOutline(const Profile& profile, std::size_t index) {
    if (profile.outer.size() < 3) return nullptr;
    Ptr<CustomGraphicsGroup> part = g_group->addGroup();
    if (!part) return nullptr;

    // Fill is best-effort: a silhouette that will not triangulate still gets
    // its outline, which is strictly better than dropping the part.
    addFillMesh(part, profile, index);
    if (!addOutlineLines(part, profile)) {
        part->deleteMe();
        return nullptr;
    }
    return part;
}

bool build(const Ptr<Application>& app, const std::vector<Profile>& profiles,
           std::string& error) {
    previewClear();

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

    ensureTriangulated(profiles);

    g_entities.resize(profiles.size());
    int built = 0;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        g_entities[i] = buildOutline(profiles[i], i);
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

bool previewHasGeometry(const Ptr<Application>& app) {
    if (!g_group) return false;
    Ptr<Component> root = rootOf(app);
    Ptr<CustomGraphicsGroups> groups = root ? root->customGraphicsGroups() : nullptr;
    return groups && groups->count() > 0;
}

bool previewBuild(const Ptr<Application>& app, const std::vector<Profile>& profiles,
                  std::string& error) {
    ScopedTime _t("preview.build");
    return build(app, profiles, error);
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
    // Writes only where the state differs, which is not just hygiene: hiding
    // is 21ms of a 25ms preview on a hundred bodies, seven times the cost of
    // the drawing it serves. The rollback was measured to restore visibility
    // every pass, so the work is real every pass.
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
    // g_triCache is deliberately NOT cleared: it has to survive the per-pass
    // rebuild. Its key invalidates it when the profiles change.
}

}  // namespace fusenest
