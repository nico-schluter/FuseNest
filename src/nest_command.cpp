#include "nest_command.h"

#include "log.h"
#include "nest_request.h"
#include "preview.h"
#include "profile.h"
#include "solver.h"
#include "timing.h"
#include "writeback.h"
#include "solver_input.h"

#include <Fusion/FusionAll.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

const char* kCmdId = "FuseNestCommand";
// The result command: opens as soon as a solve starts and stays open for its
// duration, showing progress. OK applies the placements, Cancel discards them.
// It being a command is not incidental -- Fusion has no undo-transaction API,
// and a command's execute handler is the only thing it groups into one undo
// step, so the write-back has to happen in here.
const char* kResultCmdId = "FuseNestResultCommand";
const char* kPanelId = "SolidScriptsAddinsPanel";

// Fusion's internal length unit is centimetres, and ValueCommandInput::value()
// returns internal units regardless of what the dialog displays. Convert at the
// boundary and keep millimetres everywhere inside FuseNest.
constexpr double kCmToMm = 10.0;
constexpr double kMmToCm = 0.1;

// Input ids, in one place so the dialog and the execute handler cannot drift.
const char* kInParts = "fnParts";
const char* kInSheetHeight = "fnSheetHeight";
const char* kInSpacing = "fnSpacing";
const char* kInRotation = "fnRotation";
const char* kInBudget = "fnBudget";
const char* kInWorkers = "fnWorkers";
const char* kInVerbose = "fnVerbose";
const char* kInPreviewMode = "fnPreviewMode";
const char* kInBenchmark = "fnBenchmark";
const char* kInStatus = "fnStatus";
const char* kInJob = "fnJob";

Ptr<CommandDefinition> g_cmdDef;
Ptr<CommandDefinition> g_resultDef;

/// The profiles the last solve was built from. Placement item_ids index into
/// this, so it has to outlive the command that produced it.
std::vector<Profile> g_lastProfiles;

/// What command 1 found, so the result dialog can show it instead of the
/// message box that used to carry it.
std::string g_jobSummary;

/// Everything needed to rebuild the setup dialog as the user left it.
///
/// Bodies are held as entityTokens rather than Ptr<Base>. A pointer would still
/// be valid across the handoff, but a token is checkable: one that no longer
/// resolves means that body is genuinely gone, so a changed document is
/// reported rather than silently nesting fewer parts.
struct SavedSetup {
    bool valid = false;
    double sheetHeightMm = 0.0;
    double partSpacingMm = 0.0;
    int rotationIndex = 1;
    int timeBudgetSeconds = 10;
    int workers = 4;
    bool verboseLog = false;
    std::vector<std::string> partTokens;
};

SavedSetup g_saved;

/// A job built and ready, waiting for the engine to be free.
///
/// nk_cancel is graceful -- the kernel finishes its current cycle and returns
/// its best layout -- so a new job can in principle arrive before the old solve
/// has stopped, and solverStart would refuse it with "a nest is already
/// running".
///
/// In practice this never happens, and the measurement is worth recording so
/// nobody chases it again. Across thirteen round trips on a hundred bodies the
/// drain took 52-230ms while the fastest human round trip through the setup
/// dialog was 535ms: the gap cannot be closed by clicking faster. Profile
/// extraction, which was the suspected reason, is only 35-67ms and far too
/// quick to be covering it.
///
/// It stays as a correctness guard rather than a performance one. A long
/// separation on a much larger instance polls the cancel flag less often, and
/// the failure it prevents -- an error dialog on the path built for fast
/// iteration -- is worse than the cost of a branch that is almost never taken.
struct PendingJob {
    bool valid = false;
    std::string instanceJson;
    int budgetSeconds = 0;
    int workers = 0;
    int verboseLevel = 3;
    std::vector<Profile> profiles;
    std::string summary;
    std::size_t itemCount = 0;
};

PendingJob g_pending;

/// Set when the setup dialog is being reopened because the user cancelled out
/// of the result dialog, as opposed to invoking FuseNest fresh.
///
/// The difference matters: settings are worth carrying into a fresh invocation,
/// but silently reselecting bodies someone did not pick would be startling.
bool g_restoreSelections = false;

/// The live result command, while one is open. A CustomEvent handler gets no
/// Command pointer and ui->activeCommand() yields only an id string, so the
/// only way to reach the open dialog is to stash it here on creation. Cleared
/// in the destroy handler: calling into a destroyed command is undefined.
Ptr<Command> g_resultCmd;

bool g_launchPending = false;
bool g_solveFinished = false;

/// How the live layout is drawn. Move features are exact and make Apply free,
/// but cost one timeline insert per body; the graphics modes cost a matrix
/// assignment per body and need an explicit write-back on Apply.
PreviewMode g_previewMode = PreviewMode::TempBodies;
bool g_benchmarkRequested = false;
bool g_benchmarkDone = false;

// Diagnostics for the one open question in this design: what actually makes
// Fusion re-run executePreview on a dialog whose inputs the user is not
// touching. Counted rather than logged per-event -- at a 150 ms tick a short
// solve produces a hundred of each.
long g_tickCount = 0;
long g_previewCount = 0;
long g_previewApplied = 0;

/// Bodies the last preview actually had to hide. See previewHideSources.
int g_hiddenPerPreview = -1;

/// The layout the preview is currently drawing, and where it came from. Held so
/// the preview and a later Apply use the same one even though the kernel keeps
/// publishing better ones underneath.
std::vector<NkPlacement> g_shown;
NkSnapshotInfo g_shownInfo{};

/// Generation last pushed to the preview. Previews are expensive -- a full
/// write-back of every body -- so the clock only advances when the kernel has
/// actually found something better.
std::uint64_t g_previewedGeneration = 0;

