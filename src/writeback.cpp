#include "writeback.h"
#include "log.h"
#include "timing.h"
#include "nest_kernel.h"
#include "transform4.h"

#include <Fusion/FusionAll.h>

#include <algorithm>
#include <cmath>
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
    /// The body to move. Native, necessarily: a proxy cannot carry a move
    /// feature, and the feature belongs to the component owning the geometry.
    Ptr<BRepBody> body;
    /// That component's collection, which is the root's for every unit that
    /// reaches here today.
    Ptr<MoveFeatures> moves;
    /// The placement, in the space the move feature will resolve it in.
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

/// Confirms that Matrix3D::asArray is laid out the way `toMat4` assumes.
///
/// Everything here reads a Matrix3D as 16 ROW-MAJOR cells, so a translation
/// lands at indices 3, 7 and 11. Read the wrong way round, an instance's
/// re-seated transform comes out transposed in its rotation and with the
/// translation in the wrong cells -- which is a rigid transform too, so the
/// part lands somewhere plausible rather than failing. Hence the assumption is
/// checked rather than trusted. Logged once per write-back; it costs one
/// matrix.
void logMatrixLayout() {
    Ptr<Matrix3D> probe = Matrix3D::create();
    if (!probe || !probe->translation(Vector3D::create(1.0, 2.0, 3.0))) {
        logDiag("writeback: matrix layout probe unavailable");
        return;
    }

    const Mat4 cells = toMat4(probe);
    const bool rowMajor = cells[3] == 1.0 && cells[7] == 2.0 && cells[11] == 3.0;
    const bool columnMajor = cells[12] == 1.0 && cells[13] == 2.0 && cells[14] == 3.0;
    logDiag(std::string("writeback: matrix layout is ") +
            (rowMajor ? "row-major, as assumed"
                      : columnMajor ? "COLUMN-major -- toMat4 is wrong"
                                    : "neither row- nor column-major") +
            " " + describeMat4(cells));
}

/// Finds the component that can carry the move for a root body.
///
/// Thin, and deliberately so: a body unit is always native to the component
/// being modelled, so the placement is already in the space the move feature
/// resolves its matrix in and goes through untouched. What this used to do
/// instead is worth knowing, because the shape of the problem has not changed
/// -- only who solves it. A body picked inside a component projected in
/// ASSEMBLY context, so the placement was a motion in root space, while the
/// move feature acted on the native body and resolved its matrix in that
/// body's OWN component; handing the root-space matrix over unchanged stacked
/// it on top of the occurrence transform and put the part somewhere plausible
/// and wrong. The conjugate T^-1 * M * T fixed it. Resolving the body to its
/// instance removes the question: the instance IS in root space.
///
/// So the assembly case is refused here rather than handled. It is unreachable
/// -- classification sends every body inside a component to its instance -- and
/// a refusal is the right residue of an invariant: if one ever did arrive, the
/// move would silently drag every other instance of that component along with
/// it, which is the whole reason the instance unit exists.
MoveTarget resolveMoveTarget(const Ptr<BRepBody>& selected, const Ptr<Matrix3D>& rootMatrix) {
    MoveTarget target;

    if (!selected) {
        target.error = "has no body to move";
        return target;
    }
    if (selected->assemblyContext()) {
        target.error = "is inside a component, so moving it would move it in every instance; "
                       "its instance should have been nested instead";
        return target;
    }

    Ptr<Component> owner = selected->parentComponent();
    target.moves = owner && owner->features() ? owner->features()->moveFeatures() : nullptr;
    if (!target.moves) {
        target.error = "its component has no move features";
        return target;
    }

    target.body = selected;
    target.matrix = rootMatrix;
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
///
/// `confirm` re-reads the transform and checks that the occurrence took it. Only
/// for a DIRECT design: there the transform is the model, so what comes back is
/// what the instance now carries. In a parametric design the write is a pending
/// position until the snapshot at the end of applyPlacements, so a read-back
/// would be asking a question the design has not yet answered.
bool moveInstance(const Ptr<Occurrence>& occ, const Ptr<Matrix3D>& rootMatrix, bool confirm,
                  std::string& error) {
    Ptr<Matrix3D> current = occ ? occ->transform2() : nullptr;
    if (!current) {
        error = "has no transform to move";
        return false;
    }

    const Mat4 wanted = multiply4(toMat4(rootMatrix), toMat4(current));
    Ptr<Matrix3D> next = toMatrix3D(wanted);
    if (!next) {
        error = "could not compose its new position";
        return false;
    }
    if (!occ->transform2(next)) {
        error = "Fusion refused to move the instance; something is still holding it in place";
        return false;
    }

    if (confirm) {
        Ptr<Matrix3D> seated = occ->transform2();
        // 1e-6 cm is ten nanometres: far below anything a layout cares about,
        // and far above the rounding a matrix survives on its way through
        // Fusion. The question being asked is whether the position took at all.
        if (!seated || !approxEqual4(toMat4(seated), wanted, 1e-6)) {
            error = "Fusion accepted the new position but the instance did not take it; "
                    "something is still holding it in place";
            return false;
        }
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
            if (!moveInstance(instance, m, /*confirm=*/!parametric, error)) {
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

        Ptr<MoveFeature> feature;
        {
            // The prime suspect: in a parametric design every move feature is a
            // timeline entry, and adding one can recompute.
            ScopedTime _t("writeback.movesAdd");
            feature = target.moves->add(input);
        }
        // A null return only means failure in a PARAMETRIC design. `add` is
        // documented to return "nothing in the case where the feature is
        // non-parametric": direct modelling has no timeline to hold a feature,
        // so the move is applied to the geometry and there is no feature to hand
        // back. Reading that null as a failure is what made a direct-mode apply
        // report every part skipped -- "move failed ()", with an empty
        // getLastError because nothing had in fact gone wrong -- while the
        // bodies had moved. The success of the operation itself is established
        // before this point: createInput2 vetted the entities and
        // defineAsFreeMove vetted the matrix, and both are checked above.
        if (!feature && parametric) {
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
