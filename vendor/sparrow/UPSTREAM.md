# Vendoring record -- sparrow

- **Upstream**: https://github.com/JeroenGar/sparrow
- **Pinned rev**: `6f7b9d353c2b2a4fc68454590a2e8e83475c12ef` (upstream `main`;
  upstream has no tags and no crates.io release, so the pin is a bare branch
  commit and a rebase is a deliberate act, not an automatic one)
- **Date vendored**: 2026-11-06
- **License**: MIT, Copyright (c) 2025 Jeroen Gardeyn, KU Leuven. `LICENSE` is
  kept exactly as upstream ships it.
- **`.git` removed**: this is a snapshot, not a submodule or subtree.
- **jagua-rs**: untouched. sparrow consumes it as an unmodified crates.io
  dependency (`jagua-rs = { version = "=1.1.0", features = ["spp"] }`), which
  is the same version `nest_kernel` depends on directly, so the two unify to
  one `jagua-rs` in the build graph. Patching jagua-rs would pull its MPL-2.0
  file-level copyleft into the picture; patching only sparrow keeps the whole
  fork MIT.

## Why a fork at all

FuseNest nests onto **sheets**, sparrow solves an **infinite strip**. The bridge
is to lay the sheets end to end into one synthetic strip and drop an immovable
full-height rectangle into each inter-sheet gap, so no part can straddle a
boundary. Upstream cannot express that: every item is movable, and the width
objective measures the physical strip, so compression either crushes the
separators or stalls against them.

## What was trimmed from the snapshot

`data/` held 20 MB of benchmark instances, a demo GIF and the project wordmark.
Only the three instances upstream's own integration test drives (`swim.json`,
`shirts.json`, `trousers.json`) are kept, so `cargo test` still runs here --
that suite is the guard that an empty locked set reproduces upstream behaviour.
`README.md` therefore references two images that are no longer present.

## Patches

### Locked items + movables-only width

One coherent patch. Locked items are identified by a set of item *indices*
threaded as `Arc<HashSet<usize>>`. They are ordinary hazards -- movables must
separate from them -- but are never moved, swapped, shifted or disrupted, and
they contribute nothing to the width objective: `Separator::virtual_width`, a
movables-only right edge, drives exploration, compression, sampling bounds,
exterior loss and the reported solution width, while the jagua strip, container
and CDE stay frozen at their warm-start extents.

**When the locked set is empty, every one of these paths falls back to the
physical strip width and upstream behaviour is reproduced exactly** -- which is
what keeps the standalone binary, the bench harness and the integration tests
meaningful.

Every touched file, one line each:

- `src/optimizer/mod.rs` -- `optimize()` gains `locked: Arc<HashSet<usize>>`,
  forwarded to both phase `Separator`s; compression is seeded with the final
  exploration movable extent; `strip_width_lower_bound` sums area over movable
  items only (locked separator area would otherwise inflate the bound and stop
  compression early).
- `src/optimizer/separator.rs` -- `Separator` carries `locked` +
  `virtual_width`; `new` takes the locked set and hands it to every worker;
  `change_strip_width` shifts only movable items and, when locked items exist,
  never resizes the jagua strip.
- `src/optimizer/worker.rs` -- `move_items` never selects a locked item as a
  move candidate; searches use the virtual width as their container bound.
- `src/optimizer/explore.rs` -- disruption guards on the *movable* count and
  excludes locked items from the "large item" cutoff, from swap selection and
  from "practically contained" drag-along; exploration snaps the virtual width
  down to the rightmost movable before the multiplicative shrink, so empty
  trailing sheets collapse in one step instead of many 2% rounds.
- `src/optimizer/compress.rs` -- compression targets and split sampling are
  based on the movable virtual width.
- `src/quantify/tracker.rs` -- locked items are excluded from total loss and
  from locked-vs-locked and locked-vs-exterior loss; a movable's overhang past
  `virtual_width` is counted as exterior loss.
- `src/eval/collision_loss.rs` -- exterior loss is measured against the tracker's
  loss bbox rather than the container's. The CDE raises no `Exterior` hazard for
  an item past the virtual width but still inside the physical container, so that
  overhang is charged up front in `reload`; the `Exterior` branch then adds
  nothing further, because both are the same container loss against the same bbox
  and must be a maximum, not a sum.
- `src/eval/sep_evaluator.rs` -- a sample dominated by that charge alone is
  rejected without searching, and a sample clear of every hazard is still a
  collision if it carries one.
- `src/sample/search.rs` -- uniform sampling takes the virtual right edge, so the
  search does not spend evaluations out in the empty sheets.
- `src/util/assertions.rs` -- `tracker_matches_layout` learns the same three
  rules, so the debug assertions keep their teeth in movable-width mode instead
  of being switched off.
- `src/optimizer/lbf.rs` -- the construction search is handed the physical strip
  width: there is no virtual edge yet while the initial solution is being built.
- `src/cli.rs`, `src/bench.rs`, `src/tui/runtime.rs`, `tests/tests.rs` -- pass an
  empty locked set.

Two patches listed in the plan turned out to be unnecessary on this rev, because
upstream had already made them between nestfox's snapshot and this one:

- `LBFBuilder::place_item`'s runaway strip growth is already a bounded loop
  returning `ConstructionError`, not an `assert!`.
- `SeparationEvaluator` needs no locked-ness of its own: the virtual width lives
  on the `CollisionTracker` it already borrows.
