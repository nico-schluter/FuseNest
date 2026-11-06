//! Terminal probe: import an instance JSON and solve it, printing what
//! jagua-rs made of each item on the way through.
//!
//!     cargo run --release --example probe -- instance.json [ms] > solution.json
//!
//! Exists to answer what the add-in cannot be asked from a terminal: whether a
//! given polygon survives `import_instance`, and what shape it is afterwards
//! (jagua-rs simplifies on import, so the item the solver places is not
//! necessarily the ring we sent).

use jagua_rs::io::import::Importer;
use jagua_rs::probs::spp::io::ext_repr::ExtSPInstance;
use rand::SeedableRng;
use rand::rngs::Xoshiro256PlusPlus;
use sparrow::config::{DEFAULT_SPARROW_CONFIG, ShrinkDecayStrategy};
use sparrow::optimizer::optimize;
use sparrow::util::listener::{ReportType, SolutionListener};
use sparrow::util::terminator::Terminator;
use std::time::Duration;

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

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).expect("usage: probe <instance.json> [ms]");
    let ms: u64 = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(3000);

    let json = std::fs::read_to_string(path).expect("read instance");
    let ext: ExtSPInstance = serde_json::from_str(&json).expect("parse instance");

    eprintln!("--- as sent ---");
    for it in &ext.items {
        eprintln!("  item {:<3} demand {}", it.base.id, it.demand);
    }

    let mut cfg = DEFAULT_SPARROW_CONFIG;
    let total = Duration::from_millis(ms.max(200));
    cfg.expl_cfg.time_limit = total.mul_f32(0.75);
    cfg.cmpr_cfg.time_limit = total.mul_f32(0.25);
    cfg.cmpr_cfg.shrink_decay = ShrinkDecayStrategy::FailureBased(0.5);

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

    let solution = optimize(
        instance,
        Xoshiro256PlusPlus::seed_from_u64(0),
        &mut Quiet,
        &mut Clock { timeout: None },
        &cfg.expl_cfg,
        &cfg.cmpr_cfg,
        None,
    )
    .expect("optimize");

    let ext_sol = jagua_rs::probs::spp::io::export(&solution, *sparrow::EPOCH);
    println!("{}", serde_json::to_string(&ext_sol).unwrap());
    eprintln!(
        "--- done: width {:.3}, density {:.2}% ---",
        solution.strip_width(),
        solution.density() * 100.0
    );
}