/// Floor on the interval between previews. The kernel improves a layout several
/// times a second early in a solve, and each preview moves every body in the
/// design; redrawing at that rate buys nothing a person can see.
constexpr double kMinPreviewIntervalMs = 400.0;
std::chrono::steady_clock::time_point g_lastPreviewAt{};

/// How much narrower the strip must get before a redraw is worth it.
///
/// Gating on part movement was the obvious idea and it does not work: measured
/// over a solve, the maximum distance a part moves between consecutive feasible
/// layouts has a median of 107mm and a p90 of 261mm, on a strip 62mm wide. Only
/// 1 layout in 29 was within a millimetre of its predecessor. Consecutive
/// layouts are not refinements of each other -- exploration deliberately swaps
/// large items and reshuffles -- so almost every one differs wildly while the
/// width barely moves.
///
/// Which means the churn is not redundant drawing. It is an honest animation of
/// a search that throws the layout in the air every cycle, and the only way to
/// make it legible is to show less of it. Width is what the user is waiting for,
/// so that is the gate: measured counts over one solve are 29 layouts published,
/// 22 / 18 / 14 / 11 redraws at 0.1 / 0.25 / 0.5 / 1% thresholds.
constexpr double kMinWidthImprovementPct = 0.5;

/// Redraws asked for, and layouts examined and found not worth drawing.
///
/// Kept rather than retired with the rest of the instrumentation: previews
/// against redraws is the ratio that caught the status readout quietly
/// repainting the whole design every tick, 349 previews for 5 wanted redraws.
/// It now sits near 1.5, and a regression would show up here first.
long g_toggles = 0;
long g_skippedRedraws = 0;

/// Set once a preview has claimed its result as the final one, so the execute
/// handler knows not to apply the same placements a second time.
bool g_previewClaimed = false;

/// Defined below, next to the rest of the preview plumbing; declared here
/// because the dialog's created handler needs it to catch up on a solve that
/// was already running before it opened.
void maybeAdvancePreview();

/// Reopens the setup dialog with the last job restored. Called from the result
/// dialog's destroy handler, so the same question applies as for the forward
/// direction: starting a command while Fusion tears down another. The forward
/// path proved the direct call works, so this uses it too and logs if it fails.
void reopenSetup() {
    g_restoreSelections = true;
    if (g_cmdDef && g_cmdDef->execute()) {
        logLine("setup dialog reopened for another pass");
        return;
    }
    std::string err;
    if (Ptr<Application> app = Application::get()) app->getLastError(&err);
    logLine("could not reopen the setup dialog: " + err);
    g_restoreSelections = false;
}

const char* phaseLabel(std::uint32_t phase) {
    switch (phase) {
        case 1: return "Exploring";
        case 2: return "Compressing";
        default: return "Idle";
    }
}

const char* terminationLabel(CommandTerminationReason why) {
    switch (why) {
        case CompletedTerminationReason: return "completed (OK)";
        case CancelledTerminationReason: return "cancelled";
        case AbortedTerminationReason: return "aborted (OK, execute failed)";
        case PreEmptedTerminationReason: return "pre-empted by another command";
        case SessionEndingTerminationReason: return "session ending";
        default: return "unknown";
    }
}

/// Whether there is a result worth applying. Until partial results stream out
/// of the kernel this is simply "the solve ran to completion".
bool haveApplicableResult() {
    return g_solveFinished && solverReturnCode() == 0 && !g_lastProfiles.empty();
}

std::string statusText() {
    NkProgress p{};
    const bool have = solverProgress(p);

    std::ostringstream os;
    if (g_pending.valid) {
        // Deliberately the only thing shown: the layout still on the kernel's
        // side belongs to the abandoned solve, not to this job.
        return "<b>Starting</b><br/>waiting for the cancelled nest to stop";
    }
    if (!g_solveFinished) {
        os << "<b>" << (have ? phaseLabel(p.phase) : "Starting") << "</b><br/>";
    } else if (solverReturnCode() != 0) {
        os << "<b>Failed</b> (rc " << solverReturnCode() << ")<br/>" << solverLastError();
        return os.str();
    } else {
        os << "<b>Finished</b> in " << static_cast<long>(solverElapsedMs()) << " ms<br/>";
    }

    // Numbers from the snapshot, not from progress: these must describe the
    // layout on screen. The separation count is search activity and belongs to
    // the progress side.
    if (!g_shown.empty()) {
        os << "density " << static_cast<int>(g_shownInfo.density * 100.0) << " % &middot; width "
           << static_cast<long>(g_shownInfo.strip_width) << " mm &middot; " << g_shown.size()
           << " parts placed";
        // The ordinal is not decoration. Writing this textbox is what triggers
        // the redraw, so the string has to differ every time -- and the rounded
        // width and density can easily repeat between two layouts that differ by
        // the half-percent the gate demands.
        if (!g_solveFinished)
            os << "<br/>improvement " << g_toggles << " &middot; best so far, Apply keeps it";
    } else if (!g_solveFinished) {
        os << "looking for a first feasible layout";
    }
    // Deliberately carries NOTHING that changes per tick. The separation count
    // used to be here, and it cost 349 executePreview events -- each a full
    // write-back of every body -- for 5 wanted redraws. Writing this textbox
    // fires inputChanged, which regenerates the preview, so anything in here
    // that moves on its own drives the viewport.
    //
    // (An earlier run concluded textbox writes fire no inputChanged. That was a
    // measurement artifact: the clock toggle happened later in the same tick, so
    // it always masked the textbox as the last-changed input.)
    return os.str();
}

/// The status text as last written, so an unchanged one is not written again.
std::string g_statusShown;

