use crate::consts::{GLS_WEIGHT_DECAY, GLS_WEIGHT_MAX_INC_RATIO, GLS_WEIGHT_MIN_INC_RATIO};
use crate::quantify::pair_matrix::PairMatrix;
use crate::quantify::{quantify_collision_poly_container, quantify_collision_poly_poly};
use crate::util::assertions::tracker_matches_layout;
use jagua_rs::collision_detection::hazards::collector::{BasicHazardCollector, HazardCollector};
use jagua_rs::collision_detection::hazards::BasicHazardEntity;
use jagua_rs::collision_detection::hazards::filter::NoFilter;
use jagua_rs::entities::{Layout, PItemKey};
use ordered_float::Float;
use slotmap::SecondaryMap;
use std::collections::HashSet;
use std::sync::Arc;

/// Tracker of both collisions between pair of items and collisions with the container.
/// It also stores the weights for every pair of hazards and is used as a cache for collisions.
#[derive(Debug, Clone)]
pub struct CollisionTracker {
    pub size: usize,
    pub pk_idx_map: SecondaryMap<PItemKey, usize>,
    pub pair_collisions: PairMatrix,
    pub container_collisions: Vec<CTEntry>,
    /// FuseNest fork: locked item indices (see `Separator::locked`). Empty unless
    /// the tracker was built by [`CollisionTracker::new_movable_width`].
    locked: Arc<HashSet<usize>>,
    /// FuseNest fork: locked-ness per tracker slot. `pk_idx_map` is the only link
    /// from a key to a slot, and an item's index is not recoverable from a slot
    /// without the layout, so the flags are cached here when the tracker is built.
    /// A move keeps the same item in the same slot, so they stay valid.
    locked_indices: Vec<bool>,
    /// FuseNest fork: `Some` puts the tracker in MOVABLE-WIDTH mode -- quantify
    /// against this right edge instead of the container's, and hold locked items
    /// loss-exempt. `None` is upstream behaviour.
    virtual_width: Option<f32>,
}

pub type CTSnapshot = CollisionTracker;

impl CollisionTracker {
    pub fn new(l: &Layout) -> Self {
        let size = l.placed_items().len();

        // Create the tracker
        let mut ot = Self {
            size,
            pk_idx_map: l.placed_items().keys().enumerate()
                .map(|(i, pk)| (pk, i))
                .collect(),
            pair_collisions: PairMatrix::new(size),
            container_collisions: vec![CTEntry { weight: 1.0, loss: 0.0 }; size],
            locked: Arc::new(HashSet::new()),
            locked_indices: vec![false; size],
            virtual_width: None,
        };

        // Recompute the loss for all items
        l.placed_items().keys().for_each(|pk| {
            ot.recompute_loss_for_item(pk, l)
        });

        debug_assert!(tracker_matches_layout(&ot, l));

        ot
    }

    /// FuseNest fork: a tracker that quantifies against `virtual_width` rather than
    /// the container's right edge, and treats `locked` items as immovable scenery.
    ///
    /// Three consequences, all of them deliberate:
    ///
    ///  * A locked item carries no exterior loss, so a separator laid flush against
    ///    the container edge is not permanently "colliding" with it.
    ///  * Two locked items carry no pair loss with each other, so overlapping
    ///    scenery is not a collision anyone can resolve.
    ///  * A MOVABLE item reaching past `virtual_width` carries exterior loss even
    ///    though it is still physically inside the container. That is what makes
    ///    the virtual edge an objective rather than a label.
    pub fn new_movable_width(l: &Layout, locked: Arc<HashSet<usize>>, virtual_width: f32) -> Self {
        let size = l.placed_items().len();

        let mut ot = Self {
            size,
            pk_idx_map: l.placed_items().keys().enumerate()
                .map(|(i, pk)| (pk, i))
                .collect(),
            pair_collisions: PairMatrix::new(size),
            container_collisions: vec![CTEntry { weight: 1.0, loss: 0.0 }; size],
            locked,
            locked_indices: vec![false; size],
            virtual_width: Some(virtual_width),
        };

        for (pk, pi) in l.placed_items().iter() {
            ot.locked_indices[ot.pk_idx_map[pk]] = ot.locked.contains(&pi.item().idx());
        }

        l.placed_items().keys().for_each(|pk| {
            ot.recompute_loss_for_item(pk, l)
        });

        debug_assert!(tracker_matches_layout(&ot, l));

        ot
    }

