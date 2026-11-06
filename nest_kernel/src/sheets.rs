//! Sheet nesting on top of a strip packer.
//!
//! sparrow solves an INFINITE STRIP: fixed height, width free to minimise.
//! FuseNest nests onto SHEETS. The bridge is to lay the sheets end to end into
//! one synthetic strip and drop an immovable rectangle -- a **separator** -- into
//! each inter-sheet gap. No part can straddle a sheet boundary, because the
//! separator is in the way, and the fork's movables-only width objective
//! (`vendor/sparrow/UPSTREAM.md`) means the separators out in the empty sheets
//! neither move nor hold that objective open.
//!
//! Sheets here are strictly rectangular, all the same size, and have no holes,
//! so the separators are the only locked items there are.
//!
//! # Coordinates
//!
//! One sheet of width `W`, a gap of `G` between neighbours, so sheet `k` starts
//! at `k * pitch` where `pitch = W + G`. The gap is not waste: it is empty model
//! space, and it is what makes a nest of several sheets legible when the parts
//! are moved to where the solver put them.
//!
//! jagua-rs implements `min_item_separation` by inflating every item's shape by
//! `s/2` and deflating the container by `s/2`, so two inflated shapes that do not
//! overlap are at least `s` apart. That gives this layer a pleasant invariant:
//!
//! **a separator whose ring is exactly the gap rectangle puts sheet `k` at
//! exactly `[k * pitch, k * pitch + W]` in inflated-shape coordinates.**
//!
//! A part flush against a separator has its inflated edge on the sheet boundary
//! and its real edge `s/2` inside it -- the same clearance it gets from the
//! strip's own outer edges. So assigning a part to a sheet is a division, and
//! checking it fits on that sheet is a comparison, both on the inflated bbox
//! that `PlacedItem::shape` already carries.
//!
//! # Sheet count is an output
//!
//! A strip packing problem places EVERY item; its width is the answer, not a
//! constraint. Sheets inherit that: the solver reports how many sheets it
//! needed. There is no trailing separator after the last sheet, so parts that do
//! not fit in the allocation end up in open space past it, where
//! [`SheetStrip::classify`] reports them as overflow rather than quietly
//! pretending they are on a sheet.
//!
//! So the allocation is not a limit anyone should be asked to set: an allocation
//! that binds turns the question from "how little strip does this need" into
//! "which parts fit in this many bins", which is a different optimisation
//! problem and not the one sparrow is good at. The caller therefore normally
//! asks for no particular count and [`SheetSpec::allocation`] picks one with
//! room to spill -- see there for what pulls the number in both directions.
//!
//! A SINGLE sheet therefore needs no separator at all: with no neighbour there
//! is no boundary to defend, and the solve is the plain strip packing it always
//! was -- minimising a width that may or may not come in under the sheet's. What
//! the sheet still contributes is the verdict, because `classify` reports what
//! overran the one sheet instead of leaving the caller to compare widths by eye.
//! The locked machinery is then entirely out of the picture: an empty locked set
//! takes the upstream branch at every point in the fork.

use std::collections::HashSet;
use std::sync::Arc;

use rand::rngs::Xoshiro256PlusPlus;
use sparrow::consts::LBF_SAMPLE_CONFIG;
use sparrow::optimizer::lbf::LBFBuilder;

use jagua_rs::geometry::DTransformation;
use jagua_rs::geometry::primitives::Rect;
use jagua_rs::io::import::ext_to_int_transformation;
use jagua_rs::probs::spp::entities::{SPInstance, SPPlacement, SPProblem, SPSolution};
use jagua_rs::io::ext_repr::ExtShape;
use jagua_rs::probs::spp::io::ext_repr::{ExtItem, ExtSPInstance};

/// External item id for the separator. Far above any profile index the add-in
/// writes, so a placement can be told apart from a part's by its id alone.
pub const SEPARATOR_ITEM_ID: u64 = 1 << 40;

