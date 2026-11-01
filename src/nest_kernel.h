// C++ view of the Rust kernel. Hand-written on purpose: the surface is small
// enough that a bindgen dependency would cost more than it saves, and keeping
// it hand-written makes the ABI contract explicit and reviewable.
//
// Keep this in sync with nest_kernel/src/lib.rs by hand. A mismatch in the
// NkProgress layout is silent and produces garbage -- it has happened once.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

/// One placed part. `item_id` is the id we wrote into the instance, which is
/// the index of the profile that produced it.
struct NkPlacement {
    std::uint64_t item_id;
    float rotation;   // degrees, counter-clockwise
    float tx;         // millimetres
    float ty;
    std::uint32_t reflected;  // always 0 while we request no reflection axes
};

struct NkProgress {
    std::uint32_t phase;        // 0 idle, 1 exploration, 2 compression
    std::uint64_t iteration;    // restarts per separation; use `separations`
    std::uint64_t separations;  // monotonic
    float density;              // fraction, not percent
    float strip_width;
    std::uint32_t running;
    std::uint32_t cancelled;
};

// Blocking. 0 success, -1 caught panic, -2 bad arguments, -3 solver or parse
// error. Call on a worker thread: sparrow parallelises internally via workers.
std::int32_t nk_nest(const char* input_json, std::uint64_t time_limit_ms,
                     std::uint32_t n_workers, std::uint64_t seed);

// Safe from the main thread at any time, including while a solve runs.
std::int32_t nk_progress(NkProgress* out);

// Graceful: the solver polls this and returns its best solution so far.
void nk_cancel();

// Placements as structs, so no JSON parser is needed on this side.
std::size_t nk_placement_count();
std::int32_t nk_placement_at(std::size_t index, NkPlacement* out);
float nk_strip_width();

std::size_t nk_result_len();
std::size_t nk_result_copy(unsigned char* buf, std::size_t len);
std::size_t nk_last_error(unsigned char* buf, std::size_t len);
std::size_t nk_last_panic(unsigned char* buf, std::size_t len);

}  // extern "C"
