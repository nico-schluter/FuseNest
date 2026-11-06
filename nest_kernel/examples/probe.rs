//! Terminal probe: import an instance JSON and solve it, printing what
//! jagua-rs made of each item on the way through.
//!
//!     cargo run --release --example probe -- instance.json [ms] [WxN+G] > solution.json
//!
//! Exists to answer what the add-in cannot be asked from a terminal: whether a
//! given polygon survives `import_instance`, and what shape it is afterwards
//! (jagua-rs simplifies on import, so the item the solver places is not
//! necessarily the ring we sent).
//!
//! The third argument switches on sheet nesting: `1000x4+10` is four sheets
//! 1000 units wide with a 10-unit gap between them (`+G` optional). Each placed
//! part is then reported with the sheet it landed on, and anything that did not
//! fit on an allocated sheet is called out as overflow -- which is the one thing
//! about sheet nesting that cannot be checked by eye from a flat solution JSON.

use jagua_rs::io::import::Importer;
use jagua_rs::probs::spp::io::ext_repr::ExtSPInstance;
use nest_kernel::{Placed, SheetSpec, SheetStrip};
use rand::SeedableRng;
use rand::rngs::Xoshiro256PlusPlus;
use sparrow::config::{DEFAULT_SPARROW_CONFIG, ShrinkDecayStrategy};
use sparrow::optimizer::optimize;
use sparrow::util::listener::{ReportType, SolutionListener};
use sparrow::util::terminator::Terminator;
use std::collections::HashSet;
use std::sync::Arc;
use std::time::Duration;

/// sparrow narrates every shrink, every failed separation and every pool
/// rollback through `log`. The add-in captures those lines into a ring buffer;
/// from a terminal they may as well go to stderr, and without them a solve that
/// converges badly is indistinguishable from one that converges well.
struct Stderr;
impl log::Log for Stderr {
    fn enabled(&self, _: &log::Metadata) -> bool {
        true
    }
    fn log(&self, record: &log::Record) {
        eprintln!("{}", record.args());
    }
    fn flush(&self) {}
}
static STDERR: Stderr = Stderr;

struct Quiet;
impl SolutionListener for Quiet {
    fn report(&mut self, r: ReportType, s: &jagua_rs::probs::spp::entities::SPSolution) {
        eprintln!(
            "[SOL] {:?}: width {:.3}, density {:.2}%",
            r,
            s.strip_width(),
            s.density() * 100.0
        );
    }
}

struct Clock {
    timeout: Option<jagua_rs::Instant>,
}
impl Terminator for Clock {
    fn kill(&self) -> bool {
        self.timeout
            .is_some_and(|t| jagua_rs::Instant::now() > t)
    }
    fn new_timeout(&mut self, timeout: Duration) {
        self.timeout = Some(jagua_rs::Instant::now() + timeout);
    }
    fn timeout_at(&self) -> Option<jagua_rs::Instant> {
        self.timeout
    }
}