/// Slack when deciding which sheet a part is on, in input units (millimetres).
///
/// This is load-bearing, not hygiene. A part packed flush against a separator
/// lands EXACTLY on a sheet boundary -- that is the invariant working -- so the
/// comparison is always against a coincidence, and f32 noise from the search
/// falls on whichever side it likes. Both ends need the slack: a hair under the
/// boundary would otherwise put the part on the sheet before, where its far end
/// then looks like an overrun, and a hair over would read as a straddle.
///
/// Relative, because the strip grows with the sheet count and f32's spacing
/// grows with it: at 10 m the slack works out around ten microns, which is well
/// inside a laser's kerf and so cannot turn a real overrun into a false pass.
fn fit_tolerance(x: f32) -> f32 {
    f32::max(1e-4, x.abs() * 1e-6)
}

/// How the caller wants the sheets laid out.
#[derive(Debug, Clone, Copy)]
pub struct SheetSpec {
    /// One sheet's width, in the same units as the instance.
    pub width: f32,
    /// An upper bound on the sheets to allocate, or zero to let
    /// [`SheetSpec::allocation`] derive one. The solve reports how many it used.
    pub max_sheets: u32,
    /// Empty model space between neighbouring sheets. Zero asks for a default.
    pub gap: f32,
}

impl SheetSpec {
    /// A gap wide enough to read as a gap: ten millimetres, or twice the part
    /// spacing if that is larger, since a gap narrower than the spacing between
    /// two parts does not look like a sheet boundary to anyone.
    fn resolved_gap(&self, separation: f32) -> f32 {
        match self.gap {
            g if g > 0.0 => g,
            _ => f32::max(10.0, 2.0 * separation),
        }
    }

    /// How many sheets to allocate when the caller named no count: enough that
    /// the allocation does not bind, and no more.
    ///
    /// Both directions cost something, which is why this is a derived number
    /// rather than a generous constant.
    ///
    /// Allocating too FEW is the worse failure, and not only because parts end
    /// up off the sheets: a binding allocation changes the problem. Fitting as
    /// many parts as possible into a fixed number of bins is not what a strip
    /// packer optimises, so a nest held to too few sheets does worse than one
    /// allowed to spill onto another.
    ///
    /// Allocating too MANY is not free either. The physical strip is the whole
    /// allocation wide, and jagua-rs builds its collision quadtree over the
    /// container at a fixed depth, so a strip twice as wide as the work needs
    /// has cells twice as coarse everywhere the parts actually are.
    ///
    /// The estimate is the FOOTPRINT each part consumes -- its bounding box
    /// grown by the part spacing -- over the sheet area, divided by a shelf
    /// efficiency no reasonable nest falls below, plus one whole sheet of
    /// headroom so that a nest which just misses its last sheet spills onto a
    /// real one.
    ///
    /// Footprint rather than area, and this is where the first version of this
    /// estimate went wrong twice over. The spacing is not free: at the 20 mm
    /// spacing the add-in's test instance uses, a 50 mm part occupies a 70 mm
    /// cell and needs nearly twice its own area in sheet. And the part's own
    /// area is not what a row of parts consumes either: a round-ish part leaves
    /// corners no shelf can use. Counting raw area left a third of that instance
    /// standing off the end of the sheets; counting the spacing ring exactly
    /// still left two parts off it.
    ///
    /// Parts that genuinely interlock -- triangles, Ls -- beat their bounding
    /// boxes, so this over-allocates for them. That costs a wider strip and
    /// nothing else: the spare sheets go unused, which is what the movables-only
    /// objective is for.
    pub fn allocation(&self, ext: &ExtSPInstance) -> u32 {
        // Full sheet area, not deflated by the spacing: by this module's
        // invariant the inflated shapes live in exactly [0, W] x [0, H], so the
        // spacing is already counted once, on the items.
        let sheet_area = self.width * ext.strip_height;
        let separation = ext.min_item_separation.max(0.0);
        let (parts, demand_area) = ext.items.iter().fold((0u64, 0.0f32), |(n, a), item| {
            let area = footprint(&item.base.shape, separation);
            (n + item.demand, a + item.demand as f32 * area)
        });
        if sheet_area <= 0.0 || parts == 0 {
            return 1;
        }
        let estimate = (demand_area / (sheet_area * SHELF_EFFICIENCY_FLOOR)).ceil();
        let estimate = f32::max(estimate, 1.0) as u64 + 1;
        estimate.min(parts).min(u64::from(MAX_ALLOCATION)) as u32
    }
}

