// How many solver workers this machine should run.
//
// Split in two on purpose, the way settings.cpp splits parsing from loading:
// the POLICY is pure and tested exhaustively, the DETECTION is platform code
// that no test on one machine can say anything useful about.
//
// Worker count is not only a speed dial. Each of sparrow's workers clones the
// problem and the collision tracker and is seeded from the main rng, so the
// count changes the search trajectory and therefore the layout. Two
// consequences worth stating out loud: a nest is only as reproducible as its
// worker count, and a bug report is worth nothing without the count in it. The
// resolved value is logged on every solve for exactly that reason.
#pragma once

#include <string>

namespace fusenest {

/// Performance cores, or 0 when the machine will not say.
///
/// "Performance" rather than "logical": the separator is collision-check bound
/// and cache-hungry, so SMT siblings and efficiency cores both make it slower
/// per worker while adding a full clone of the problem to memory. On Apple
/// silicon this is the P-core cluster; on an Intel Mac there is one cluster and
/// this is its physical cores; on Windows it is the cores in the most
/// performant efficiency class.
int detectPerformanceCores();

/// The worker count to use given a performance-core count. Pure.
///
/// `performanceCores` of 0 means detection failed, which is NOT an error: the
/// answer is then the conservative floor rather than a guess at the hardware.
int workerCountFor(int performanceCores);

/// What a solve runs with when nothing overrides it: the policy applied to the
/// detected core count.
int autoWorkerCount();

/// The count a solve should actually use. `workerOverride` of 0 or less -- the
/// shipped default -- means derive it; anything else is taken as given, clamped
/// only to what the kernel will accept.
///
/// Here rather than at the two call sites so that the number written to the
/// journal at start-up and the number handed to the kernel per solve cannot
/// drift apart. They are the same question asked twice.
int resolveWorkerCount(int workerOverride);

/// One journal line: the count, where it came from, and the core count behind
/// it. Always recorded, not a diagnostic -- it is the only value that changes
/// the layout without anyone choosing it, so a log that omits it cannot explain
/// why two machines nested the same parts differently.
std::string describeWorkerCount(int workerOverride);

}  // namespace fusenest