/// Pushes the current status into the open result dialog, if there is one.
/// Returns whether a dialog was there to update.
bool refreshResultDialog() {
    if (!g_resultCmd) return false;
    Ptr<CommandInputs> inputs = g_resultCmd->commandInputs();
    if (!inputs) return false;

    // Only on a real change. Every write to this input regenerates the preview,
    // so a redundant one costs a full write-back of the whole design.
    const std::string text = statusText();
    if (text != g_statusShown) {
        if (Ptr<TextBoxCommandInput> tb = inputs->itemById(kInStatus)) {
            tb->formattedText(text);
            g_statusShown = text;
        }
    }
    return true;
}

RotationMode rotationFromIndex(int index) {
    switch (index) {
        case 0: return RotationMode::None;
        case 1: return RotationMode::Half;
        case 2: return RotationMode::Quarter;
        default: return RotationMode::Continuous;
    }
}

/// Stashes the dialog exactly as the user left it, so cancelling out of the
/// result dialog can put it back rather than making them set it up again.
void rememberSetup(const Ptr<CommandInputs>& inputs, const NestRequest& req) {
    if (!inputs) return;

    g_saved = SavedSetup{};
    g_saved.sheetHeightMm = req.sheetHeightMm;
    g_saved.partSpacingMm = req.partSpacingMm;
    g_saved.timeBudgetSeconds = req.timeBudgetSeconds;
    g_saved.workers = req.workers;
    g_saved.verboseLog = req.verboseLog;
    if (Ptr<DropDownCommandInput> d = inputs->itemById(kInRotation))
        if (Ptr<ListItem> item = d->selectedItem()) g_saved.rotationIndex = item->index();

    for (const Ptr<Base>& e : req.parts) {
        // Tokens live on the concrete type, not on Base. Only solid bodies can
        // be selected, so this is the only cast needed.
        if (Ptr<BRepBody> body = e ? e->cast<BRepBody>() : nullptr) {
            const std::string token = body->entityToken();
            if (!token.empty()) g_saved.partTokens.push_back(token);
        }
    }
    g_saved.valid = true;
    logLine("remembered setup: " + std::to_string(g_saved.partTokens.size()) + " of " +
            std::to_string(req.parts.size()) + " parts tokenised");
}

NestRequest readInputs(const Ptr<CommandInputs>& inputs) {
    NestRequest req;
    if (!inputs) return req;

    if (Ptr<SelectionCommandInput> sel = inputs->itemById(kInParts)) {
        const size_t n = sel->selectionCount();
        for (size_t i = 0; i < n; ++i)
            if (Ptr<Selection> s = sel->selection(i))
                if (Ptr<Base> e = s->entity()) req.parts.push_back(e);
    }
    if (Ptr<ValueCommandInput> v = inputs->itemById(kInSheetHeight))
        req.sheetHeightMm = v->value() * kCmToMm;
    if (Ptr<ValueCommandInput> v = inputs->itemById(kInSpacing))
        req.partSpacingMm = v->value() * kCmToMm;
    if (Ptr<DropDownCommandInput> d = inputs->itemById(kInRotation))
        if (Ptr<ListItem> item = d->selectedItem()) req.rotation = rotationFromIndex(item->index());
    if (Ptr<IntegerSpinnerCommandInput> s = inputs->itemById(kInBudget))
        req.timeBudgetSeconds = s->value();
    if (Ptr<IntegerSpinnerCommandInput> s = inputs->itemById(kInWorkers))
        req.workers = s->value();
    if (Ptr<BoolValueCommandInput> b = inputs->itemById(kInVerbose))
        req.verboseLog = b->value();

    return req;
}

// --- the result command --------------------------------------------------

/// OK. Does the actual model change; running inside a command execute is what
/// makes the whole write-back a single undo step.
class ResultExecuteHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override {
        Ptr<Application> app = Application::get();
        Ptr<UserInterface> ui = app ? app->userInterface() : nullptr;

        if (g_previewClaimed) {
            // Fusion reused the preview's transforms, so there is nothing left
            // to do. If this line never appears, isValidResult did not take and
            // the write-back has to stay in execute.
            logLine("execute: preview already claimed the result, nothing to apply");
            return;
        }

        // The graphics modes leave the sources hidden and the preview unclaimed,
        // so OK lands here and has to put them back before moving them.
        previewHideSources(g_lastProfiles, false);

        const WriteBackResult wb = applyPlacements(app, g_lastProfiles, g_shown);
        logLine("applied: " + std::to_string(wb.moved) + " moved, " +
                std::to_string(wb.skipped) + " skipped");
        for (const auto& w : wb.warnings) logLine("writeback warning: " + w);

        if (!wb.warnings.empty() && ui) {
            std::ostringstream os;
            os << "Applied " << wb.moved << " of " << (wb.moved + wb.skipped) << " parts.\n\n";
            for (const auto& w : wb.warnings) os << "  - " << w << "\n";
            ui->messageBox(os.str(), "FuseNest");
        }
    }
};

/// Fires whenever Fusion decides the preview should be regenerated. Right now
/// it only counts itself: proving what triggers it is the point of this step.
class ResultPreviewHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& args) override {
        ScopedTime _t("preview.total");
        ++g_previewCount;

        // Draws the layout the tick captured, NOT a freshly fetched one. Fusion
        // rolls each preview back before regenerating, so this re-applies every
        // time; fetching here instead would mean the drawing changed under a
        // user who had not asked for anything.
        //
        // Timeline grouping is skipped: these moves are about to be discarded,
        // and grouping them costs a timeline operation per preview. The preview
        // that OK keeps is regrouped by the execute path.
        if (g_previewMode != PreviewMode::MoveFeatures) {
            // Re-hidden every time: visibility is a model change, so Fusion's
            // rollback makes the bodies visible again before each regeneration.
            g_hiddenPerPreview = previewHideSources(g_lastProfiles, true);
            if (previewShow(g_shown) > 0) ++g_previewApplied;
            // Custom graphics are not a model change, so there is nothing for
            // Fusion to keep. The execute handler does the real write-back.
            if (args) args->isValidResult(false);
            return;
        }

