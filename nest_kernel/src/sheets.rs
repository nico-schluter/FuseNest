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
//! constraint. Sheets inherit that: the caller says how many sheets to allocate
//! and the solver reports how many it needed. There is no trailing separator
//! after the last sheet, so parts that do not fit in the allocation end up in
//! open space past it, where [`SheetStrip::classify`] reports them as overflow
//! rather than quietly pretending they are on a sheet.
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
    /// How many sheets to allocate. The solve reports how many it used.
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
    /// `Ok(None)` means no sheets were asked for, which leaves the solve exactly
    /// as it was before sheets existed. `Err` means sheets were asked for and
    /// cannot be had -- reported rather than quietly falling back to the
    /// unbounded strip, because a caller who asked to nest onto sheets and got
    /// a strip instead has been given the wrong answer, not a lesser one.
    ///
    /// The separator is ONE item with demand `count - 1`, not `count - 1` items:
    /// every gap is the same rectangle, and a locked item index locks all of its
    /// placements, so one item is both cheaper and exactly as expressive.
    pub fn prepare(ext: &mut ExtSPInstance, spec: &SheetSpec) -> Result<Option<Self>, String> {
        if spec.max_sheets < 1 || spec.width <= 0.0 {
            return Ok(None);
        }
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
            count: spec.max_sheets,
            height,
            separation,
            separator_idx: None,
        };
        if spec.max_sheets < 2 {
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
            demand: u64::from(spec.max_sheets - 1),
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
