#include "solver.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace adsk::core;

namespace fusenest {
namespace {

const char* kEventId = "FuseNestSolveComplete";
const char* kProgressEventId = "FuseNestSolveProgress";

// Fast enough to feel live, slow enough that the main thread spends no real
// time on it. The kernel's atomics update far more often than this; we are
// sampling them, not following them.
constexpr int kTickMs = 150;

Ptr<CustomEvent> g_event;
Ptr<CustomEvent> g_progressEvent;
std::thread g_thread;
std::atomic<bool> g_busy{false};
std::atomic<int> g_rc{0};
std::atomic<double> g_elapsedMs{0.0};
std::function<void()> g_onFinished;
std::function<void()> g_onProgress;

// The ticker exists because kernel progress is polled, not pushed: something
// has to decide when to look. A thread firing a CustomEvent is the only option
// -- Fusion has no timer a worker can hang off.
std::thread g_ticker;
std::mutex g_tickerMutex;
std::condition_variable g_tickerCv;
bool g_tickerStop = false;

void startTicker() {
    {
        std::lock_guard<std::mutex> lock(g_tickerMutex);
        g_tickerStop = false;
    }
    g_ticker = std::thread([] {
        for (;;) {
            std::unique_lock<std::mutex> lock(g_tickerMutex);
            // wait_for rather than sleep_for so stopping is immediate: a join
            // that blocks for a whole tick would be a visible hitch.
            g_tickerCv.wait_for(lock, std::chrono::milliseconds(kTickMs),
                                [] { return g_tickerStop; });
            if (g_tickerStop) return;
            lock.unlock();
            if (Ptr<Application> app = Application::get())
                app->fireCustomEvent(kProgressEventId);
        }
    });
}

void stopTicker() {
    {
        std::lock_guard<std::mutex> lock(g_tickerMutex);
        g_tickerStop = true;
    }
    g_tickerCv.notify_all();
    if (g_ticker.joinable()) g_ticker.join();
}

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
        stopTicker();
        // Join here rather than in the worker: the thread is finished by the
        // time the event is delivered, so this is immediate, and it leaves no
        // joinable thread behind if the add-in is stopped straight after.
        if (g_thread.joinable()) g_thread.join();
        g_busy = false;
        if (g_onFinished) g_onFinished();
    }
};

/// Runs on the main thread, every kTickMs while a solve is live. A tick queued
/// just before completion can still arrive after it; reading the atomics is
/// harmless either way, so this deliberately does not check g_busy.
class ProgressHandler : public CustomEventHandler {
  public:
    void notify(const Ptr<CustomEventArgs>& /*args*/) override {
        if (g_onProgress) g_onProgress();
    }
};

CompleteHandler g_handler;
ProgressHandler g_progressHandler;

}  // namespace

bool solverInit(const Ptr<Application>& app, std::function<void()> onProgress,
                std::function<void()> onFinished) {
    if (!app) return false;
    g_onProgress = std::move(onProgress);
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

    g_progressEvent = app->registerCustomEvent(kProgressEventId);
    if (!g_progressEvent) {
        std::string err;
        app->getLastError(&err);
        logLine("registerCustomEvent (progress) failed: " + err);
        return false;
    }
    if (!g_progressEvent->add(&g_progressHandler)) {
        logLine("CustomEvent::add (progress) failed");
        return false;
    }
    return true;
}

void solverShutdown(const Ptr<Application>& app) {
    nk_cancel();
    stopTicker();
    if (g_thread.joinable()) g_thread.join();
    g_busy = false;

    if (g_event) {
        g_event->remove(&g_handler);
        g_event = nullptr;
    }
    if (g_progressEvent) {
        g_progressEvent->remove(&g_progressHandler);
        g_progressEvent = nullptr;
    }
    if (app) {
        app->unregisterCustomEvent(kEventId);
        app->unregisterCustomEvent(kProgressEventId);
    }
    g_onFinished = nullptr;
    g_onProgress = nullptr;
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

    try {
        startTicker();
    } catch (const std::exception& e) {
        // A solve without a progress readout is degraded, not broken: let it run.
        logLine(std::string("could not start progress ticker: ") + e.what());
    }
    return true;
}

void solverDrainKernelLog() {
    // One buffer, reused: a drain happens every 150 ms and most of them find
    // nothing. Sized to swallow a whole cycle's worth of lines in one call.
    static std::vector<unsigned char> buf(16 * 1024);
    for (;;) {
        const std::size_t n = nk_log_drain(buf.data(), buf.size());
        if (n == 0) return;
        // The kernel writes newline-terminated lines; split so each reaches the
        // log with its own timestamp rather than as one block.
        const char* p = reinterpret_cast<const char*>(buf.data());
        const char* end = p + n;
        while (p < end) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
            const std::size_t len = nl ? static_cast<std::size_t>(nl - p) : static_cast<std::size_t>(end - p);
            if (len) logLine("  kernel | " + std::string(p, len));
            p += len + 1;
        }
    }
}

void solverSetLogLevel(int level) { nk_log_set_level(static_cast<std::uint32_t>(level)); }

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
