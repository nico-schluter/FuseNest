#include "nest_command.h"

#include "log.h"
#include "nest_request.h"
#include "profile.h"
#include "solver.h"
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
// Starting a command from inside another command's destroy handler is too
// early; Fusion is still tearing the first one down. Hopping through a
// CustomEvent defers the launch until the main thread is back in its event
// loop and command 1 is really gone.
const char* kLaunchEventId = "FuseNestLaunchResult";
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
const char* kInStatus = "fnStatus";
const char* kInJob = "fnJob";
const char* kInPreviewClock = "fnPreviewClock";

Ptr<CommandDefinition> g_cmdDef;
Ptr<CommandDefinition> g_resultDef;
Ptr<CustomEvent> g_launchEvent;

/// The profiles the last solve was built from. Placement item_ids index into
/// this, so it has to outlive the command that produced it.
std::vector<Profile> g_lastProfiles;

/// What command 1 found, so the result dialog can show it instead of the
/// message box that used to carry it.
std::string g_jobSummary;

/// The live result command, while one is open. A CustomEvent handler gets no
/// Command pointer and ui->activeCommand() yields only an id string, so the
/// only way to reach the open dialog is to stash it here on creation. Cleared
/// in the destroy handler: calling into a destroyed command is undefined.
Ptr<Command> g_resultCmd;

bool g_launchPending = false;
bool g_solveFinished = false;

// Diagnostics for the one open question in this design: what actually makes
// Fusion re-run executePreview on a dialog whose inputs the user is not
// touching. Counted rather than logged per-event -- at a 150 ms tick a short
// solve produces a hundred of each.
long g_tickCount = 0;
long g_previewCount = 0;
long g_validateCount = 0;
long g_inputChangedCount = 0;
long g_previewApplied = 0;
std::string g_lastChangedId;

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

/// Diagnostics for the open question: how many executePreview events does one
/// clock toggle actually produce? Earlier numbers suggested about four, but
/// doExecutePreview() was still in the picture then.
long g_toggles = 0;
long g_previewsSinceToggle = 0;
long g_maxPreviewsPerToggle = 0;
long g_skippedRedraws = 0;

/// Set once a preview has claimed its result as the final one, so the execute
/// handler knows not to apply the same placements a second time.
bool g_previewClaimed = false;

/// Defined below, next to the rest of the preview plumbing; declared here
/// because the dialog's created handler needs it to catch up on a solve that
/// was already running before it opened.
void maybeAdvancePreview();

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
        if (!g_solveFinished) os << "<br/>best so far -- Apply keeps it";
    } else if (!g_solveFinished) {
        os << "looking for a first feasible layout";
    }
    if (have && !g_solveFinished) os << "<br/>" << p.separations << " separation attempts";
    return os.str();
}

/// Pushes the current status into the open result dialog, if there is one.
/// Returns whether a dialog was there to update.
bool refreshResultDialog() {
    if (!g_resultCmd) return false;
    Ptr<CommandInputs> inputs = g_resultCmd->commandInputs();
    if (!inputs) return false;
    if (Ptr<TextBoxCommandInput> tb = inputs->itemById(kInStatus))
        tb->formattedText(statusText());
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
        ++g_previewCount;

        // Draws the layout the tick captured, NOT a freshly fetched one. Fusion
        // rolls each preview back before regenerating, so this re-applies every
        // time; fetching here instead would mean the drawing changed under a
        // user who had not asked for anything.
        //
        // Timeline grouping is skipped: these moves are about to be discarded,
        // and grouping them costs a timeline operation per preview. The preview
        // that OK keeps is regrouped by the execute path.
        ++g_previewsSinceToggle;
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
        ++g_validateCount;
        // Validity gates the preview as well as the OK button: while this was
        // false for the duration of a solve, executePreview never fired once.
        // Both now rest on the same condition -- there is a feasible layout to
        // show and to accept. Which is the whole point of streaming: that
        // becomes true a fraction of a second into a solve rather than at the
        // end of one.
        if (args) args->areInputsValid(!g_shown.empty());
    }
};

/// Answers a direct question: does writing to a TextBoxCommandInput count as an
/// input change at all? If this only ever reports the diagnostic checkbox, then
/// textbox writes are invisible to Fusion's change pipeline, which would explain
/// the preview never firing on its own.
class ResultInputChangedHandler : public InputChangedEventHandler {
  public:
    void notify(const Ptr<InputChangedEventArgs>& args) override {
        ++g_inputChangedCount;
        if (args)
            if (Ptr<CommandInput> in = args->input()) g_lastChangedId = in->id();
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
                " applied, up to " + std::to_string(g_maxPreviewsPerToggle) +
                " per redraw), inputChanged " + std::to_string(g_inputChangedCount));

        // Unconditional: a dialog that is gone has nobody left to show progress
        // to, so the engine has no reason to keep burning cores. On OK the
        // solve has already finished and this is a no-op.
        solverCancel();
        g_resultCmd = nullptr;
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
        // The preview clock. Fusion regenerates a preview in response to an input
        // VALUE changing, and a TextBoxCommandInput's formattedText is not one --
        // writing it fires no inputChanged at all. So the status readout cannot
        // drive the preview and something else has to. This input exists purely
        // to be toggled once per progress tick; it is hidden because it carries
        // no meaning to the user.
        if (Ptr<BoolValueCommandInput> clock =
                inputs->addBoolValueInput(kInPreviewClock, "preview clock", true, "", false))
            clock->isVisible(false);