        const WriteBackResult wb =
            applyPlacements(Application::get(), g_lastProfiles, g_shown, /*groupTimeline=*/false);
        if (wb.moved > 0) {
            ++g_previewApplied;
            g_previewClaimed = true;
        }
        // isValidResult tells Fusion the preview is reusable: OK keeps these
        // transforms instead of rolling back and firing execute.
        if (args) args->isValidResult(wb.moved > 0);
    }
};

class ResultValidateHandler : public ValidateInputsEventHandler {
  public:
    void notify(const Ptr<ValidateInputsEventArgs>& args) override {
        // Validity gates the preview as well as the OK button: while this was
        // false for the duration of a solve, executePreview never fired once.
        // Both now rest on the same condition -- there is a feasible layout to
        // show and to accept. Which is the whole point of streaming: that
        // becomes true a fraction of a second into a solve rather than at the
        // end of one.
        if (args) args->areInputsValid(!g_shown.empty());
    }
};

class ResultDestroyHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& args) override {
        const CommandTerminationReason why =
            args ? args->terminationReason() : UnknownTerminationReason;
        logLine(std::string("result dialog closed: ") + terminationLabel(why) + " -- ticks " +
                std::to_string(g_tickCount) + ", redraws " + std::to_string(g_toggles) + ", " +
                std::to_string(g_skippedRedraws) + " skipped as invisible, previews " +
                std::to_string(g_previewCount) + " (" + std::to_string(g_previewApplied) +
                " applied)");

        // Unconditional: a dialog that is gone has nobody left to show progress
        // to, so the engine has no reason to keep burning cores. On OK the
        // solve is already over and this is a no-op.
        logLine(timingReport());

        // Custom graphics are not part of the command transaction and would
        // outlive a cancel.
        previewClear();

        solverCancel();
        g_resultCmd = nullptr;

        // Cancel means "not this layout", and the reason it is not this layout
        // is almost always the setup -- wrong spacing, wrong sheet, wrong
        // rotation. So Cancel goes back to setup rather than simply ending,
        // which is also what makes iterating cheap.
        //
        // Only on a real cancel. Pre-empted means the user reached for another
        // tool and would not thank us for a dialog; completed and session-ending
        // speak for themselves.
        if (why == CancelledTerminationReason && g_saved.valid) reopenSetup();
    }
};

class ResultCreatedHandler : public CommandCreatedEventHandler {
  public:
    void notify(const Ptr<CommandCreatedEventArgs>& args) override {
        Ptr<Command> cmd = args ? args->command() : nullptr;
        if (!cmd) {
            logLine("result commandCreated: command() was null");
            return;
        }

        cmd->okButtonText("Apply");
        cmd->cancelButtonText("Discard");
        cmd->isRepeatable(false);
        // Critical: if another command pre-empts this dialog, terminate rather
        // than silently executing. Moving the user's bodies because they
        // reached for a different tool would be indefensible.
        cmd->isExecutedWhenPreEmpted(false);

        Ptr<CommandInputs> inputs = cmd->commandInputs();
        if (!inputs) {
            logLine("result commandCreated: commandInputs() was null");
            return;
        }

        inputs->addTextBoxCommandInput(kInStatus, "", statusText(), 3, true);
        if (!g_jobSummary.empty())
            inputs->addTextBoxCommandInput(kInJob, "", g_jobSummary, 6, true);

        cmd->validateInputs()->add(&validate_);
        cmd->executePreview()->add(&preview_);
        cmd->execute()->add(&execute_);
        cmd->destroy()->add(&destroy_);

        if (g_previewMode != PreviewMode::MoveFeatures) {
            std::string error;
            if (!previewBuild(Application::get(), g_lastProfiles, g_previewMode, error)) {
                logLine("preview build failed (" + error + "); falling back to move features");
                g_previewMode = PreviewMode::MoveFeatures;
            }
        }

        g_resultCmd = cmd;
        g_tickCount = g_previewCount = 0;
        g_previewApplied = 0;
        g_previewClaimed = false;
        g_previewedGeneration = 0;
        g_lastPreviewAt = {};
        g_toggles = g_skippedRedraws = 0;
        timingReset();
        g_statusShown.clear();
        g_shown.clear();
        g_shownInfo = NkSnapshotInfo{};
        // A solve may already have published layouts before this dialog opened.
        // Only updates state here; the first tick's status write draws it.
        maybeAdvancePreview();
        logLine("result dialog open");
    }

  private:
    ResultValidateHandler validate_;
    ResultPreviewHandler preview_;
    ResultExecuteHandler execute_;
    ResultDestroyHandler destroy_;
};

ResultCreatedHandler g_resultCreatedHandler;

/// Opens the result dialog, one event-loop turn after command 1 has gone.

// --- engine callbacks ----------------------------------------------------

/// Starts a job and makes everything that describes a solve refer to it.
///
/// One place, because the pieces have to move together: the profiles a
/// placement's item_id indexes into, and the layout the preview is drawing. A
/// stale layout paired with fresh profiles would move the wrong bodies.
bool beginJob(const PendingJob& job, std::string& error) {
    ScopedTime _t("job.begin");
    g_lastProfiles = job.profiles;
    g_jobSummary = job.summary;

    g_shown.clear();
    g_shownInfo = NkSnapshotInfo{};
    g_previewedGeneration = 0;
    g_lastPreviewAt = {};
    g_statusShown.clear();
    g_solveFinished = false;

    solverSetLogLevel(job.verboseLevel);
    if (!solverStart(job.instanceJson, job.budgetSeconds, job.workers, error)) return false;

    logLine("solve started: " + std::to_string(job.itemCount) + " items, " +
            std::to_string(job.workers) + " workers, " + std::to_string(job.budgetSeconds) +
            " s ceiling");
    return true;
}