/// How much of a sheet the automatic allocation assumes gets filled with part
/// footprints.
///
/// Not a yield figure: footprints already carry the spacing, so what this has to
/// allow for is the waste between rows and at the sheet's far edge, which is what
/// stops a sheet taking a whole number of rows, plus the corners a round-ish part
/// leaves in its own cell. Measured at 0.60 on the add-in's own test instance --
/// 95 compact parts at 20 mm spacing, which came to thirteen 300x200 sheets -- so
/// this is set at that figure rather than above it. The headroom costs a coarser
/// quadtree (see [`SheetSpec::allocation`]) and running short costs a sheet's
/// worth of parts standing off to the side, so neither side wants slack it cannot
/// justify.
const SHELF_EFFICIENCY_FLOOR: f32 = 0.6;

/// Most sheets the automatic allocation will ever ask for.
///
/// Guards the arithmetic against its own inputs: a sheet width mistyped by three
/// orders of magnitude would otherwise allocate separators by the thousand and
/// make the solve look hung rather than wrong.
const MAX_ALLOCATION: u32 = 256;

/// How much sheet one part consumes, for the allocation estimate only: its
/// bounding box grown by the spacing it has to keep from its neighbours.
///
/// The spacing is counted once, not twice, because that is how jagua-rs enforces
/// it -- every shape inflated by `s/2`, so two touching cells are `s` apart --
/// and it is why this module's invariant can put a flush part exactly on a sheet
/// boundary.
///
/// Measured on the shape AS SENT rather than after import, because the
/// separator's demand -- and so the sheet count -- has to be fixed before the
/// instance is imported. jagua-rs may simplify a polygon on import, but only
/// within a tolerance far below the slack the shelf floor already carries.
fn footprint(shape: &ExtShape, separation: f32) -> f32 {
    /// Bounding box extents of a ring.
    fn extents(pts: &[(f32, f32)]) -> (f32, f32) {
        let fold = |f: fn(f32, f32) -> f32, sel: fn(&(f32, f32)) -> f32| {
            pts.iter().map(sel).fold(f32::NAN, f)
        };
        (
            fold(f32::max, |p| p.0) - fold(f32::min, |p| p.0),
            fold(f32::max, |p| p.1) - fold(f32::min, |p| p.1),
        )
    }
    let (w, h) = match shape {
        ExtShape::Rectangle { width, height, .. } => (*width, *height),
        ExtShape::SimplePolygon(p) => extents(&p.0),
        ExtShape::Polygon(p) => extents(&p.outer.0),
        // Disjoint pieces are one item and are placed as one, so their common
        // bounding box is the footprint.
        ExtShape::MultiPolygon(ps) => {
            let rings: Vec<(f32, f32)> = ps.iter().flat_map(|p| p.outer.0.iter().copied()).collect();
            extents(&rings)
        }
    };
    // NaN from an empty ring would poison the sum; such a shape does not survive
    // import anyway, so it contributes nothing here.
    match w.is_finite() && h.is_finite() {
        true => (w + separation) * (h + separation),
        false => 0.0,
    }
}

/// Where a placement ended up.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Placed {
    /// Wholly within this sheet, counting from zero.
    OnSheet(u32),
    /// Past the last allocated sheet, or straddling a boundary. Either way it is
    /// not on a sheet anyone can cut, and the caller has to say so.
    Overflow,
}

