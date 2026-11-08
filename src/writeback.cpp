#include "writeback.h"
#include "log.h"
#include "timing.h"
#include "nest_kernel.h"
#include "transform4.h"

#include <Fusion/FusionAll.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

constexpr double kMmToCm = 0.1;

Mat4 toMat4(const Ptr<Matrix3D>& m) {
    Mat4 out = identity4();
    const std::vector<double> cells = m ? m->asArray() : std::vector<double>{};
    if (cells.size() == 16) std::copy(cells.begin(), cells.end(), out.begin());
    return out;
}

Ptr<Matrix3D> toMatrix3D(const Mat4& m) {
    Ptr<Matrix3D> out = Matrix3D::create();
    if (!out) return nullptr;
    if (!out->setWithArray(std::vector<double>(m.begin(), m.end()))) return nullptr;
    return out;
}

/// Where a placement's move feature has to be created, and the matrix it needs
/// once it is there.
struct MoveTarget {
    /// The NATIVE body. A proxy cannot carry a move feature, and the feature
    /// belongs to the component that actually owns the geometry.
    Ptr<BRepBody> body;
    /// That component's, which is not necessarily the root's.
    Ptr<MoveFeatures> moves;
    /// `rootMatrix` as seen from inside that component.
    Ptr<Matrix3D> matrix;
    /// Non-empty when the placement cannot be applied; the caller reports it
    /// verbatim after the profile name.
    std::string error;
};

std::string describeMat4(const Mat4& m) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(4) << "[";
    for (int r = 0; r < 4; ++r) {
        if (r) os << " | ";
        for (int c = 0; c < 4; ++c) {
            if (c) os << " ";
            os << m[r * 4 + c];
        }
    }
    os << "]";
    return os.str();
}

std::string describeBox(const Ptr<BoundingBox3D>& box) {
    Ptr<Point3D> lo = box ? box->minPoint() : nullptr;
    Ptr<Point3D> hi = box ? box->maxPoint() : nullptr;
    if (!lo || !hi) return "(none)";

    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << "(" << lo->x() << ", " << lo->y() << ", "
       << lo->z() << ") .. (" << hi->x() << ", " << hi->y() << ", " << hi->z() << ")";
    return os.str();
}

/// Confirms that Matrix3D::asArray is laid out the way `toMat4` assumes.
///
/// Everything here reads a Matrix3D as 16 ROW-MAJOR cells, so a translation
/// lands at indices 3, 7 and 11. If Fusion ever handed back column-major cells
/// instead, a single conjugation would still come out right often enough to
/// look fine, while a composed chain would not -- so the assumption is checked
/// rather than trusted. Logged once per write-back; it costs one matrix.
void logMatrixLayout() {
    Ptr<Matrix3D> probe = Matrix3D::create();
    if (!probe || !probe->translation(Vector3D::create(1.0, 2.0, 3.0))) {
        logLine("writeback: matrix layout probe unavailable");
        return;
    }

    const Mat4 cells = toMat4(probe);
    const bool rowMajor = cells[3] == 1.0 && cells[7] == 2.0 && cells[11] == 3.0;
    const bool columnMajor = cells[12] == 1.0 && cells[13] == 2.0 && cells[14] == 3.0;
    logLine(std::string("writeback: matrix layout is ") +
            (rowMajor ? "row-major, as assumed"
                      : columnMajor ? "COLUMN-major -- toMat4 is wrong"
                                    : "neither row- nor column-major") +
            " " + describeMat4(cells));
}

/// The transform from a body's own component space into ROOT space.
///
/// `transform2` is documented as the occurrence's position "in its assembly
/// context", and for a nested occurrence that context is its PARENT occurrence,
/// not the root -- `assemblyContext()` returns exactly that parent, or null once
/// the occurrence is native. So one `transform2` is only the whole story at
/// depth one; deeper, the chain has to be composed. Reading just the leaf is
/// the bug this function exists to avoid, and it is invisible at depth one,
/// which is why it survived the first round of testing.
///
/// Walking leaf-first, each enclosing transform multiplies on the LEFT, since
/// an outer frame is applied after the inner one.
Mat4 occurrenceFrame(const Ptr<Occurrence>& leaf) {
    Mat4 frame = identity4();
    for (Ptr<Occurrence> occ = leaf; occ; occ = occ->assemblyContext())
        frame = multiply4(toMat4(occ->transform2()), frame);
    return frame;
}