        cmd->validateInputs()->add(&validate_);
        cmd->executePreview()->add(&preview_);
        cmd->inputChanged()->add(&inputChanged_);
        cmd->execute()->add(&execute_);
        cmd->destroy()->add(&destroy_);

        g_resultCmd = cmd;
        g_tickCount = g_previewCount = g_validateCount = g_inputChangedCount = 0;
        g_previewApplied = 0;
        g_previewClaimed = false;
        g_lastChangedId.clear();
        g_previewedGeneration = 0;
        g_lastPreviewAt = {};
        g_toggles = g_previewsSinceToggle = g_maxPreviewsPerToggle = g_skippedRedraws = 0;
        g_shown.clear();
        g_shownInfo = NkSnapshotInfo{};
        // A solve may already have published layouts before this dialog opened.
        maybeAdvancePreview();
        logLine("result dialog open");
    }

  private:
    ResultValidateHandler validate_;
    ResultPreviewHandler preview_;
    ResultInputChangedHandler inputChanged_;
    ResultExecuteHandler execute_;
    ResultDestroyHandler destroy_;
};

ResultCreatedHandler g_resultCreatedHandler;

/// Opens the result dialog, one event-loop turn after command 1 has gone.
class LaunchHandler : public CustomEventHandler {
  public:
    void notify(const Ptr<CustomEventArgs>& /*args*/) override {
        if (!g_resultDef) return;
        if (!g_resultDef->execute()) {
            std::string err;
            if (Ptr<Application> app = Application::get()) app->getLastError(&err);
            logLine("result command failed to start: " + err);
            // Without a dialog there is nothing to show progress to or accept
            // from, so the solve is pointless.
            solverCancel();
        }
    }
};

LaunchHandler g_launchHandler;

// --- engine callbacks ----------------------------------------------------

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
    if (!solverSnapshot(fresh, info)) return;

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

    g_maxPreviewsPerToggle = std::max(g_maxPreviewsPerToggle, g_previewsSinceToggle);
    g_previewsSinceToggle = 0;
    ++g_toggles;

    if (Ptr<CommandInputs> inputs = g_resultCmd->commandInputs())
        if (Ptr<BoolValueCommandInput> b = inputs->itemById(kInPreviewClock))
            b->value(!b->value());
}

/// Main thread, every ~150 ms while a solve runs.
void onProgressTick() {
    ++g_tickCount;
    // Before the early return: the trace is worth having even with no dialog
    // open to show it to.
    solverDrainKernelLog();
    if (!refreshResultDialog()) return;

    // The open question. doExecutePreview() is documented as being for commands
    // with no dialog, with "changing command inputs" named as the normal
    // trigger -- and we are changing one just above. Call it anyway and count
    // what arrives; the log says which of the two is doing the work.
    maybeAdvancePreview();

    if (g_tickCount == 1 || g_tickCount % 40 == 0)
        logLine("tick " + std::to_string(g_tickCount) + ": gen " +
                std::to_string(g_shownInfo.generation) + ", redraws " +
                std::to_string(g_toggles) + " (" + std::to_string(g_skippedRedraws) +
                " skipped as invisible), previews " + std::to_string(g_previewCount) +
                ", previews per redraw max " + std::to_string(g_maxPreviewsPerToggle));
}

/// Main thread, once, however the solve ended.
void reportCompletion() {
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

        logLine("execute:");
        logLine(req.describe());

        ExtractOptions opts;
        const ExtractResult extracted = extractProfiles(app, req.parts, opts);

        int approximated = 0;
        double totalAreaMm2 = 0.0;
        size_t totalPoints = 0;
        for (const auto& p : extracted.profiles) {
            if (p.silhouetteApproximated) ++approximated;
            totalAreaMm2 += p.areaMm2;
            totalPoints += p.outer.size();
        }

        g_lastProfiles = extracted.profiles;
        const SolverInput input = buildSolverInput(req, extracted.profiles, "fusenest");
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

        g_solveFinished = false;
        // 4 = debug, 3 = info. Set before the solve so the import and the first
        // cycle are already covered.
        solverSetLogLevel(req.verboseLog ? 4 : 3);
        std::string startError;
        if (!solverStart(input.json, req.timeBudgetSeconds, req.workers, startError)) {
            logLine("solverStart failed: " + startError);
            if (ui)
                ui->messageBox("Could not start nesting:\n" + startError + "\n\nLog: " + logPath(),
                               "FuseNest");
            return;
        }
        logLine("solve started: " + std::to_string(input.itemCount) + " items, " +
                std::to_string(req.workers) + " workers, " +
                std::to_string(req.timeBudgetSeconds) + " s budget");

        // The dialog cannot open from here -- this command still owns the UI.
        g_launchPending = true;
    }
};

class DestroyHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override {
        if (!g_launchPending) return;
        g_launchPending = false;
        if (Ptr<Application> app = Application::get()) app->fireCustomEvent(kLaunchEventId);
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

        cmd->validateInputs()->add(&validate_);
        cmd->execute()->add(&execute_);
        cmd->destroy()->add(&destroy_);
        logLine("commandCreated: dialog built");
    }

  private:
    ValidateHandler validate_;
    ExecuteHandler execute_;
    DestroyHandler destroy_;
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

    g_launchEvent = app->registerCustomEvent(kLaunchEventId);
    if (!g_launchEvent || !g_launchEvent->add(&g_launchHandler)) {
        logLine("could not register the launch event");
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

    if (g_launchEvent) {
        g_launchEvent->remove(&g_launchHandler);
        g_launchEvent = nullptr;
    }
    if (app) app->unregisterCustomEvent(kLaunchEventId);

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