/// Picks up a newer layout from the kernel and asks Fusion to redraw, but only
/// when there is something better to show and enough time has passed to be
/// worth a full write-back.
///
/// Fusion regenerates a preview in response to an input VALUE changing. A
/// TextBoxCommandInput's formattedText is not one -- writing it fires no
/// inputChanged at all -- so the hidden clock input is what carries the request.
/// Whether redrawing `fresh` over what was last drawn is worth the cost.
///
/// Compared against the last DRAWN layout, not the last seen one, so a run of
/// individually small improvements still accumulates into a redraw.
bool worthRedrawing(const std::vector<NkPlacement>& shown, const NkSnapshotInfo& shownInfo,
                    const std::vector<NkPlacement>& fresh, const NkSnapshotInfo& freshInfo) {
    // A different number of placed parts is always worth showing, and is also
    // the only case where comparing widths would be misleading.
    if (shown.size() != fresh.size()) return true;
    if (shownInfo.strip_width <= 0.0f) return true;

    const double gainedPct =
        (shownInfo.strip_width - freshInfo.strip_width) / shownInfo.strip_width * 100.0;
    return gainedPct >= kMinWidthImprovementPct;
}

void maybeAdvancePreview() {
    if (!g_resultCmd) return;
    // The published layout belongs to the solve being abandoned, and the
    // profiles it indexes into have already been replaced.
    if (g_pending.valid) return;

    const std::uint64_t gen = solverSnapshotGeneration();
    if (gen == g_previewedGeneration) return;

    const auto now = std::chrono::steady_clock::now();
    const double sinceMs =
        std::chrono::duration<double, std::milli>(now - g_lastPreviewAt).count();
    // The final result always goes through: it is the one the user acts on, it
    // arrives exactly once, and the numbers beside it must match it exactly.
    const bool force = g_solveFinished;
    if (!force && sinceMs < kMinPreviewIntervalMs) return;

    NkSnapshotInfo info{};
    std::vector<NkPlacement> fresh;
    {
        ScopedTime _t("snapshot.copy");
        if (!solverSnapshot(fresh, info)) return;
    }

    // Examined, so do not examine it again -- but g_shown stays as the last
    // layout actually drawn, which is what the next comparison must be against.
    g_previewedGeneration = info.generation;

    if (!force && !g_shown.empty() && !worthRedrawing(g_shown, g_shownInfo, fresh, info)) {
        ++g_skippedRedraws;
        // Deliberately does NOT update g_shownInfo: it describes the layout on
        // screen, and the next comparison has to be against that layout's width
        // or a long run of sub-threshold gains would never add up to a redraw.
        // The progress line carries the sense of activity instead.
        return;
    }

    g_shown = std::move(fresh);
    g_shownInfo = info;
    g_lastPreviewAt = now;

    if (g_benchmarkRequested && !g_benchmarkDone) {
        g_benchmarkDone = true;
        previewBenchmark(Application::get(), g_lastProfiles, g_shown);
        // Rebuilt because the benchmark tore down its own geometry, which is
        // the same geometry the live preview was using.
        if (g_previewMode != PreviewMode::MoveFeatures) {
            std::string error;
            if (!previewBuild(Application::get(), g_lastProfiles, g_previewMode, error))
                logLine("preview rebuild after benchmark failed: " + error);
        }
    }

    ++g_toggles;
    // No separate trigger. Writing an input is what makes Fusion regenerate the
    // preview, and the status refresh that follows in the same tick is already
    // a write -- so one write does both jobs. Toggling a clock as well cost a
    // second preview round, measured at 1832ms against the status refresh's
    // 3679ms on a hundred bodies: two thirds of the freeze was the readout and
    // the remaining third was a widget whose only job was to change.
}

/// Main thread, every ~150 ms while a solve runs.
void onProgressTick() {
    ScopedTime _t("tick.total");
    ++g_tickCount;
    // Before the early return: the trace is worth having even with no dialog
    // open to show it to.
    {
        ScopedTime _t("tick.drainKernelLog");
        solverDrainKernelLog();
    }

    if (!g_resultCmd) return;

    // Redraw first, then describe: the status line reports the layout on
    // screen, so writing it before the redraw would caption the previous one.
    {
        ScopedTime _a("tick.maybeAdvancePreview");
        maybeAdvancePreview();
    }
    {
        ScopedTime _r("tick.refreshStatus");
        refreshResultDialog();
    }

    if (g_tickCount == 1 || g_tickCount % 40 == 0)
        logLine("tick " + std::to_string(g_tickCount) + ": gen " +
                std::to_string(g_shownInfo.generation) + ", redraws " +
                std::to_string(g_toggles) + " (" + std::to_string(g_skippedRedraws) +
                " skipped as invisible), previews " + std::to_string(g_previewCount) +
                ", bodies re-hidden by the last preview " + std::to_string(g_hiddenPerPreview) +
                "\n" + timingReport());
}