    /// The right edge this tracker quantifies against, if it is not the container's.
    pub fn virtual_width(&self) -> Option<f32> {
        self.virtual_width
    }

    /// Whether an item index is locked. Always false in upstream mode.
    pub fn is_locked(&self, item_idx: usize) -> bool {
        self.virtual_width.is_some() && self.locked.contains(&item_idx)
    }

    /// The bbox this tracker measures exterior loss against: the container's, with
    /// its right edge pulled in to the virtual width in movable-width mode.
    pub fn loss_bbox(&self, l: &Layout) -> jagua_rs::geometry::primitives::Rect {
        let mut bbox = l.container().outer_cd().bbox();
        if let Some(virtual_width) = self.virtual_width {
            bbox.x_max = virtual_width;
        }
        bbox
    }

    fn recompute_loss_for_item(&mut self, pk: PItemKey, l: &Layout) {
        let idx = self.pk_idx_map[pk];
        let pi = &l.placed_items()[pk];
        let shape = pi.shape();

        // Reset all current loss values for the item
        for i in 0..self.size {
            self.pair_collisions[(idx, i)].loss = 0.0;
        }
        self.container_collisions[idx].loss = 0.0;

        // Compute which hazards are currently colliding with the item
        let mut collector = BasicHazardCollector::with_capacity(l.placed_items().len() + 1);
        l.cde().collect_poly_collisions(shape, &NoFilter, &mut collector);
        // Remove the item itself from the detector
        collector.remove_by_entity(&BasicHazardEntity::from((pk, pi)));

        // For each colliding hazard, quantify the collision and store it in the tracker
        let self_locked = self.is_locked(pi.item().idx());
        for (_, haz) in collector.iter() {
            match haz {
                BasicHazardEntity::PlacedItem { pk: other_pk, .. } => {
                    let other = &l.placed_items()[*other_pk];
                    // Scenery overlapping scenery is nobody's collision to resolve.
                    if self_locked && self.is_locked(other.item().idx()) {
                        continue;
                    }
                    let shape_other = other.shape();
                    let idx_other = self.pk_idx_map[*other_pk];

                    let loss = quantify_collision_poly_poly(shape, shape_other);
                    assert!(loss > 0.0, "loss for a collision should be > 0.0");
                    self.pair_collisions[(idx, idx_other)].loss = loss;
                }
                BasicHazardEntity::Exterior => {
                    if self_locked {
                        continue;
                    }
                    let loss = quantify_collision_poly_container(shape, self.loss_bbox(l));
                    assert!(loss > 0.0, "loss for a collision should be > 0.0");
                    self.container_collisions[idx].loss = loss;
                }
                _ => unimplemented!("unsupported hazard entity"),
            }
        }

        // The CDE raises no `Exterior` hazard for an item past the virtual width but
        // still inside the physical container, so that overhang has to be charged
        // here rather than read off a hazard.
        if let Some(virtual_width) = self.virtual_width {
            if !self_locked && shape.bbox().x_max > virtual_width {
                let loss = quantify_collision_poly_container(shape, self.loss_bbox(l));
                assert!(loss > 0.0, "loss for a virtual-width overhang should be > 0.0");
                self.container_collisions[idx].loss = self.container_collisions[idx].loss.max(loss);
            }
        }
    }

    pub fn restore_but_keep_weights(&mut self, cts: &CTSnapshot, layout: &Layout) {
        //Copy the loss and keys, but keep the weights
        self.pk_idx_map = cts.pk_idx_map.clone();
        //The locked flags are keyed by slot, so they travel with the key mapping.
        self.locked_indices.clone_from(&cts.locked_indices);
        self.pair_collisions.data.iter_mut()
            .zip(cts.pair_collisions.data.iter())
            .for_each(|(a, b)| a.loss = b.loss);
        self.container_collisions.iter_mut()
            .zip(cts.container_collisions.iter())
            .for_each(|(a, b)| a.loss = b.loss);
        debug_assert!(tracker_matches_layout(self, layout));
    }

    pub fn save(&self) -> CTSnapshot {
        self.clone()
    }

    pub fn register_item_move(&mut self, l: &Layout, old_pk: PItemKey, new_pk: PItemKey) {
        //swap the keys in the pk_idx_map
        let idx = self.pk_idx_map.remove(old_pk).unwrap();
        self.pk_idx_map.insert(new_pk, idx);

        self.recompute_loss_for_item(new_pk, l);

        debug_assert!(tracker_matches_layout(self, l));
    }


