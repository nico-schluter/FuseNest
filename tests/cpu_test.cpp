// What the worker count is allowed to be: never 1, never more than measured,
// and never a guess at hardware that would not identify itself.
//
// Only the policy is asserted. The detection beside it answers for whichever
// machine is running the test, so a test cannot know the right answer -- it can
// only check that whatever came back is a number the policy can use, which is
// the one thing that would actually break the add-in.
#include "cpu.h"

#include <climits>
#include <cstdio>
#include <string>

using namespace fusenest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

/// The floor and ceiling the policy promises, stated here independently rather
/// than read from cpu.cpp: a test that imports the constants it is checking
/// agrees with any change to them, including a mistake.
constexpr int kFloor = 2;
constexpr int kCeiling = 8;

void aFailedDetectionDoesNotGuess() {
    check(workerCountFor(0) == kFloor, "an unknown core count falls to the floor");
    check(workerCountFor(-1) == kFloor, "so does a negative one");
    check(workerCountFor(INT_MIN) == kFloor, "and the most negative one there is");
}

void oneWorkerIsNeverChosen() {
    // Measured worst on both counts -- slowest and least dense -- so the floor
    // exists to make it unreachable, whatever the machine claims.
    check(workerCountFor(1) == kFloor, "a single-core machine still gets the floor");
    check(workerCountFor(2) == kFloor, "two cores get two");
}

void theCountIsTheCoreCountInBetween() {
    check(workerCountFor(3) == 3, "three cores get three");
    check(workerCountFor(4) == 4, "four cores get four, as the hardcoded value did");
    check(workerCountFor(6) == 6, "six cores get six");
    check(workerCountFor(8) == 8, "eight cores get eight");
}

void nothingExceedsWhatWasMeasured() {
    check(workerCountFor(9) == kCeiling, "nine cores are capped");
    check(workerCountFor(64) == kCeiling, "so are sixty-four");
    check(workerCountFor(INT_MAX) == kCeiling, "and a nonsense count cannot overflow the cap");
}

void thePolicyIsMonotonicAndInRange() {
    // Swept rather than spot-checked: a clamp written with the bounds the wrong
    // way round passes every single-value case above for some inputs.
    int previous = workerCountFor(INT_MIN);
    for (int cores = -2; cores <= 256; ++cores) {
        const int workers = workerCountFor(cores);
        if (workers < kFloor || workers > kCeiling) {
            check(false, "every input lands inside the promised range");
            return;
        }
        if (workers < previous) {
            check(false, "more cores never mean fewer workers");
            return;
        }
        previous = workers;
    }
    check(true, "every input lands inside the promised range, never decreasing");
}

void thisMachineResolvesToSomethingUsable() {
    const int cores = detectPerformanceCores();
    // Zero is a legitimate answer -- it is how detection reports that it could
    // not tell -- so the assertion is only that it did not come back negative,
    // which would mean a sysctl or a Win32 call was misread rather than failed.
    check(cores >= 0, "detection returns a count or zero, never a negative");
    std::printf("      (this machine: %d performance cores)\n", cores);

    const int workers = autoWorkerCount();
    check(workers >= kFloor && workers <= kCeiling, "and the derived count is in range");
    check(workers == workerCountFor(cores), "auto is exactly the policy applied to detection");
    std::printf("      (this machine: %d workers)\n", workers);
}

void anOverrideIsTakenAsGiven() {
    check(resolveWorkerCount(0) == autoWorkerCount(), "zero means derive");
    check(resolveWorkerCount(-5) == autoWorkerCount(), "so does a negative");
    check(resolveWorkerCount(1) == 1,
          "an override of one is allowed where the policy would refuse it");
    check(resolveWorkerCount(32) == 32, "an override past the policy cap is honoured");
    check(resolveWorkerCount(9999) == 64, "but not past what the kernel accepts");
    check(resolveWorkerCount(INT_MAX) == 64, "and cannot overflow on the way");
}

void theJournalLineSaysWhereTheNumberCameFrom() {
    const std::string automatic = describeWorkerCount(0);
    check(automatic.find("workers: ") == 0, "the line starts with the count");
    check(automatic.find("auto") != std::string::npos, "a derived count says so");
    check(automatic.find(std::to_string(autoWorkerCount())) != std::string::npos,
          "and names the count that will actually be used");
    std::printf("      %s\n", automatic.c_str());

    const std::string overridden = describeWorkerCount(7);
    check(overridden.find("settings.json") != std::string::npos,
          "an overridden count names the file it came from");
    check(overridden.find("auto") == std::string::npos, "and does not claim to be automatic");
    check(overridden.find("7") != std::string::npos, "and names the override");
    std::printf("      %s\n", overridden.c_str());
}

}  // namespace

int main() {
    std::printf("-- detection failure --\n");
    aFailedDetectionDoesNotGuess();
    std::printf("-- the floor --\n");
    oneWorkerIsNeverChosen();
    std::printf("-- the core count --\n");
    theCountIsTheCoreCountInBetween();
    std::printf("-- the cap --\n");
    nothingExceedsWhatWasMeasured();
    std::printf("-- shape --\n");
    thePolicyIsMonotonicAndInRange();
    std::printf("-- overrides --\n");
    anOverrideIsTakenAsGiven();
    std::printf("-- the journal line --\n");
    theJournalLineSaysWhereTheNumberCameFrom();
    std::printf("-- this machine --\n");
    thisMachineResolvesToSomethingUsable();

    std::printf("%s\n", g_failures ? "FAILURES" : "all passed");
    return g_failures;
}
