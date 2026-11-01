#include "nest_command.h"

#include "log.h"
#include "nest_request.h"
#include "profile.h"
#include "solver.h"
#include "writeback.h"
#include "solver_input.h"

#include <Fusion/FusionAll.h>

#include <fstream>
#include <sstream>

using namespace adsk::core;
using namespace adsk::fusion;

namespace fusenest {
namespace {

const char* kCmdId = "FuseNestCommand";
// A second, dialog-free command exists purely so the write-back runs INSIDE a
// command execute handler: Fusion has no undo-transaction API, and that is the
// only thing it groups into a single undo step.
const char* kApplyCmdId = "FuseNestApplyCommand";
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

Ptr<CommandDefinition> g_cmdDef;
Ptr<CommandDefinition> g_applyDef;

/// The profiles the last solve was built from. Placement item_ids index into
/// this, so it has to outlive the command that produced it.
std::vector<Profile> g_lastProfiles;

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

    return req;
}

/// Does the actual model change. Running here, inside a command execute, is
/// what makes the whole write-back a single undo step.
class ApplyExecuteHandler : public CommandEventHandler {
  public:
    void notify(const Ptr<CommandEventArgs>& /*args*/) override {
        Ptr<Application> app = Application::get();
        Ptr<UserInterface> ui = app ? app->userInterface() : nullptr;

        const WriteBackResult wb = applyPlacements(app, g_lastProfiles);

        NkProgress p{};
        solverProgress(p);

        std::ostringstream os;
        os << "density:      " << p.density * 100.0 << " %\n"
           << "strip width:  " << p.strip_width << " mm\n"
           << "wall:         " << solverElapsedMs() << " ms\n"
           << "\nmoved:        " << wb.moved << " bodies\n";
        if (wb.skipped) os << "skipped:      " << wb.skipped << "\n";
        if (wb.grouped) os << "timeline:     grouped into one entry\n";
        if (!wb.warnings.empty()) {
            os << "\nwarnings:\n";
            for (const auto& w : wb.warnings) {
                os << "  - " << w << "\n";
                logLine("writeback warning: " + w);
            }
        }
        os << "\nLog: " << logPath();
        if (ui) ui->messageBox(os.str(), "FuseNest");
    }
};

class ApplyCreatedHandler : public CommandCreatedEventHandler {
  public:
    void notify(const Ptr<CommandCreatedEventArgs>& args) override {
        Ptr<Command> cmd = args ? args->command() : nullptr;
        if (!cmd) return;
        // No inputs and no dialog: fire straight through to execute.
        cmd->isAutoExecute(true);
        cmd->isRepeatable(false);
        cmd->execute()->add(&execute_);
    }

  private:
    ApplyExecuteHandler execute_;
};

ApplyCreatedHandler g_applyCreatedHandler;

/// Runs on the main thread once a solve ends, delivered by CustomEvent.
void reportCompletion() {
    Ptr<Application> app = Application::get();
    Ptr<UserInterface> ui = app ? app->userInterface() : nullptr;

    const int rc = solverReturnCode();
    std::ostringstream os;

    if (rc != 0) {
        os << "Nesting failed (rc " << rc << ")\n\n" << solverLastError();
        logLine(os.str());
        if (ui) ui->messageBox(os.str(), "FuseNest");
        return;
    }

    const std::string json = solverResultJson();
    const std::string path = std::string(outputDir()) + "/fusenest_solution.json";
    {
        std::ofstream f(path);
        if (f) f << json;
    }

    logLine("solve complete: " + json);

    // Autodesk's own threading guidance: end any active command before making
    // changes that add undo operations. Then run the write-back as a command,
    // so Fusion treats it as one undo step.
    if (ui) {
        if (Ptr<CommandDefinition> sel = ui->commandDefinitions()->itemById("SelectCommand"))
            sel->execute();
        if (g_applyDef && !g_applyDef->execute()) {
            std::string err;
            app->getLastError(&err);
            logLine("apply command failed to start: " + err);
            ui->messageBox("Solve finished but the write-back could not start:\n" + err,
                           "FuseNest");
        }
    }
}

// --- handlers ------------------------------------------------------------
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

        std::ostringstream os;
        os << req.describe() << "\n"
           << "bodies:       " << extracted.bodiesExamined << "\n"
           << "profiles:     " << extracted.profiles.size() << "\n"
           << "outer points: " << totalPoints << "\n"
           << "total area:   " << totalAreaMm2 << " mm2\n"
           << "holes found:  " << extracted.holesDropped
           << " (captured, but a sparrow item cannot carry them)\n"
           << "approximated: " << approximated << " of " << extracted.profiles.size()
           << " silhouettes\n";

        if (!extracted.warnings.empty()) {
            os << "\nwarnings:\n";
            for (const auto& w : extracted.warnings) {
                os << "  - " << w << "\n";
                logLine("warning: " + w);
            }
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

        os << "\nsolver input:\n"
           << "  items:      " << input.itemCount << "\n"
           << "  bytes:      " << input.json.size() << "\n"
           << "  holes lost: " << input.holesDropped << "\n";

        if (input.itemCount == 0) {
            os << "\nNothing to nest.";
            if (ui) ui->messageBox(os.str(), "FuseNest");
            return;
        }

        std::string startError;
        if (!solverStart(input.json, req.timeBudgetSeconds, req.workers, startError)) {
            os << "\ncould not start: " << startError;
            logLine("solverStart failed: " + startError);
            if (ui) ui->messageBox(os.str(), "FuseNest");
            return;
        }

        os << "\nNesting " << input.itemCount << " parts on " << req.workers
           << " workers for up to " << req.timeBudgetSeconds << " s.\n\n"
           << "This dialog closes now and the solve continues in the background --\n"
              "Fusion stays usable. A second message box reports the result.\n"
              "\nLog: " << logPath();
        if (ui) ui->messageBox(os.str(), "FuseNest");
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

        cmd->validateInputs()->add(&validate_);
        cmd->execute()->add(&execute_);
        logLine("commandCreated: dialog built");
    }

  private:
    ValidateHandler validate_;
    ExecuteHandler execute_;
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

    if (Ptr<CommandDefinition> stale = ui->commandDefinitions()->itemById(kApplyCmdId))
        stale->deleteMe();
    g_applyDef = ui->commandDefinitions()->addButtonDefinition(
        kApplyCmdId, "FuseNest Apply", "Applies the last nesting result");
    if (!g_applyDef || !g_applyDef->commandCreated()->add(&g_applyCreatedHandler)) {
        logLine("could not create the apply command");
        return false;
    }

    if (!solverInit(app, &reportCompletion)) {
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

    g_cmdDef = nullptr;
    logLine("command unregistered");
}

}  // namespace fusenest