    /// Algorithm 8 from https://doi.org/10.48550/arXiv.2509.13329
    pub fn update_weights(&mut self) {
        // Find the maximum loss across all entries
        let max_loss = self.pair_collisions.data.iter()
            .chain(self.container_collisions.iter())
            .map(|e| e.loss)
            .fold(0.0, |a, b| a.max(b));

        // Go over all entries (pairs) and modify their weights.
        for e in self.pair_collisions.data.iter_mut()
            .chain(self.container_collisions.iter_mut()) {
            let multiplier = match e.loss == 0.0 {
                true => {
                    // No collision at the moment, slowly decay the weight back to 1.0
                    GLS_WEIGHT_DECAY
                },
                false => {
                    // Collision detected, increase the weight based on 'how bad' the collision is relative to the worst collision
                    GLS_WEIGHT_MIN_INC_RATIO + (GLS_WEIGHT_MAX_INC_RATIO - GLS_WEIGHT_MIN_INC_RATIO) * (e.loss / max_loss)
                },
            };
            e.weight = (e.weight * multiplier).max(1.0);
        }
    }

    pub fn get_pair_weight(&self, pk1: PItemKey, pk2: PItemKey) -> f32 {
        let (idx1, idx2) = (self.pk_idx_map[pk1], self.pk_idx_map[pk2]);
        self.pair_collisions[(idx1, idx2)].weight
    }

    pub fn get_container_weight(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        self.container_collisions[idx].weight
    }

    /// Algorithm 1 from https://doi.org/10.48550/arXiv.2509.13329
    /// Evaluations between item pairs are stored in this data-structure for quick and easy retrieval.
    pub fn get_pair_loss(&self, pk1: PItemKey, pk2: PItemKey) -> f32 {
        let (idx1, idx2) = (self.pk_idx_map[pk1], self.pk_idx_map[pk2]);
        self.pair_collisions[(idx1, idx2)].loss
    }

    pub fn get_container_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];
        self.container_collisions[idx].loss
    }

    pub fn get_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];

        let pair_loss = (0..self.size)
            .map(|i| self.pair_collisions[(idx, i)].loss)
            .sum::<f32>();

        self.container_collisions[idx].loss + pair_loss
    }

    pub fn get_weighted_loss(&self, pk: PItemKey) -> f32 {
        let idx = self.pk_idx_map[pk];

        let w_pair_loss = (0..self.size)
            .map(|i| self.pair_collisions[(idx, i)].weighted_loss())
            .sum::<f32>();

        self.container_collisions[idx].weighted_loss() + w_pair_loss
    }

    pub fn get_total_loss(&self) -> f32 {
        if self.virtual_width.is_some() {
            // Sum per-MOVABLE-item loss rather than over the triangular matrix, so
            // that locked items contribute nothing of their own. The scale differs
            // from upstream's -- a movable<->movable pair is counted from both ends
            // here, a movable<->locked pair only from the movable's -- which gives
            // collisions with scenery half the GLS emphasis. Deliberate, and the
            // only property the phases rely on is preserved: zero still means every
            // movable is separated from everything and inside the virtual width.
            return self.pk_idx_map.iter()
                .filter(|(_, idx)| !self.locked_indices[**idx])
                .map(|(pk, _)| self.get_loss(pk))
                .sum();
        }
        let cont_o = self.container_collisions.iter().map(|e| e.loss).sum::<f32>();

        let pair_o = self.pair_collisions.data.iter()
            .map(|e| e.loss)
            .sum::<f32>();

        cont_o + pair_o
    }

    pub fn get_total_weighted_loss(&self) -> f32 {
        let cont_w_o = self.container_collisions.iter()
            .map(|e| e.weighted_loss())
            .sum::<f32>();

        let pair_w_o = self.pair_collisions.data.iter()
            .map(|e| e.weighted_loss())
            .sum::<f32>();

        cont_w_o + pair_w_o
    }
}

#[derive(Debug, Clone, Copy)]
pub struct CTEntry {
    pub loss: f32,
    pub weight: f32,
}

impl CTEntry {
    pub fn weighted_loss(&self) -> f32 {
        self.weight * self.loss
    }
}