/// A strip of sheets, and everything derived from it that the solve needs.
#[derive(Debug, Clone)]
pub struct SheetStrip {
    /// One sheet's width.
    pub width: f32,
    /// Sheet-to-sheet distance: width plus the resolved gap.
    pub pitch: f32,
    /// Sheets allocated.
    pub count: u32,
    /// The strip height, which is the sheet height.
    pub height: f32,
    /// `min_item_separation` from the instance.
    pub separation: f32,
    /// The instance index of the separator item, for the locked set. `None` for a
    /// single sheet, which has no boundary to separate.
    pub separator_idx: Option<usize>,
}

impl SheetStrip {
    /// Describes `spec` as a strip, adding the separator item to `ext` if more
    /// than one sheet was asked for.
    ///
    /// `Ok(None)` means no sheets were asked for -- a width of zero -- which
    /// leaves the solve exactly as it was before sheets existed. `Err` means sheets were asked for and
    /// cannot be had -- reported rather than quietly falling back to the
    /// unbounded strip, because a caller who asked to nest onto sheets and got
    /// a strip instead has been given the wrong answer, not a lesser one.
    ///
    /// The separator is ONE item with demand `count - 1`, not `count - 1` items:
    /// every gap is the same rectangle, and a locked item index locks all of its
    /// placements, so one item is both cheaper and exactly as expressive.
    pub fn prepare(ext: &mut ExtSPInstance, spec: &SheetSpec) -> Result<Option<Self>, String> {
        if spec.width <= 0.0 {
            return Ok(None);
        }
        // Zero asks this layer to choose, which is the normal case: see
        // `SheetSpec::allocation` for why the count is nobody's input.
        let count = match spec.max_sheets {
            0 => spec.allocation(ext),
            asked => asked,
        };
        let separation = ext.min_item_separation.max(0.0);
        let height = ext.strip_height;
        if height <= 2.0 * separation {
            return Err(format!(
                "a {height} sheet leaves no usable height at {separation} part spacing"
            ));
        }
        let gap = spec.resolved_gap(separation);

        let mut strip = Self {
            width: spec.width,
            pitch: spec.width + gap,
            count,
            height,
            separation,
            separator_idx: None,
        };
        if count < 2 {
            return Ok(Some(strip));
        }
        strip.separator_idx = Some(usize::MAX); // resolved after import

        // The ring is the gap rectangle, inset vertically to the band a movable
        // can actually occupy: inflated by `s/2` it becomes exactly the deflated
        // container's height, so the separator seals the gap without poking
        // outside the container and raising an exterior hazard of its own.
        ext.items.push(ExtItem {
            base: jagua_rs::io::ext_repr::ExtItem {
                id: SEPARATOR_ITEM_ID,
                orientation: jagua_rs::io::ext_repr::ExtOrientation {
                    rotation: jagua_rs::io::ext_repr::ExtRotation::Discrete {
                        angles: vec![0.0],
                    },
                    reflection_axes: vec![],
                },
                shape: jagua_rs::io::ext_repr::ExtShape::Rectangle {
                    x_min: 0.0,
                    y_min: 0.0,
                    width: gap,
                    height: height - 2.0 * separation,
                },
                min_quality: None,
            },
            demand: u64::from(count - 1),
        });

        Ok(Some(strip))
    }

    /// Resolves the separator's instance index, which only exists once the
    /// instance has been imported.
    pub fn resolve(mut self, instance: &SPInstance) -> Result<Self, String> {
        if self.separator_idx.is_some() {
            self.separator_idx = Some(
                instance
                    .item_idx(SEPARATOR_ITEM_ID)
                    .ok_or_else(|| "separator item did not survive import".to_string())?,
            );
        }
        Ok(self)
    }

    /// The item indices that must not move: the separator's, or none at all on a
    /// single sheet -- in which case the fork behaves exactly as upstream.
    pub fn locked(&self) -> Arc<HashSet<usize>> {
        Arc::new(self.separator_idx.into_iter().collect())
    }

