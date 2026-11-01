#include "solver.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace adsk::core;

namespace fusenest {
namespace {

const char* kEventId = "FuseNestSolveComplete";

Ptr<CustomEvent> g_event;
std::thread g_thread;
std::atomic<bool> g_busy{false};
std::atomic<int> g_rc{0};
std::atomic<double> g_elapsedMs{0.0};
std::function<void()> g_onFinished;

std::string copyFrom(std::size_t (*fn)(unsigned char*, std::size_t), std::size_t hint) {
    if (hint == 0) hint = 1024;
    std::vector<unsigned char> buf(hint + 1, 0);
    fn(buf.data(), buf.size());
    return std::string(reinterpret_cast<const char*>(buf.data()));
}

/// Runs on the main thread. Fusion calls this once the queued event drains.
class CompleteHandler : public CustomEventHandler {
  public:
    void notify(const Ptr<CustomEventArgs>& /*args*/) override {
        // Join here rather than in the worker: the thread is finished by the
        // time the event is delivered, so this is immediate, and it leaves no
        // joinable thread behind if the add-in is stopped straight after.
        if (g_thread.joinable()) g_thread.join();
        g_busy = false;
        if (g_onFinished) g_onFinished();
    }
};

CompleteHandler g_handler;

}  // namespace

bool solverInit(const Ptr<Application>& app, std::function<void()> onFinished) {
    if (!app) return false;
    g_onFinished = std::move(onFinished);

    g_event = app->registerCustomEvent(kEventId);
    if (!g_event) {
        std::string err;
        app->getLastError(&err);
        logLine("registerCustomEvent failed: " + err);
        return false;
    }
    if (!g_event->add(&g_handler)) {
        logLine("CustomEvent::add failed");
        return false;
    }
    return true;
}

void solverShutdown(const Ptr<Application>& app) {
    nk_cancel();
    if (g_thread.joinable()) g_thread.join();
    g_busy = false;

    if (g_event) {
        g_event->remove(&g_handler);
        g_event = nullptr;
    }
    if (app) app->unregisterCustomEvent(kEventId);
    g_onFinished = nullptr;
    logLine("solver shut down");
}

bool solverStart(const std::string& instanceJson, int budgetSeconds, int workers,
                 std::string& error) {
    if (g_busy.exchange(true)) {
        error = "a nest is already running";
        return false;
    }
    if (g_thread.joinable()) g_thread.join();

    const std::uint64_t budgetMs = static_cast<std::uint64_t>(budgetSeconds) * 1000ull;
    const std::uint32_t n = static_cast<std::uint32_t>(workers > 0 ? workers : 1);

    try {
        // The job string is copied into the thread: the caller's buffer is long
        // gone by the time the solver reads it.
        g_thread = std::thread([json = instanceJson, budgetMs, n] {
            const auto t0 = std::chrono::steady_clock::now();
            const int rc = nk_nest(json.c_str(), budgetMs, n, 0xC0FFEE);
            g_elapsedMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
            g_rc = rc;

            // The one legal way to reach Fusion from a worker. Everything else,
            // messageBox included, can crash the process.
            if (Ptr<Application> app = Application::get()) app->fireCustomEvent(kEventId);
        });
    } catch (const std::exception& e) {
        g_busy = false;
        error = std::string("could not start solver thread: ") + e.what();
        return false;
    }
    return true;
}

void solverCancel() { nk_cancel(); }

bool solverBusy() { return g_busy.load(); }

bool solverProgress(NkProgress& out) { return nk_progress(&out) == 0; }

int solverReturnCode() { return g_rc.load(); }

double solverElapsedMs() { return g_elapsedMs.load(); }

std::string solverResultJson() { return copyFrom(&nk_result_copy, nk_result_len()); }

std::string solverLastError() {
    std::string err = copyFrom(&nk_last_error, 2048);
    if (err.empty()) {
        const std::string panic = copyFrom(&nk_last_panic, 2048);
        if (!panic.empty()) err = "panic: " + panic;
    }
    return err;
}

}  // namespace fusenest
