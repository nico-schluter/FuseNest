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
    std::uint32_t sheet;      // 0-based; always 0 when the solve had no sheets
};

/// How the strip is cut into sheets.
///
/// Passed to nk_nest as a POINTER, and a null one asks for the infinite strip
/// the add-in solved before sheets existed -- so the sheet machinery is opt-in
/// and its absence costs nothing. The sheets are laid out along +x with `gap`
/// of empty model space between them, which is where the kernel puts the
/// immovable separators that stop a part straddling a boundary.
struct NkSheets {
    float width;               // one sheet, millimetres
    float gap;                 // between sheets; 0 asks for a default
    std::uint32_t max_sheets;  // allocated; see NkSnapshotInfo::sheets_used
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
                     std::uint32_t n_workers, std::uint64_t seed,
                     const NkSheets* sheets);

// Safe from the main thread at any time, including while a solve runs.
std::int32_t nk_progress(NkProgress* out);

// Graceful: the solver polls this and returns its best solution so far.
void nk_cancel();

/// A published layout, apart from its placements.
///
/// `strip_width` and `density` belong to THIS layout, which is the whole point
/// of them living here rather than being read from NkProgress: those fields are
/// overwritten continuously by the separator, including from states where items
/// still overlap, so pairing them with a snapshot describes a layout other than
/// the one being drawn.
struct NkSnapshotInfo {
    std::uint64_t generation;
    std::uint64_t count;  // placements held, which may exceed what a copy fits
    float strip_width;    // millimetres
    float density;        // fraction
    /// Sheets this layout occupies. The caller allocates a maximum; this is the
    /// answer. Zero when the solve had no sheets.
    std::uint32_t sheets_used;
    /// Parts that fit on no allocated sheet. They are NOT among the placements:
    /// a part in the open space past the last sheet is not something anyone can
    /// cut, so it is reported as a count and its body is left where it was.
    std::uint32_t overflow;
    /// Sheet-to-sheet distance: sheet width plus the gap between sheets. Comes
    /// back from the kernel rather than being recomputed here, because a gap of
    /// zero asks the kernel for its own default -- so this is the only thing
    /// that knows where the sheet boundaries actually are.
    float sheet_pitch;
};

// The published layout, replaced on every feasible improvement rather than
// written once at the end. Generation is cheap and is the only call needed to
// decide whether anything changed; the copy takes everything under one lock,
// because reading placements one at a time can splice two layouts together.
std::uint64_t nk_snapshot_generation();
std::size_t nk_snapshot_copy(NkPlacement* out, std::size_t max, NkSnapshotInfo* info);

// sparrow narrates its own search through the `log` crate; the kernel captures
// those lines into a ring buffer instead of letting a GUI process discard them.
// Drain returns newline-terminated text and 0 when empty, so callers loop.
// Level: 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace. Info is the
// cycle-by-cycle narrative; debug adds per-iteration separator chatter.
void nk_log_set_level(std::uint32_t level);
std::size_t nk_log_drain(unsigned char* buf, std::size_t len);

std::size_t nk_result_len();
std::size_t nk_result_copy(unsigned char* buf, std::size_t len);
std::size_t nk_last_error(unsigned char* buf, std::size_t len);
std::size_t nk_last_panic(unsigned char* buf, std::size_t len);

}  // extern "C"