/// Checks a frame against the model instead of trusting it.
///
/// `frame` claims to carry the native body onto the proxy the outline was
/// projected from, so the check is to do exactly that -- copy the native body,
/// transform the copy, and compare where it lands with where the proxy already
/// is. A composed-wrong frame is off by an enclosing occurrence's transform,
/// which is centimetres, so this separates right from wrong by a wide margin.
///
/// Bounding boxes are compared rather than vertices because they need no
/// assumption about topology ordering surviving the copy, and the copy is
/// transformed as GEOMETRY rather than as eight corner points -- the axis
/// aligned box of a rotated body is not the box of its rotated corners.
///
/// This is deliberately always on, not a verbose-only diagnostic. The whole
/// placement rests on a property of the Fusion API that the docs state in one
/// clause, and a misread there does not fail loudly -- it puts the part
/// somewhere plausible. Cheaper to measure it than to be sure.
bool frameAgreesWithGeometry(const Ptr<BRepBody>& native, const Ptr<BRepBody>& proxy,
                             const Ptr<Matrix3D>& frame, double& errorCm, std::string& detail) {
    errorCm = 0.0;

    Ptr<TemporaryBRepManager> mgr = TemporaryBRepManager::get();
    Ptr<BRepBody> copy = mgr ? mgr->copy(native) : nullptr;
    if (!copy || !mgr->transform(copy, frame)) {
        detail = "could not copy and transform the native body";
        return false;
    }

    Ptr<BoundingBox3D> moved = copy->boundingBox();
    Ptr<BoundingBox3D> expected = proxy ? proxy->boundingBox() : nullptr;
    // Three boxes, because which PAIR disagrees says what is wrong. Predicted
    // matching the proxy is the pass. Predicted matching the native's own box
    // instead means the proxy box came back in local space and the check, not
    // the frame, is at fault. Neither matching means the frame is wrong.
    detail = "native " + describeBox(native ? native->boundingBox() : nullptr) + ", predicted " +
             describeBox(moved) + ", proxy " + describeBox(expected);
    if (!moved || !expected) return false;

    const Ptr<Point3D> corners[4] = {moved->minPoint(), moved->maxPoint(), expected->minPoint(),
                                     expected->maxPoint()};
    for (const Ptr<Point3D>& c : corners)
        if (!c) return false;

    errorCm = std::max({std::fabs(corners[0]->x() - corners[2]->x()),
                        std::fabs(corners[0]->y() - corners[2]->y()),
                        std::fabs(corners[0]->z() - corners[2]->z()),
                        std::fabs(corners[1]->x() - corners[3]->x()),
                        std::fabs(corners[1]->y() - corners[3]->y()),
                        std::fabs(corners[1]->z() - corners[3]->z())});

    // One micron. Far below any real mismatch -- an enclosing transform left out
    // is centimetres -- and far above the noise between a BRep box and the box
    // of a transformed copy of the same BRep.
    return errorCm <= 1.0e-4;
}

