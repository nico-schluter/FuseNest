// Where main-thread time goes.
//
// Everything the result dialog does -- the write-back, the snapshot copy, the
// status refresh -- runs on Fusion's main thread, so any of it that takes too
// long is a frozen UI rather than a slow background task. These accumulate
// named wall-clock spans so the cost can be attributed instead of guessed at.
//
// Main thread only, deliberately: no locking, and nothing worth measuring here
// happens anywhere else.
#pragma once

#include <chrono>
#include <string>

namespace fusenest {

void timingAdd(const char* name, double ms);

/// Everything accumulated since the last reset, slowest total first.
std::string timingReport();
void timingReset();

/// Times a scope and files it under `name`. `name` must outlive the call,
/// which a string literal does.
class ScopedTime {
  public:
    explicit ScopedTime(const char* name)
        : name_(name), t0_(std::chrono::steady_clock::now()) {}

    ~ScopedTime() {
        timingAdd(name_, std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0_)
                             .count());
    }

    ScopedTime(const ScopedTime&) = delete;
    ScopedTime& operator=(const ScopedTime&) = delete;

  private:
    const char* name_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace fusenest