    /// Whether this strip has anything locked, and so needs a warm start that
    /// pre-places it. A single sheet does not: letting the construction heuristic
    /// build its own initial layout is both simpler and what it already does.
    pub fn has_separators(&self) -> bool {
        self.separator_idx.is_some()
    }

    /// Whether an item index is a separator rather than a part.
    pub fn is_separator(&self, item_idx: usize) -> bool {
        self.separator_idx == Some(item_idx)
    }

    /// The full span of the allocated sheets, including the trailing gap.
    pub fn span(&self) -> f32 {
        self.pitch * self.count as f32
    }

    /// Pre-places every separator and builds the warm-start problem around them.
    ///
    /// The order matters. The strip is widened to the full span first, because
    /// jagua-rs sizes a fresh problem's strip for 100% density and the separators
    /// live far to the right of that. Then the separators go in, and only then
    /// does the construction heuristic run: it places what is still owed, so it
    /// builds the first layout AROUND the separators rather than through them.
    pub fn preplace(&self, prob: &mut SPProblem) -> Result<(), String> {
        prob.change_strip_width(self.span())
            .map_err(|e| format!("widening the strip to the sheet span: {e}"))?;

        let Some(separator_idx) = self.separator_idx else {
            return Ok(());
        };
        let pre_transf = prob.instance().item(separator_idx).shape_orig().pre_transform;
        for k in 0..self.count - 1 {
            let origin = (self.pitch * k as f32 + self.width, self.separation);
            prob.place_item(SPPlacement {
                item_idx: separator_idx,
                d_transf: ext_to_int_transformation(
                    &DTransformation::new(0.0, origin),
                    &pre_transf,
                ),
            });
        }
        Ok(())
    }

    /// The initial layout a sheet solve starts from: every separator at its fixed
    /// pose, and the construction heuristic's first attempt at the parts around them.
    ///
    /// The physical strip is restored to the full sheet span at the end, because
    /// `construct` finishes with `fit_strip`, which shrinks it to the content it
    /// happened to place. Leaving it shrunk would cap the search below the sheets the
    /// caller allocated. It is never narrowed: if construction had to grow past the
    /// span -- more parts than the sheets can hold -- that extra space is where
    /// overflow lives, and the objective will try to pull it back in.
    pub fn warm_start(
        &self,
        instance: &SPInstance,
        rng: Xoshiro256PlusPlus,
    ) -> Result<SPSolution, String> {
        let mut builder = LBFBuilder::new(instance.clone(), rng, LBF_SAMPLE_CONFIG)
            .map_err(|e| format!("initial layout: {e}"))?;
        self.preplace(&mut builder.prob)?;
        let mut prob = builder
            .construct()
            .map_err(|e| format!("initial layout: {e}"))?
            .prob;
        let width = self.span().max(prob.strip_width());
        prob.change_strip_width(width)
            .map_err(|e| format!("restoring the sheet span: {e}"))?;
        Ok(prob.save())
    }