/// Resolves a selected body -- which may be a proxy inside an occurrence -- to
/// the component that can carry the move, and re-expresses the placement there.
///
/// This is the whole reason a body inside a component used to land in the wrong
/// place. The outline was projected in ASSEMBLY context, so the solver's
/// placement is a motion in root space; a move feature, however, acts on the
/// native body and resolves its matrix in that body's OWN component. Handing
/// the root-space matrix over unchanged stacks it on top of the occurrence
/// transform. The conjugate T^-1 * M * T is the same rigid motion viewed from
/// inside the component, so the body ends up where the layout says.
///
/// A root-level body has no assembly context, its frame is the identity, and
/// conjugation leaves the matrix bit-for-bit alone -- the path that already
/// worked is not perturbed.
///
/// NOTE, since the conjugation is the interesting half and is now unreachable:
/// classification resolves every body inside a component to its INSTANCE, so a
/// body unit always arrives context-free and `frame` is always the identity.
/// The branch is kept because it is the general answer to "move this body,
/// wherever it lives", it is covered by transform4_test, and the moment a
/// deeper unit becomes nestable -- a body inside an instance that is itself
/// placed once, say -- it is the code that does it. It is not load-bearing
/// today, and a reader should not conclude from its presence that the
/// body-in-component path is live.
MoveTarget resolveMoveTarget(const Ptr<BRepBody>& selected, const Ptr<Matrix3D>& rootMatrix) {
    MoveTarget target;

    Ptr<Occurrence> context = selected ? selected->assemblyContext() : nullptr;
    target.body = context ? selected->nativeObject() : selected;
    if (!target.body) {
        target.error = "has no native body to move";
        return target;
    }

    Ptr<Component> owner = target.body->parentComponent();
    target.moves = owner && owner->features() ? owner->features()->moveFeatures() : nullptr;
    if (!target.moves) {
        target.error = "its component has no move features";
        return target;
    }

    if (!context) {
        target.matrix = rootMatrix;
        return target;
    }

    const Mat4 frame = occurrenceFrame(context);

    Ptr<Matrix3D> frameMatrix = toMatrix3D(frame);
    double errorCm = 0.0;
    std::string detail;
    if (!frameMatrix ||
        !frameAgreesWithGeometry(target.body, selected, frameMatrix, errorCm, detail)) {
        std::ostringstream os;
        os << "its position inside the assembly could not be confirmed (off by " << errorCm
           << " cm), so the placement is not applied";
        target.error = os.str();

        // Everything needed to tell a wrong frame from a wrong check, in one
        // run: the whole chain level by level, the frame it composed to, and
        // the three boxes.
        logLine("writeback: frame check FAILED for " + context->fullPathName() + "/" +
                selected->name() + ", error " + std::to_string(errorCm) + " cm");
        for (Ptr<Occurrence> occ = context; occ; occ = occ->assemblyContext())
            logLine("  level " + occ->fullPathName() + " transform2 " +
                    describeMat4(toMat4(occ->transform2())));
        logLine("  composed frame " + describeMat4(frame));
        logLine("  " + detail);
        return target;
    }

    Mat4 local{};
    if (!conjugate4(toMat4(rootMatrix), frame, local)) {
        // Only reachable for a non-rigid frame, which Fusion should not
        // produce. Refused rather than approximated: the transpose shortcut
        // would be silently wrong and the part would move somewhere nobody
        // asked for.
        target.error = "its occurrence transform is not rigid, so the placement cannot be "
                       "expressed inside the component";
        return target;
    }

    target.matrix = toMatrix3D(local);
    if (!target.matrix) target.error = "could not express the placement inside its component";
    return target;
}


/// Deletes every constraint in one of an occurrence's constraint lists.
///
/// Templated over the list because JointList, AsBuiltJointList and
/// RigidGroupList have the same shape and nothing else in common -- they share
/// no base that exposes count/item/deleteMe.
template <typename ListT>
int deleteConstraints(const Ptr<ListT>& list, const char* kind, const std::string& where,
                      std::vector<std::string>& warnings) {
    if (!list) return 0;

    // Copied out first: deleting from the design shortens the list under the
    // iteration, so indexing it while deleting skips every other entry.
    std::vector<decltype(list->item(0))> items;
    for (size_t i = 0; i < list->count(); ++i)
        if (auto item = list->item(i)) items.push_back(item);

    int gone = 0;
    for (auto& item : items) {
        const std::string name = item->name();
        // These lists come back as proxies when the occurrence is one. Our
        // instances are always top-level and therefore native, so this is a
        // belt-and-braces unwrap: deleteMe belongs to the native object.
        auto native = item->nativeObject();
        if ((native ? native : item)->deleteMe()) {
            ++gone;
            logLine("writeback: broke " + std::string(kind) + " " + name + " on " + where);
        } else {
            // The move is still attempted: Fusion may refuse it, which is
            // reported separately, or allow it and let the constraint pull the
            // part back later. Either way the user has to know this one is
            // still there, which is what this says.
            warnings.push_back(where + ": the " + kind + " \"" + name +
                               "\" could not be broken and may pull it back out of the layout");
        }
    }
    return gone;
}