/// Main thread, once, however the solve ended.
void reportCompletion() {
    // A queued job means this completion belongs to a solve the user already
    // walked away from. Hand straight over instead of reporting a result nobody
    // asked for.
    if (g_pending.valid) {
        const PendingJob job = g_pending;
        g_pending = PendingJob{};
        solverDrainKernelLog();
        logLine("cancelled solve drained; starting the queued job");

        std::string error;
        if (!beginJob(job, error)) {
            logLine("queued job failed to start: " + error);
            // Otherwise the dialog sits claiming it is about to start something.
            g_solveFinished = true;
            if (Ptr<Application> app = Application::get())
                if (Ptr<UserInterface> ui = app->userInterface())
                    ui->messageBox("Could not start nesting:\n" + error, "FuseNest");
        }
        refreshResultDialog();
        return;
    }

    g_solveFinished = true;
    // The tail of the trace: the ticker has already stopped, so the lines from
    // the final cycle and the phase summaries are still sitting in the buffer.
    solverDrainKernelLog();

    const int rc = solverReturnCode();
    if (rc != 0) {
        logLine("solve failed (rc " + std::to_string(rc) + "): " + solverLastError());
    } else {
        const std::string json = solverResultJson();
        const std::string path = std::string(outputDir()) + "/fusenest_solution.json";
        {
            std::ofstream f(path);
            if (f) f << json;
        }
        logLine("solve complete in " + std::to_string(static_cast<long>(solverElapsedMs())) +
                " ms, generation " + std::to_string(solverSnapshotGeneration()));
    }

    // The final solution is published as a snapshot like any other, so the same
    // path picks it up -- there is no separate finishing step.
    maybeAdvancePreview();

    if (!refreshResultDialog()) {
        // Two different situations, and the trivial-solve case is not a problem:
        // the launch is deferred by one event-loop turn, so a solve that takes
        // less than that finishes first and the dialog reads the final state
        // from statusText() when it opens. g_launchPending still being set is
        // what tells the two apart.
        if (g_launchPending)
            logLine("solve finished before the result dialog opened -- it will show the result");
        else
            logLine("solve finished with no result dialog open -- result discarded");
    }
}

// --- the setup command ---------------------------------------------------
// Fusion stores a RAW POINTER to every handler. A stack-allocated handler dies
// when the creating function returns and Fusion then calls into freed memory,
// so all of these are file-scope statics with lifetimes longer than the events
// they are attached to. This has no Python or TypeScript equivalent.

class ExecuteHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& args) override {
        Ptr<Application> app = Application::get();
        if (!app) return;
        Ptr<UserInterface> ui = app->userInterface();

        Ptr<Command> cmd = args ? args->command() : nullptr;
        const NestRequest req = readInputs(cmd ? cmd->commandInputs() : nullptr);

        timingReset();
        logLine("execute:");
        logLine(req.describe());

        rememberSetup(cmd ? cmd->commandInputs() : nullptr, req);

        if (Ptr<CommandInputs> in = cmd ? cmd->commandInputs() : nullptr) {
            if (Ptr<DropDownCommandInput> d = in->itemById(kInPreviewMode))
                if (Ptr<ListItem> item = d->selectedItem())
                    g_previewMode = item->index() == 0   ? PreviewMode::TempBodies
                                    : item->index() == 1 ? PreviewMode::Outlines
                                                         : PreviewMode::MoveFeatures;
            if (Ptr<BoolValueCommandInput> b = in->itemById(kInBenchmark))
                g_benchmarkRequested = b->value();
        }
        g_benchmarkDone = false;
        logLine(std::string("preview mode: ") + previewModeLabel(g_previewMode));

        ExtractOptions opts;
        ExtractResult extracted;
        {
            // Unmeasured until now, and it is main-thread work on every body in
            // the job -- so it is both a possible freeze of its own and the
            // reason the engine may already be idle by the time we ask.
            ScopedTime _t("setup.extractProfiles");
            extracted = extractProfiles(app, req.parts, opts);
        }

        int approximated = 0;
        double totalAreaMm2 = 0.0;
        size_t totalPoints = 0;
        for (const auto& p : extracted.profiles) {
            if (p.silhouetteApproximated) ++approximated;
            totalAreaMm2 += p.areaMm2;
            totalPoints += p.outer.size();
        }

        g_lastProfiles = extracted.profiles;
        SolverInput input;
        {
            ScopedTime _t("setup.buildSolverInput");
            input = buildSolverInput(req, extracted.profiles, "fusenest");
        }
        const std::string instancePath = std::string(outputDir()) + "/fusenest_instance.json";
        {
            std::ofstream f(instancePath);
            if (f) f << input.json;
        }
        logLine("wrote solver input: " + instancePath + " (" + std::to_string(input.json.size()) +
                " bytes, " + std::to_string(input.itemCount) + " items)");

        // Everything command 1 learned, carried into the result dialog rather
        // than thrown at a message box the user has to dismiss before working.
        std::ostringstream job;
        job << "<b>" << input.itemCount << " parts</b> from " << extracted.bodiesExamined
            << " bodies &middot; " << totalPoints << " points<br/>"
            << static_cast<long>(totalAreaMm2) << " mm&sup2; total";
        if (approximated)
            job << "<br/>" << approximated << " of " << extracted.profiles.size()
                << " silhouettes approximated";
        if (extracted.holesDropped)
            job << "<br/>" << extracted.holesDropped << " holes dropped (a sparrow item "
                << "cannot carry them)";
        for (const auto& w : extracted.warnings) {
            job << "<br/>warning: " << w;
            logLine("warning: " + w);
        }
        g_jobSummary = job.str();

        if (input.itemCount == 0) {
            if (ui)
                ui->messageBox(std::string("Nothing to nest.\n\nLog: ") + logPath(),
                               "FuseNest");
            return;
        }

        PendingJob pending;
        pending.valid = true;
        pending.instanceJson = input.json;
        pending.budgetSeconds = req.timeBudgetSeconds;
        pending.workers = req.workers;
        // 4 = debug, 3 = info.
        pending.verboseLevel = req.verboseLog ? 4 : 3;
        pending.profiles = extracted.profiles;
        pending.summary = g_jobSummary;
        pending.itemCount = input.itemCount;

        logLine(std::string("engine busy when the job was ready: ") +
                (solverBusy() ? "yes, queueing" : "no, starting now"));
        if (solverBusy()) {
            // A previous solve is still winding down from a cancel. Queue this
            // one and let the completion handler start it; the dialog opens
            // either way and says what it is waiting for.
            solverCancel();
            g_pending = pending;
            logLine("engine still busy; job queued behind the cancelled solve");
            g_launchPending = true;
            return;
        }