/// `WxN` or `WxN+G`: sheet width, sheet count, optional gap.
fn parse_sheets(arg: &str) -> SheetSpec {
    let (dims, gap) = match arg.split_once('+') {
        Some((d, g)) => (d, g.parse().expect("sheet gap")),
        None => (arg, 0.0),
    };
    let (width, count) = dims.split_once('x').expect("sheets look like WxN or WxN+G");
    SheetSpec {
        width: width.parse().expect("sheet width"),
        max_sheets: count.parse().expect("sheet count"),
        gap,
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).expect("usage: probe <instance.json> [ms] [WxN+G]");
    let ms: u64 = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(3000);
    let spec = args.get(3).map(|s| parse_sheets(s));

    log::set_logger(&STDERR).expect("install logger");
    log::set_max_level(log::LevelFilter::Info);

    let json = std::fs::read_to_string(path).expect("read instance");
    let mut ext: ExtSPInstance = serde_json::from_str(&json).expect("parse instance");

    eprintln!("--- as sent ---");
    for it in &ext.items {
        eprintln!("  item {:<3} demand {}", it.base.id, it.demand);
    }

    let mut cfg = DEFAULT_SPARROW_CONFIG;
    let total = Duration::from_millis(ms.max(200));
    cfg.expl_cfg.time_limit = total.mul_f32(0.75);
    cfg.cmpr_cfg.time_limit = total.mul_f32(0.25);
    cfg.cmpr_cfg.shrink_decay = ShrinkDecayStrategy::FailureBased(0.5);

    let sheets = spec.and_then(|spec| SheetStrip::prepare(&mut ext, &spec).expect("sheet spec"));
    if let Some(s) = &sheets {
        eprintln!(
            "--- sheets: {} x {:.3} wide, pitch {:.3}, span {:.3} ---",
            s.count,
            s.width,
            s.pitch,
            s.span()
        );
    }

    let importer = Importer::new(
        cfg.cde_config,
        cfg.poly_simpl_tolerance,
        cfg.narrow_concavity_cutoff_ratio,
    );

    eprintln!("--- import ---");
    let instance = match jagua_rs::probs::spp::io::import_instance(&importer, &ext) {
        Ok(i) => i,
        Err(e) => {
            eprintln!("REJECTED: {e}");
            std::process::exit(3);
        }
    };
    eprintln!("ACCEPTED");

    for (item, qty) in instance.items.iter() {
        let sp = item.shape_cd();
        eprintln!(
            "  item {:<3} qty {:<3} {:>4} verts  area {:>12.4}  bbox {:?}",
            item.idx(),
            qty,
            sp.vertices().len(),
            sp.area(),
            sp.bbox()
        );
    }

    let sheets = sheets.map(|s| s.resolve(&instance).expect("resolve separator"));

    let mut rng = Xoshiro256PlusPlus::seed_from_u64(0);
    let (initial, locked) = match sheets.as_ref().filter(|s| s.has_separators()) {
        None => (None, Arc::new(HashSet::new())),
        Some(s) => (
            Some(
                s.warm_start(&instance, Xoshiro256PlusPlus::seed_from_u64(1))
                    .expect("warm start"),
            ),
            s.locked(),
        ),
    };
    if let Some(s) = &sheets {
        eprintln!("--- separator is item idx {:?} ---", s.separator_idx);
    }

    let solution = optimize(
        instance,
        std::mem::replace(&mut rng, Xoshiro256PlusPlus::seed_from_u64(0)),
        &mut Quiet,
        &mut Clock { timeout: None },
        &cfg.expl_cfg,
        &cfg.cmpr_cfg,
        initial.as_ref(),
        locked,
    )
    .expect("optimize");

    // The same envelope the add-in writes, so a probe run can be fed straight to
    // tools/plot_nest.py.
    println!(
        "{}",
        nest_kernel::solution_json(&solution, sheets.as_ref()).expect("solution JSON")
    );

    match &sheets {
        None => eprintln!(
            "--- done: width {:.3}, density {:.2}% ---",
            solution.strip_width(),
            solution.density() * 100.0
        ),
        Some(s) => {
            let mut per_sheet = vec![0u32; s.count as usize];
            let mut overflow = 0u32;
            let mut part_area = 0.0_f32;
            let mut extent = 0.0_f32;
            for pi in solution.layout_snapshot().placed_items().values() {
                if s.is_separator(pi.item().idx()) {
                    continue;
                }
                part_area += pi.item().area();
                extent = extent.max(pi.shape().bbox().x_max);
                match s.classify(pi.shape().bbox()) {
                    Placed::OnSheet(k) => per_sheet[k as usize] += 1,
                    Placed::Overflow => {
                        overflow += 1;
                        eprintln!("  OVERFLOW: part bbox {:?}", pi.shape().bbox());
                    }
                }
            }
            let used = per_sheet.iter().rposition(|n| *n > 0).map_or(0, |i| i + 1);
            for (k, n) in per_sheet.iter().enumerate() {
                eprintln!("  sheet {k}: {n} parts");
            }
            eprintln!(
                "--- done: {} of {} sheets, {} overflow, extent {:.3}, \
                 density over used sheets {:.2}% ---",
                used,
                s.count,
                overflow,
                extent,
                100.0 * part_area / (used as f32 * s.width * s.height).max(1.0),
            );
        }
    }
}