/// Breaks whatever is holding an instance where it is.
///
/// A placement is a position, and anything that fixes the instance's position
/// outranks it: a grounded instance will not move at all, and a joint pulls it
/// back the next time the design recomputes -- so the layout the user accepted
/// would quietly come apart afterwards, which is worse than refusing to nest.
/// Asking for a nest is a decision about where the parts go, so the constraint
/// is what gives way.
///
/// Deleted rather than suppressed. A suppressed joint is still in the timeline
/// and the next unsuppress snaps the part back out of the layout -- the same
/// failure, only later and further from the cause. Undo is what puts them back:
/// this runs inside the command's execute handler, so the constraints and the
/// moves are one step.
int releaseInstance(const Ptr<Occurrence>& occ, std::vector<std::string>& warnings) {
    if (!occ) return 0;
    const std::string where = occ->fullPathName();

    int broken = 0;
    // Grounding is the one that fails loudly -- transform2 simply refuses --
    // and the one with no timeline entry, so it is a flag rather than a delete.
    if (occ->isGrounded()) {
        if (occ->isGrounded(false)) {
            ++broken;
            logLine("writeback: ungrounded " + where);
        } else {
            warnings.push_back(where + " is grounded and could not be released");
        }
    }
    if (occ->isGroundToParent()) {
        if (occ->isGroundToParent(false)) {
            ++broken;
            logLine("writeback: released ground-to-parent on " + where);
        } else {
            warnings.push_back(where + " is grounded to its parent and could not be released");
        }
    }

    // joints() is documented as the joints that affect THIS occurrence's
    // position, which is exactly the set that would fight the placement -- no
    // scan of the design and no guessing at which end of a joint we are.
    broken += deleteConstraints(occ->joints(), "joint", where, warnings);
    broken += deleteConstraints(occ->asBuiltJoints(), "as-built joint", where, warnings);
    broken += deleteConstraints(occ->rigidGroups(), "rigid group", where, warnings);
    return broken;
}

/// Moves one component instance by re-seating its own transform.
///
/// Not a move feature, unlike the body path: createInput2 is documented to take
/// "BRepBody or BRepFace objects", so an occurrence cannot go into one. An
/// assembly position lives in the occurrence's transform instead. A parametric
/// design treats a change to it as a PENDING transform, which is why
/// applyPlacements commits one snapshot at the end -- without that the position
/// is not recorded and the next recompute undoes the whole layout.
///
/// `transform2` maps the component's own space into root space and the
/// placement is a motion in root space, so the motion goes on the LEFT: the new
/// transform is placement * current. This is also why the instance path needs
/// no conjugation -- the placement is already in the space the transform lands
/// in, which is the thing the body path has to work for.
bool moveInstance(const Ptr<Occurrence>& occ, const Ptr<Matrix3D>& rootMatrix,
                  std::string& error) {
    Ptr<Matrix3D> current = occ ? occ->transform2() : nullptr;
    if (!current) {
        error = "has no transform to move";
        return false;
    }

    Ptr<Matrix3D> next = toMatrix3D(multiply4(toMat4(rootMatrix), toMat4(current)));
    if (!next) {
        error = "could not compose its new position";
        return false;
    }
    if (!occ->transform2(next)) {
        error = "Fusion refused to move the instance; something is still holding it in place";
        return false;
    }
    return true;
}