    /// Which sheet an inflated-shape bounding box sits on.
    ///
    /// Division, because of the invariant in this module's header: sheet `k` is
    /// exactly `[k * pitch, k * pitch + W]` once shapes are inflated by `s/2`.
    /// Both comparisons carry [`fit_tolerance`], because a part flush against a
    /// separator sits exactly on a boundary and the test is therefore always
    /// against a coincidence.
    pub fn classify(&self, bbox: Rect) -> Placed {
        let tol = fit_tolerance(bbox.x_max);
        if bbox.x_min < -tol {
            return Placed::Overflow;
        }
        // Nudged before the division: a part flush to a separator's right face
        // can land a hair BELOW the boundary, which would otherwise name the
        // sheet before it and then read the part as overrunning that sheet.
        let k = ((bbox.x_min + tol) / self.pitch).max(0.0) as u32;
        if k >= self.count {
            return Placed::Overflow;
        }
        let left = self.pitch * k as f32;
        match bbox.x_min >= left - tol && bbox.x_max <= left + self.width + tol {
            true => Placed::OnSheet(k),
            // Either reaching into the gap past the last sheet, or -- if a
            // separator were ever missing -- straddling a boundary.
            false => Placed::Overflow,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec() -> SheetSpec {
        SheetSpec { width: 100.0, max_sheets: 3, gap: 10.0 }
    }

    fn strip() -> SheetStrip {
        SheetStrip {
            width: 100.0,
            pitch: 110.0,
            count: 3,
            height: 50.0,
            separation: 2.0,
            separator_idx: Some(0),
        }
    }

    #[test]
    fn a_single_sheet_needs_no_separator() {
        let mut ext = ExtSPInstance {
            name: "t".into(),
            min_item_separation: 2.0,
            items: vec![],
            strip_height: 50.0,
        };
        let mut one = spec();
        one.max_sheets = 1;
        let strip = SheetStrip::prepare(&mut ext, &one)
            .expect("a lone sheet is possible")
            .expect("one sheet is still a sheet");
        assert!(!strip.has_separators(), "a lone sheet has no boundary to defend");
        assert!(ext.items.is_empty(), "and so adds nothing to the instance");
    }

    /// `n` parts of `w` x `h`, as the add-in would send them.
    fn parts(n: u64, w: f32, h: f32, strip_height: f32) -> ExtSPInstance {
        ExtSPInstance {
            name: "t".into(),
            min_item_separation: 2.0,
            items: (0..n)
                .map(|i| ExtItem {
                    base: jagua_rs::io::ext_repr::ExtItem {
                        id: i,
                        orientation: jagua_rs::io::ext_repr::ExtOrientation {
                            rotation: jagua_rs::io::ext_repr::ExtRotation::Discrete {
                                angles: vec![0.0],
                            },
                            reflection_axes: vec![],
                        },
                        shape: ExtShape::Rectangle { x_min: 0.0, y_min: 0.0, width: w, height: h },
                        min_quality: None,
                    },
                    demand: 1,
                })
                .collect(),
            strip_height,
        }
    }

    /// The point of the automatic allocation: there is always somewhere to
    /// spill, so the count never becomes the constraint the solver is working
    /// against. Twelve 100x100 parts come to three of these sheets at the assumed
    /// shelf efficiency, and get a fourth to spill onto.
    #[test]
    fn an_unasked_count_allocates_room_to_spill() {
        let ext = parts(12, 100.0, 100.0, 100.0);
        let spec = SheetSpec { width: 1000.0, max_sheets: 0, gap: 10.0 };
        assert_eq!(spec.allocation(&ext), 4);
    }

    /// The estimate that was wrong first time round. At 20 mm spacing a 50 mm
    /// part occupies a 70 mm cell, so it needs very nearly twice its own area in
    /// sheet -- and an allocation that counted raw area left a third of the set
    /// standing off the end.
    #[test]
    fn the_spacing_ring_counts_towards_the_sheets() {
        let mut ext = parts(96, 50.0, 50.0, 200.0);
        ext.min_item_separation = 20.0;
        let spec = SheetSpec { width: 300.0, max_sheets: 0, gap: 0.0 };

        // Eight 70 mm cells fit on a 300x200 sheet, so the set needs twelve.
        let allocated = spec.allocation(&ext);
        assert!(
            (13..=18).contains(&allocated),
            "a twelve-sheet set should allocate a little over twelve, not {allocated}"
        );
    }

    /// No packing here does worse than one part per sheet, so allocating past
    /// that is strip nobody can use -- and a wider strip is a coarser quadtree.
    #[test]
    fn the_allocation_is_never_more_sheets_than_parts() {
        let ext = parts(2, 10.0, 10.0, 1000.0);
        let spec = SheetSpec { width: 1000.0, max_sheets: 0, gap: 10.0 };
        assert_eq!(spec.allocation(&ext), 2);
    }

    /// A count the caller does name is taken as given: the probe and the tests
    /// pin a count on purpose, and a bound asked for is not an estimate.
    #[test]
    fn an_asked_count_is_taken_as_asked() {
        let mut ext = parts(12, 100.0, 100.0, 100.0);
        let strip = SheetStrip::prepare(&mut ext, &spec())
            .expect("three sheets are possible")
            .expect("three sheets");
        assert_eq!(strip.count, 3);
    }

    #[test]
    fn separator_count_is_one_fewer_than_the_sheets() {
        let mut ext = ExtSPInstance {
            name: "t".into(),
            min_item_separation: 2.0,
            items: vec![],
            strip_height: 50.0,
        };
        let strip = SheetStrip::prepare(&mut ext, &spec())
            .expect("three sheets are possible")
            .expect("three sheets");
        assert!(strip.has_separators());
        assert_eq!(ext.items.len(), 1);
        assert_eq!(ext.items[0].demand, 2);
        assert_eq!(ext.items[0].base.id, SEPARATOR_ITEM_ID);
    }

    #[test]
    fn a_part_is_on_the_sheet_whose_span_contains_it() {
        let s = strip();
        assert_eq!(s.classify(Rect::try_new(1.0, 1.0, 99.0, 49.0).unwrap()), Placed::OnSheet(0));
        assert_eq!(s.classify(Rect::try_new(111.0, 1.0, 209.0, 49.0).unwrap()), Placed::OnSheet(1));
        assert_eq!(s.classify(Rect::try_new(221.0, 1.0, 319.0, 49.0).unwrap()), Placed::OnSheet(2));
    }

    /// The boundary case the whole scheme turns on: flush against a separator is
    /// ON the sheet, a hair past it is not.
    #[test]
    fn flush_against_a_separator_is_still_on_the_sheet() {
        let s = strip();
        assert_eq!(s.classify(Rect::try_new(50.0, 1.0, 100.0, 49.0).unwrap()), Placed::OnSheet(0));
        assert_eq!(s.classify(Rect::try_new(50.0, 1.0, 100.5, 49.0).unwrap()), Placed::Overflow);
    }

    /// The same coincidence from the other side, and the one that actually bit:
    /// a part flush against a separator's RIGHT face sits exactly on the start of
    /// the next sheet, so f32 noise can drop it a hair below. Taking the sheet
    /// index from an un-nudged division then names the sheet BEFORE it, whose
    /// right edge the part is of course nowhere near -- and a perfectly good
    /// placement is reported as overflow.
    #[test]
    fn a_hair_short_of_a_boundary_is_on_the_sheet_that_starts_there() {
        let s = strip();
        let eps = 1e-5;
        assert_eq!(
            s.classify(Rect::try_new(110.0 - eps, 1.0, 150.0, 49.0).unwrap()),
            Placed::OnSheet(1)
        );
        assert_eq!(
            s.classify(Rect::try_new(220.0 - eps, 1.0, 260.0, 49.0).unwrap()),
            Placed::OnSheet(2)
        );
    }

    /// The slack must not be so generous that it swallows a real overrun. A
    /// tenth of a millimetre past the edge is a part hanging off the sheet.
    #[test]
    fn the_slack_does_not_excuse_a_real_overrun() {
        let s = strip();
        assert_eq!(s.classify(Rect::try_new(50.0, 1.0, 100.1, 49.0).unwrap()), Placed::Overflow);
        assert_eq!(s.classify(Rect::try_new(109.9, 1.0, 150.0, 49.0).unwrap()), Placed::Overflow);
    }

    #[test]
    fn past_the_last_sheet_is_overflow() {
        let s = strip();
        // In the trailing gap, which is open space: no separator follows sheet 2.
        assert_eq!(s.classify(Rect::try_new(305.0, 1.0, 325.0, 49.0).unwrap()), Placed::Overflow);
        // Beyond the allocation entirely.
        assert_eq!(s.classify(Rect::try_new(340.0, 1.0, 380.0, 49.0).unwrap()), Placed::Overflow);
    }
}