        std::string startError;
        if (!beginJob(pending, startError)) {
            logLine("solverStart failed: " + startError);
            if (ui)
                ui->messageBox("Could not start nesting:\n" + startError + "\n\nLog: " + logPath(),
                               "FuseNest");
            return;
        }

        logLine(timingReport());

        // The dialog cannot open from here -- this command still owns the UI.
        g_launchPending = true;
    }
};

class DestroyHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override {
        if (!g_launchPending) return;
        g_launchPending = false;

        // Straight from destroy. Starting a command while Fusion is still
        // tearing down the previous one looked like it ought to be too early,
        // and a CustomEvent hop used to defer it a turn -- but the direct call
        // was then measured launching cleanly seven times out of seven, in both
        // directions, so the hop is gone. Keeping it would have meant keeping a
        // registered event and a handler for a path nothing takes.
        if (g_resultDef && g_resultDef->execute()) {
            logLine("result dialog launched");
            return;
        }

        std::string err;
        if (Ptr<Application> app = Application::get()) app->getLastError(&err);
        logLine("result dialog failed to launch: " + err);
        // Nothing left to show progress to or accept from.
        solverCancel();
    }
};

/// Puts the last job's numbers back. Applies to a fresh invocation too: the
/// settings someone chose last time are a far better default than ours.
void restoreSetupValues(const Ptr<CommandInputs>& inputs) {
    if (!g_saved.valid || !inputs) return;

    if (Ptr<ValueCommandInput> v = inputs->itemById(kInSheetHeight))
        v->value(g_saved.sheetHeightMm * kMmToCm);
    if (Ptr<ValueCommandInput> v = inputs->itemById(kInSpacing))
        v->value(g_saved.partSpacingMm * kMmToCm);
    if (Ptr<DropDownCommandInput> d = inputs->itemById(kInRotation))
        if (Ptr<ListItems> items = d->listItems())
            if (g_saved.rotationIndex >= 0 && g_saved.rotationIndex < items->count())
                items->item(g_saved.rotationIndex)->isSelected(true);
    if (Ptr<IntegerSpinnerCommandInput> sp = inputs->itemById(kInBudget))
        sp->value(g_saved.timeBudgetSeconds);
    if (Ptr<IntegerSpinnerCommandInput> sp = inputs->itemById(kInWorkers))
        sp->value(g_saved.workers);
    if (Ptr<BoolValueCommandInput> b = inputs->itemById(kInVerbose))
        b->value(g_saved.verboseLog);
}

/// Reselects the bodies the last job used, by token.
///
/// Only when coming back from the result dialog. A token that fails to resolve
/// means the body is gone, which is worth saying out loud -- it is the one way
/// the handoff can lose work, and silence would look like the nest shrinking
/// for no reason.
void restoreSetupSelections(const Ptr<CommandInputs>& inputs) {
    if (!g_restoreSelections || !g_saved.valid || !inputs) return;
    g_restoreSelections = false;

    Ptr<SelectionCommandInput> sel = inputs->itemById(kInParts);
    Ptr<Application> app = Application::get();
    Ptr<Design> design = app ? app->activeProduct()->cast<Design>() : nullptr;
    if (!sel || !design) return;

    int restored = 0, missing = 0, ambiguous = 0;
    for (const std::string& token : g_saved.partTokens) {
        std::vector<Ptr<Base>> found = design->findEntityByToken(token);
        if (found.empty()) {
            ++missing;
            continue;
        }
        // One token can resolve to several proxies of the same body across
        // occurrences. The token came from the entity we were handed, so the
        // first match is the one we want; a count above one is worth knowing
        // about but not worth refusing over.
        if (found.size() > 1) ++ambiguous;
        if (sel->addSelection(found[0])) ++restored;
    }

    logLine("restored " + std::to_string(restored) + " of " +
            std::to_string(g_saved.partTokens.size()) + " parts" +
            (missing ? ", " + std::to_string(missing) + " no longer exist" : "") +
            (ambiguous ? ", " + std::to_string(ambiguous) + " resolved to several entities" : ""));

    if (missing && app)
        if (Ptr<UserInterface> ui = app->userInterface())
            ui->messageBox(std::to_string(missing) +
                               " of the previously selected bodies no longer exist and could "
                               "not be reselected.",
                           "FuseNest");
}

class ActivateHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& args) override {
        Ptr<Command> cmd = args ? args->command() : nullptr;
        restoreSetupSelections(cmd ? cmd->commandInputs() : nullptr);
    }
};

class ValidateHandler : public ValidateInputsEventHandler {
  public:
    void notify(const Ptr<ValidateInputsEventArgs>& args) override {
        if (!args) return;
        const NestRequest req = readInputs(args->inputs());
        // Greys out OK rather than failing later with a confusing message.
        args->areInputsValid(!req.parts.empty() && req.sheetHeightMm > 0.0 &&
                             req.partSpacingMm >= 0.0);
    }
};