/// The instance a profile moves, or null when the profile moves its own body.
/// Profile holds both as Base to keep the Fusion modelling headers out of
/// profile.h, so the cast happens here.
Ptr<Occurrence> instanceOf(const Profile& profile) {
    return profile.instance ? profile.instance->cast<Occurrence>() : nullptr;
}

}  // namespace

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

    // No moveFeatures lookup here: each body resolves its own, because a body
    // inside a component is moved by THAT component's feature collection, not
    // the root's. The timeline is still the root design's -- there is only one,
    // whichever component a feature lands in.
    const bool parametric = design->designType() == ParametricDesignType;
    Ptr<Timeline> timeline = parametric ? design->timeline() : nullptr;
    const int markerBefore = timeline ? timeline->markerPosition() : 0;

    if (placements.empty()) {
        result.warnings.push_back("no placements to apply");
        return result;
    }

    logMatrixLayout();

    // Constraints first, for every instance in the layout, before anything
    // moves. Two reasons it is its own pass: one joint can hold two of the
    // instances being nested, so releasing as we go would read a transform that
    // a later release is about to change; and a joint deletion recomputes, so
    // doing them together costs one recompute rather than one per part.
    for (const NkPlacement& p : placements) {
        // A placement that is going to be refused below must not cost the user
        // a joint on the way: nothing is nested, so nothing had to be released.
        if (p.reflected || p.item_id >= profiles.size()) continue;
        if (Ptr<Occurrence> occ = instanceOf(profiles[static_cast<std::size_t>(p.item_id)]))
            result.constraintsBroken += releaseInstance(occ, result.warnings);
    }

    bool instancesMoved = false;

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
        Ptr<Occurrence> instance = instanceOf(profile);
        Ptr<BRepBody> body = profile.source ? profile.source->cast<BRepBody>() : nullptr;
        if (!instance && !body) {
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

        if (instance) {
            ScopedTime _t("writeback.moveInstance");
            std::string error;
            if (!moveInstance(instance, m, error)) {
                result.warnings.push_back(profile.name + ": " + error);
                ++result.skipped;
                continue;
            }
            instancesMoved = true;
            ++result.moved;
            continue;
        }

        MoveTarget target;
        {
            ScopedTime _t("writeback.resolveTarget");
            target = resolveMoveTarget(body, m);
        }
        if (!target.error.empty()) {
            result.warnings.push_back(profile.name + ": " + target.error);
            ++result.skipped;
            continue;
        }

        Ptr<ObjectCollection> entities;
        {
            ScopedTime _t("writeback.collection");
            entities = ObjectCollection::create();
            entities->add(target.body);
        }

        Ptr<MoveFeatureInput> input;
        bool defined = false;
        {
            ScopedTime _t("writeback.createInput2");
            input = target.moves->createInput2(entities);
        }
        if (input) {
            ScopedTime _t("writeback.defineAsFreeMove");
            defined = input->defineAsFreeMove(target.matrix);
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
            added = target.moves->add(input);
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

    // Instance moves are pending until they are snapshot: a parametric design
    // holds a changed occurrence transform as an uncommitted position, and the
    // next recompute reverts it. One snapshot covers every instance in the
    // layout, so this is the whole cost of the component path in the timeline.
    //
    // Direct modelling has no snapshots and needs none -- the transform is the
    // model there -- so hasPendingSnapshot is what decides, not our own flag.
    if (instancesMoved && parametric) {
        ScopedTime _snap("writeback.snapshot");
        Ptr<Snapshots> snapshots = design->snapshots();
        if (snapshots && snapshots->hasPendingSnapshot()) {
            if (snapshots->add()) {
                logLine("write-back: captured the component positions in a snapshot");
            } else {
                std::string err;
                app->getLastError(&err);
                result.warnings.push_back(
                    "the component moves could not be captured in the timeline (" + err +
                    "), so they will revert on the next recompute");
            }
        }
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
    os << "write-back: moved " << result.moved << ", skipped " << result.skipped;
    if (result.constraintsBroken) os << ", broke " << result.constraintsBroken << " constraints";
    os << (result.grouped ? ", timeline grouped" : "");
    logLine(os.str());
    return result;
}

}  // namespace fusenest
