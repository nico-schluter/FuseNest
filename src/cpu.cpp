#include "cpu.h"

#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <vector>
#else
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace fusenest {

namespace {

#ifdef _WIN32

/// Cores in the most performant efficiency class, or 0.
///
/// EfficiencyClass is Win32's own ordering and HIGHER means more performant,
/// which reads backwards from the name. On a machine with one class -- every
/// pre-Alder-Lake part -- they are all 0, and the answer is simply the core
/// count. RelationProcessorCore yields one record per PHYSICAL core, so SMT
/// siblings are already excluded without touching the affinity masks.
int performanceCoresWin32() {
    DWORD bytes = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes))
        return 0;  // Succeeding with no buffer would mean no data to read.
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return 0;

    std::vector<unsigned char> buffer(bytes);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &bytes)) return 0;

    // Two passes, because the best class is not known until every record has
    // been seen. Walked by each record's own Size: these are variable length,
    // so indexing them as an array is the classic way to read garbage.
    BYTE best = 0;
    int count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        DWORD offset = 0;
        while (offset < bytes) {
            auto* record = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
            if (record->Size == 0) return 0;  // Refuse to loop forever.
            if (record->Relationship == RelationProcessorCore) {
                const BYTE efficiency = record->Processor.EfficiencyClass;
                if (pass == 0)
                    best = std::max(best, efficiency);
                else if (efficiency == best)
                    ++count;
            }
            offset += record->Size;
        }
    }
    return count;
}

#else

/// Reads an integer sysctl by name, or 0 if it is not there.
///
/// Missing is the normal case for the perflevel keys: they exist only on
/// machines with more than one core cluster, so an Intel Mac has none of them
/// and falls through to the next candidate.
int sysctlInt(const char* name) {
    int value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return 0;
    if (size != sizeof(value) || value <= 0) return 0;
    return value;
}

#endif

}  // namespace

int workerCountFor(int performanceCores) {
    // Measured on the 29-part instance, M1 Air (4 P-cores + 4 E-cores), three
    // repeats each, no binding clock:
    //
    //   workers  wall (3 reps)         density  width      separations
    //   1        31.78 31.88 31.96     71.32%   3854.467   159
    //   2        15.59 15.61 15.50     71.91%   3822.779   157
    //   4        18.44 18.63 18.94     72.54%   3789.458   163
    //   6        41.31 42.20 43.85     73.89%   3719.963   182
    //   8        42.81 34.46 28.32     71.81%   3828.041   156
    //
    // Read that carefully, because it says less than it looks like it says. The
    // layout is a pure function of (seed, n_workers) -- every repeat above is
    // identical but for wall time -- so the density column is NOT a scaling
    // curve. Changing the worker count changes how many candidate moves are
    // sampled per iteration and therefore which one wins, which is a DIFFERENT
    // search, not more of the same one. 6 beating 4 and 8 losing to 2 are
    // artifacts of one instance and one seed, and tuning to them would be
    // fitting noise. There is no quality-optimal count to detect.
    //
    // What survives as structural, and is what this policy is built on:
    //
    //   - One worker is genuinely worse: least search per cycle, worst density,
    //     and twice the wall time of two. Never choose it.
    //   - Saturating every core is genuinely bad. At 8 on 8 the wall time
    //     scatters across 28-43s while every other count holds to a third of a
    //     second, because the pool is now competing with everything else on the
    //     machine. A nest whose duration is a lottery is worse than a slower one.
    //   - In between, time is stable and density is a coin toss.
    //
    // So: the performance-core count, which leaves the efficiency cores for the
    // rest of the system, with a floor of 2 and a cap of 8. On this machine that
    // resolves to 4 -- the value that was hardcoded -- so what changes here is
    // that it is now true on other machines too, not that it was retuned.
    constexpr int kFloor = 2;
    // No measurement supports this number, and it is honest to say so: nothing
    // with more than 8 performance cores has been run. It is a guard on the one
    // cost that is known to scale with the count rather than wander -- each
    // worker clones the problem and the collision tracker -- not a claim that 9
    // would be worse at nesting.
    constexpr int kCeiling = 8;

    // Detection failing is not an invitation to guess at the hardware. The floor
    // is the only count that cannot oversubscribe a machine that would not say
    // how small it is -- a two-core VM being the case that matters.
    if (performanceCores <= 0) return kFloor;
    return std::clamp(performanceCores, kFloor, kCeiling);
}

int autoWorkerCount() { return workerCountFor(detectPerformanceCores()); }

int resolveWorkerCount(int workerOverride) {
    if (workerOverride <= 0) return autoWorkerCount();
    // The kernel's own bound, not the policy's: an override is someone saying
    // they know better, and the only thing worth refusing is a value nk_nest
    // cannot use.
    return std::clamp(workerOverride, 1, 64);
}

std::string describeWorkerCount(int workerOverride) {
    const int cores = detectPerformanceCores();
    const int workers = resolveWorkerCount(workerOverride);
    const std::string detected = cores > 0 ? std::to_string(cores) + " performance cores"
                                           : "an unreadable performance-core count";
    return "workers: " + std::to_string(workers) +
           (workerOverride > 0 ? " (set in settings.json; " + detected + " detected)"
                               : " (auto, from " + detected + ")");
}

int detectPerformanceCores() {
#ifdef _WIN32
    if (const int cores = performanceCoresWin32(); cores > 0) return cores;
    // GetSystemInfo cannot fail and counts logical processors, so this is the
    // pessimistic end of the fallback chain rather than a good answer.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return static_cast<int>(info.dwNumberOfProcessors);
#else
    // perflevel0 is the most performant cluster: the P-cores on Apple silicon.
    // physicalcpu rather than logicalcpu so that an Intel Mac, which has one
    // cluster and two hyperthreads per core, does not count the siblings.
    if (const int cores = sysctlInt("hw.perflevel0.physicalcpu"); cores > 0) return cores;
    if (const int cores = sysctlInt("hw.physicalcpu"); cores > 0) return cores;
    return sysctlInt("hw.ncpu");
#endif
}

}  // namespace fusenest