class CreatedHandler : public CommandCreatedEventHandler {
  public:
    void notify(const Ptr<CommandCreatedEventArgs>& args) override {
        Ptr<Command> cmd = args ? args->command() : nullptr;
        if (!cmd) {
            logLine("commandCreated: command() was null");
            return;
        }
        cmd->okButtonText("Nest");

        Ptr<CommandInputs> inputs = cmd->commandInputs();
        if (!inputs) {
            logLine("commandCreated: commandInputs() was null");
            return;
        }

        Ptr<SelectionCommandInput> sel =
            inputs->addSelectionInput(kInParts, "Parts", "Select bodies to nest");
        if (sel) {
            // Solid bodies only. Surface bodies have no thickness, so a projected
            // outline is meaningless for nesting, and occurrence selection is
            // deferred until the per-body edge cases are worked through.
            sel->addSelectionFilter("SolidBodies");
            sel->setSelectionLimits(1, 0);  // at least one, no upper bound
        }

        // Units are pinned to mm rather than the design's default length unit:
        // UnitsManager has no defaultLengthUnits() accessor in C++ despite its
        // own doc comment referring to one.
        inputs->addValueInput(kInSheetHeight, "Sheet height", "mm",
                              ValueInput::createByReal(1000.0 * kMmToCm));
        inputs->addValueInput(kInSpacing, "Part spacing", "mm",
                              ValueInput::createByReal(2.0 * kMmToCm));

        Ptr<DropDownCommandInput> rot = inputs->addDropDownCommandInput(
            kInRotation, "Rotation", DropDownStyles::TextListDropDownStyle);
        if (Ptr<ListItems> items = rot ? rot->listItems() : nullptr) {
            items->add(rotationModeLabel(RotationMode::None), false);
            items->add(rotationModeLabel(RotationMode::Half), true);
            items->add(rotationModeLabel(RotationMode::Quarter), false);
            items->add(rotationModeLabel(RotationMode::Continuous), false);
        }

        inputs->addIntegerSpinnerCommandInput(kInBudget, "Time budget (s)", 1, 600, 1, 10);
        // Default 4 = the performance-core count on an M1. Oversubscribing to
        // all cores measured faster, but the efficiency cores make a poor
        // default for a latency-sensitive interactive run.
        inputs->addIntegerSpinnerCommandInput(kInWorkers, "Workers", 1, 64, 1, 4);
        inputs->addBoolValueInput(kInVerbose, "Verbose log", true, "", false);

        Ptr<DropDownCommandInput> mode = inputs->addDropDownCommandInput(
            kInPreviewMode, "Preview using", DropDownStyles::TextListDropDownStyle);
        if (Ptr<ListItems> items = mode ? mode->listItems() : nullptr) {
            items->add(previewModeLabel(PreviewMode::TempBodies), true);
            items->add(previewModeLabel(PreviewMode::Outlines), false);
            items->add(previewModeLabel(PreviewMode::MoveFeatures), false);
        }
        inputs->addBoolValueInput(kInBenchmark, "Benchmark preview modes", true, "", false);

        restoreSetupValues(inputs);

        cmd->validateInputs()->add(&validate_);
        cmd->execute()->add(&execute_);
        cmd->destroy()->add(&destroy_);
        // Selections are restored on activate, not here: a selection input
        // cannot take entities until the dialog is actually up.
        cmd->activate()->add(&activate_);
        logLine("commandCreated: dialog built");
    }

  private:
    ValidateHandler validate_;
    ExecuteHandler execute_;
    DestroyHandler destroy_;
    ActivateHandler activate_;
};

CreatedHandler g_createdHandler;

}  // namespace

bool registerNestCommand(const Ptr<Application>& app) {
    Ptr<UserInterface> ui = app ? app->userInterface() : nullptr;
    if (!ui) return false;

    // A crash can leave the previous definition behind; clear it first.
    if (Ptr<CommandDefinition> stale = ui->commandDefinitions()->itemById(kCmdId))
        stale->deleteMe();

    g_cmdDef = ui->commandDefinitions()->addButtonDefinition(
        kCmdId, "FuseNest", "Nest selected parts onto a sheet");
    if (!g_cmdDef) {
        std::string err;
        app->getLastError(&err);
        logLine("addButtonDefinition failed: " + err);
        return false;
    }

    if (!g_cmdDef->commandCreated()->add(&g_createdHandler)) {
        logLine("commandCreated()->add failed");
        return false;
    }

    Ptr<ToolbarPanel> panel = ui->allToolbarPanels()->itemById(kPanelId);
    if (!panel) {
        logLine(std::string("panel not found: ") + kPanelId);
        return false;
    }

    Ptr<ToolbarControl> ctrl = panel->controls()->addCommand(g_cmdDef);
    if (!ctrl) {
        std::string err;
        app->getLastError(&err);
        logLine("addCommand failed: " + err);
        return false;
    }
    // addCommand is typed ToolbarControl, but isPromoted lives on the derived
    // CommandControl. cast<>() returns null rather than throwing.
    if (Ptr<CommandControl> cc = ctrl->cast<CommandControl>()) cc->isPromoted(true);

    if (Ptr<CommandDefinition> stale = ui->commandDefinitions()->itemById(kResultCmdId))
        stale->deleteMe();
    g_resultDef = ui->commandDefinitions()->addButtonDefinition(
        kResultCmdId, "FuseNest Result", "Shows nesting progress and applies the result");
    if (!g_resultDef || !g_resultDef->commandCreated()->add(&g_resultCreatedHandler)) {
        logLine("could not create the result command");
        return false;
    }

    if (!solverInit(app, &onProgressTick, &reportCompletion)) {
        logLine("solverInit failed -- the command will register but cannot run");
        return false;
    }

    logLine("command registered");
    return true;
}

void unregisterNestCommand(const Ptr<Application>& app) {
    // Before anything else: cancel and join. A solver thread outliving the
    // dylib would execute freed code.
    solverShutdown(app);

    Ptr<UserInterface> ui = app ? app->userInterface() : nullptr;
    if (!ui) return;

    if (Ptr<ToolbarPanel> panel = ui->allToolbarPanels()->itemById(kPanelId))
        if (Ptr<ToolbarControl> ctrl = panel->controls()->itemById(kCmdId)) ctrl->deleteMe();

    if (Ptr<CommandDefinition> def = ui->commandDefinitions()->itemById(kCmdId))
        def->deleteMe();
    if (Ptr<CommandDefinition> def = ui->commandDefinitions()->itemById(kResultCmdId))
        def->deleteMe();

    g_resultCmd = nullptr;
    g_cmdDef = nullptr;
    g_resultDef = nullptr;
    logLine("command unregistered");
}

}  // namespace fusenest
