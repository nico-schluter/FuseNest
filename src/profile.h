// Turning Fusion geometry into flat 2D profiles.
//
// The hard constraint comes from the far end: a sparrow/jagua-rs *item* must be
// a single simple polygon. `import_item` rejects holes outright
// ("item N has unsupported holes") and multipolygons ("No support for
// multipolygons yet"). Containers may have holes -- items may not.
//
// So holes are captured here but dropped by the adapter that builds solver
// input. They are still worth having: honest area reporting, preview drawing,
// and eventually container keep-out zones.
//
// The second constraint comes from the near end: write-back moves ONE THING per
// profile, so a selection must yield exactly one outline. Emitting several and
// letting them nest apart produces one placement per region against the same
// thing, and those compose -- two opposed placements cancel to almost no
// movement at all. Anything that cannot be reduced to one outline is therefore
// refused, not split.
//
// WHAT MOVES. Two kinds of unit come out of a selection, and which one it is
// decides how the placement is applied:
//
//   a body in the root component  -> the body moves, by a move feature
//   a component instance          -> the instance moves, by its own transform
//
// The split is forced by the same Fusion rule, read twice. A move feature acts
// on the NATIVE body, so moving a body inside a component moves it in every
// instance of that component -- which is never what a layout asked for. The
// instance is the only unit that can carry a placement there, so a body picked
// inside one resolves to its instance rather than to itself. Picking the
// instance as well as its body is therefore the same unit twice, and is
// deduplicated rather than nested twice.
//
// An instance is only nestable when its silhouette is the whole of it and it
// can move on its own: exactly one solid body inside it, nothing nested in it,
// and nothing holding it in place. The first two are refusals (`collectUnits`);
// the third is broken on the way out, in write-back.
#pragma once

#include "point2.h"

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

/// One nestable outline. `outer` is closed implicitly -- the first point is not
/// repeated at the end, which is what jagua-rs expects (it strips a repeated
/// closing vertex anyway).
struct Profile {
    std::string name;
    /// The BRepBody this outline was projected from, held as Base to keep the
    /// Fusion modelling headers out of this one. Cast at the point of use.
    ///
    /// For an instance this is the body PROXY inside it, so the outline is in
    /// root space either way -- and it is the body the preview hides, since an
    /// instance with one body in it has nothing else to hide.
    adsk::core::Ptr<adsk::core::Base> source;
    /// The Occurrence to move, or null to move `source` itself. Non-null IS
    /// what makes this a component placement: see WHAT MOVES above.
    adsk::core::Ptr<adsk::core::Base> instance;
    std::vector<Point2> outer;
    std::vector<std::vector<Point2>> holes;
    double areaMm2 = 0.0;          ///< true material area, holes excluded
    bool silhouetteApproximated = false;
};

struct ExtractOptions {
    /// Chord tolerance for turning curves into line segments. Lines and arcs
    /// come out exact or near-exact; splines -- lettering -- are approximated
    /// twice over, once in the silhouette and once here.
    double toleranceMm = 0.05;
};

/// What is wrong with a selection, coarse enough to say in a sentence.
///
/// The kind exists for the setup pane, which has room for names and not for
/// mechanism: it groups the issues by kind and writes one sentence per group,
/// so five unnestable bodies and one nested component read as two lines rather
/// than six. The fine-grained `reason` is still what the log and the refusal
/// print, so nothing is lost -- only deferred to where there is room for it.
///
/// Declaration order is the order the pane lists them in, and the blocking
/// kinds come first so the thing that greys out OK is the thing at the top.
enum class IssueKind {
    /// The outline is not one rigid piece: separate regions, or a pinch.
    Geometry,
    /// An instance inside another instance, whose placement would not hold.
    NotInRoot,
    /// An instance with sub-components, whose geometry is outside its outline.
    HasChildren,
    /// An instance with no solid body in it to project.
    NoBody,
    /// An instance holding several bodies, which one placement cannot carry.
    ManyBodies,
    /// ADVISORY: grounds, joints or rigid groups that write-back will delete
    /// to make room for the placement. Not a refusal -- the nest is correct
    /// either way -- but it changes the user's model, so it is said in advance.
    Constraints,
};

/// False for the kinds that only warn. Kept here rather than at the call site
/// so the dialog's OK gating and the pane's colouring cannot drift apart.
bool blocks(IssueKind kind);

/// One thing wrong with one selection.
///
/// Name and reason are kept apart rather than pre-joined because the two
/// consumers phrase them differently: the setup dialog leads with the names so
/// the user can see at a glance which selection to undo, while the log and the
/// refusal want whole sentences. `reason` is a fragment that reads correctly
/// after the name and a colon, with no leading capital and no trailing period.
struct NestIssue {
    std::string name;
    std::string reason;
    IssueKind kind = IssueKind::Geometry;
};

/// True when at least one issue in the list stops the nest from running.
bool anyBlocking(const std::vector<NestIssue>& issues);

struct ExtractResult {
    std::vector<Profile> profiles;
    std::vector<std::string> warnings;
    /// Why the job must not run, one entry per offending body.
    /// Kept apart from `warnings` because these decide control flow rather than
    /// describe it: nesting the rest would leave something the user explicitly
    /// selected out of the layout without saying so.
    std::vector<NestIssue> blockers;
    /// Nestable units the selection came to, after instances and bodies have
    /// been resolved and the duplicates between them dropped.
    int unitsExamined = 0;
    int holesDropped = 0;          ///< holes found; none of them reach the solver
};

/// Projects each selected unit onto the root XY plane and reads its outline.
///
/// Accepts BRepBody and Occurrence selections, in any mixture, and resolves
/// both to the units described above -- so a body inside a component and that
/// component are one unit, listed once.
ExtractResult extractProfiles(const adsk::core::Ptr<adsk::core::Application>& app,
                              const std::vector<adsk::core::Ptr<adsk::core::Base>>& selections,
                              const ExtractOptions& options);

/// Applies the same nestability rules as `extractProfiles` without reading any
/// outlines, so the setup dialog can say no before the user commits.
///
/// Returns advisory issues as well as refusals -- a constrained instance nests
/// fine, but applying the layout deletes what holds it, and the dialog is the
/// last place that can be said before it happens. Callers gate OK on
/// `anyBlocking`, not on emptiness.
///
/// It still has to project each unit -- a pinched silhouette is only visible in
/// the projection, and the body's own lump count cannot reveal it -- but it
/// stops at the face count instead of walking loops, tessellating curves and
/// building rings. Both paths classify and decide through the same code, so a
/// selection the dialog accepts cannot be refused by extraction afterwards.
std::vector<NestIssue> validateSelections(
    const adsk::core::Ptr<adsk::core::Application>& app,
    const std::vector<adsk::core::Ptr<adsk::core::Base>>& selections,
    const ExtractOptions& options = {});

}  // namespace fusenest
