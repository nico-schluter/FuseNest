//! The sparrow nesting solver behind a C ABI.
//!
//! JSON in, JSON out. That is not a convenience compromise: sparrow's own
//! external representation *is* serde JSON (`ExtSPInstance` / `ExtSPSolution`),
//! so strings put us exactly on the library's native boundary and no polygon is
//! ever hand-marshalled.
//!
//! Threading: `nk_nest` is deliberately **synchronous**. The C++ side owns a
//! thread and marshals completion back through a Fusion CustomEvent, and
//! sparrow parallelises internally via `n_workers`. One owner of threading.

use jagua_rs::Instant;
use jagua_rs::io::import::Importer;
use jagua_rs::probs::spp::io::ext_repr::ExtSPInstance;
use rand::SeedableRng;
use rand::rngs::Xoshiro256PlusPlus;
use sparrow::config::{DEFAULT_SPARROW_CONFIG, ShrinkDecayStrategy};
use sparrow::optimizer::optimize;
use sparrow::util::listener::{
    OptimizationPhase, ReportType, SeparationProgress, SeparationResult, SolutionListener,
};
use sparrow::util::terminator::Terminator;

use log::{LevelFilter, Log, Metadata, Record};
use std::collections::VecDeque;
use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::time::Duration;

// --- macOS quality of service -------------------------------------------
// sparrow builds its own rayon pool with no start_handler, so QoS cannot be
// injected into it directly. But threads created on macOS inherit the QoS of
// the thread that spawns them, so raising it here -- on the thread that calls
// into sparrow -- should carry into the pool. Default-QoS threads drift onto
// the efficiency cores, which measured ~10% slower on an M1.
#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn pthread_set_qos_class_self_np(qos_class: u32, relative_priority: i32) -> i32;
}
#[cfg(target_os = "macos")]
const QOS_CLASS_USER_INITIATED: u32 = 0x19;

// --- state the main thread polls ----------------------------------------
// Progress crosses the thread boundary as plain atomics. A solver worker must
// never call into the Fusion API, so there is no callback anywhere.
static CANCEL: AtomicBool = AtomicBool::new(false);
static RUNNING: AtomicBool = AtomicBool::new(false);
static ITERATION: AtomicU64 = AtomicU64::new(0);
static SEPARATIONS: AtomicU64 = AtomicU64::new(0);
static PHASE: AtomicU32 = AtomicU32::new(0); // 0 idle, 1 exploration, 2 compression
static DENSITY_BITS: AtomicU32 = AtomicU32::new(0);
static STRIP_WIDTH_BITS: AtomicU32 = AtomicU32::new(0);

/// Placements in a C-friendly form. The solver already has these structured,
/// so handing them over as structs spares the C++ side a JSON parser it would
/// otherwise need only for this.
/// A feasible layout, published whole.
///
/// The width and density live HERE rather than being read from the progress
/// atomics, which is a distinction that cost a test failure to find: those
/// atomics are overwritten continuously by report_separation_progress, including
/// from states where items still overlap. Reading them alongside these
/// placements describes a different layout than the one being drawn.
struct Snapshot {
    placements: Vec<NkPlacement>,
    strip_width: f32,
    density: f32,
}

static SNAPSHOT: Mutex<Snapshot> = Mutex::new(Snapshot {
    placements: Vec::new(),
    strip_width: 0.0,
    density: 0.0,
});

/// Bumped every time SNAPSHOT is replaced. The main thread uses it to tell a
/// layout it has already drawn from a new one, which is what keeps a live
/// preview from re-applying the same placements several times a second.
static SNAPSHOT_GEN: AtomicU64 = AtomicU64::new(0);

static RESULT: Mutex<String> = Mutex::new(String::new());
static ERROR: Mutex<String> = Mutex::new(String::new());
static LAST_PANIC: Mutex<String> = Mutex::new(String::new());

/// One placed part. `item_id` is the id we put in the instance, which is the
/// index of the profile that produced it.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NkPlacement {
    pub item_id: u64,
    pub rotation: f32,
    pub tx: f32,
    pub ty: f32,
    /// Always 0 unless the instance asked for reflection axes. Fusion's move
    /// feature cannot take a reflection -- it is not a rigid transform in the
    /// plane -- so the C++ side refuses rather than silently mangling a part.
    pub reflected: u32,
}

/// What a snapshot is, apart from its placements. Kept in step with
/// nest_kernel.h by hand; a layout mismatch here is silent.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NkSnapshotInfo {
    pub generation: u64,
    /// Placements in the snapshot, which may exceed what a copy could fit.
    pub count: u64,
    pub strip_width: f32,
    pub density: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NkProgress {
    pub phase: u32,
    pub iteration: u64,
    pub separations: u64,
    pub density: f32,
    pub strip_width: f32,
    pub running: u32,
    pub cancelled: u32,
}

fn set_f32(slot: &AtomicU32, v: f32) {
    slot.store(v.to_bits(), Ordering::Relaxed);
}
fn get_f32(slot: &AtomicU32) -> f32 {
    f32::from_bits(slot.load(Ordering::Relaxed))
}

fn store(slot: &Mutex<String>, value: String) {
    if let Ok(mut g) = slot.lock() {
        *g = value;
    }
}

// --- log capture --------------------------------------------------------
// sparrow already narrates its own search: explore.rs alone has nine info!
// sites covering every shrink, every failed separation and every pool
// rollback. That is exactly the cycle-by-cycle trace needed to understand why
// a solve does or does not converge, and it was being discarded because a GUI
// process has no stderr. Capture it into a ring buffer the main thread drains.

/// Bounded so a long solve cannot grow this without limit if nobody drains.
/// At info level a cycle produces a handful of lines, so this is many minutes
/// of history.
const LOG_CAP: usize = 8192;

static LOG_LINES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());

fn push_log(line: String) {
    if let Ok(mut g) = LOG_LINES.lock() {
        if g.len() >= LOG_CAP {
            g.pop_front();
        }
        g.push_back(line);
    }
}

struct FfiLogger;

impl Log for FfiLogger {
    fn enabled(&self, _metadata: &Metadata) -> bool {
        // Level filtering is done by `log` itself via set_max_level, so there
        // is nothing to decide here.
        true
    }

    fn log(&self, record: &Record) {
        push_log(format!("{}", record.args()));
    }

    fn flush(&self) {}
}

static LOGGER: FfiLogger = FfiLogger;

/// set_logger can only succeed once per process image, so failure here means a
/// previous install is still in place -- which is fine, it is this same one.
fn install_logger() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        let _ = log::set_logger(&LOGGER);
        log::set_max_level(LevelFilter::Info);
    });
}

/// Fusion is a GUI process with no stderr, so Rust's default panic output goes
/// nowhere. Capture it instead and let C++ pull it into the log.
fn install_panic_hook() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            store(&LAST_PANIC, format!("{info}"));
        }));
    });
}

/// Replaces the published layout with this solution's.
///
/// Only ever called with a FEASIBLE solution. sparrow also reports solutions
/// mid-separation, where items still overlap -- those describe the search, not
/// a layout anyone could cut, and publishing them would draw parts sitting on
/// top of each other.
fn publish_snapshot(sol: &jagua_rs::probs::spp::entities::SPSolution) {
    let ext = jagua_rs::probs::spp::io::export(sol, *sparrow::EPOCH);
    let placements = ext
        .layout
        .placed_items
        .iter()
        .map(|p| NkPlacement {
            item_id: p.item_id,
            rotation: p.transformation.rotation,
            tx: p.transformation.translation.0,
            ty: p.transformation.translation.1,
            reflected: u32::from(p.transformation.reflected),
        })
        .collect::<Vec<_>>();

    if let Ok(mut g) = SNAPSHOT.lock() {
        *g = Snapshot {
            placements,
            strip_width: ext.strip_width,
            density: sol.density(),
        };
    }
    // Release: everything written above must be visible to a thread that sees
    // this new generation.
    SNAPSHOT_GEN.fetch_add(1, Ordering::Release);
}

struct FfiListener;

impl SolutionListener for FfiListener {
    fn report(&mut self, r: ReportType, s: &jagua_rs::probs::spp::entities::SPSolution) {
        set_f32(&DENSITY_BITS, s.density());
        set_f32(&STRIP_WIDTH_BITS, s.strip_width());
        // Which ReportType a solution arrives under is the difference between a
        // layout that can be previewed and one with items still overlapping, so
        // it is worth seeing in the trace.
        push_log(format!(
            "[SOL] {:?}: width {:.3}, density {:.2}%",
            r,
            s.strip_width(),
            s.density() * 100.0
        ));

        // This is what makes early accept possible: a feasible report is the
        // same kind of artifact as the final answer, just earlier. optimize()
        // ends by reporting its return value as ReportType::Final, so the last
        // snapshot published here IS the final solution -- there is no separate
        // finishing step a partial result would be missing.
        match r {
            ReportType::ExplFeas | ReportType::CmprFeas | ReportType::Final => {
                publish_snapshot(s)
            }
            ReportType::ExplInfeas | ReportType::ExplImproving => {}
        }
    }

    fn report_phase(&mut self, phase: OptimizationPhase) {
        PHASE.store(
            match phase {
                OptimizationPhase::Exploration => 1,
                OptimizationPhase::Compression => 2,
            },
            Ordering::Relaxed,
        );
    }

    /// Once per separate() call -- one cycle. sparrow logs the decision that
    /// follows but not the effort it took to get there.
    fn report_separation_result(&mut self, r: SeparationResult) {
        push_log(format!(
            "[SEP] {} after {} iters in {:.2}s ({} evals, {} moves)",
            if r.success { "separated" } else { "FAILED" },
            r.iterations,
            r.elapsed_seconds,
            r.total_evals,
            r.total_moves
        ));
    }

    fn report_separation_progress(&mut self, p: SeparationProgress) {
        // `iteration` restarts at 0 every separate() call, so on its own it
        // appears to run backwards. Count the calls for a monotonic signal.
        if p.iteration == 0 {
            SEPARATIONS.fetch_add(1, Ordering::Relaxed);
        }
        ITERATION.store(p.iteration as u64, Ordering::Relaxed);
        // CAREFUL: SeparationProgress.density is already a percentage
        // (separator.rs multiplies by 100) while SPSolution::density() is a
        // fraction. Undocumented inconsistency; normalised here so one scale
        // leaves this layer.
        set_f32(&DENSITY_BITS, p.density / 100.0);
        set_f32(&STRIP_WIDTH_BITS, p.strip_width);
    }
}

/// A timeout plus our cancel flag. `kill()` is polled by the solver, which is
/// what makes cancellation fast and graceful -- it returns its best solution so
/// far rather than being killed mid-flight.
struct FfiTerminator {
    timeout: Option<Instant>,
}

impl Terminator for FfiTerminator {
    fn kill(&self) -> bool {
        CANCEL.load(Ordering::Relaxed) || self.timeout.is_some_and(|t| Instant::now() > t)
    }
    fn new_timeout(&mut self, timeout: Duration) {
        self.timeout = Some(Instant::now() + timeout);
    }
    fn timeout_at(&self) -> Option<Instant> {
        self.timeout
    }
}

/// What a caller may tune about a solve.
///
/// This exists because sparrow's shipped defaults are calibrated for a NINE
/// MINUTE benchmark run -- `DEFAULT_SPARROW_CONFIG.expl_cfg.time_limit` is
/// 9*60s -- and an interactive nest gets seconds. The step size is the one that
/// matters most: at the default 0.1% per cycle, a 7.5s exploration buys about
/// 146 shrinks, or 13% total reduction, which is why a solve that has found a
/// good layout still looks like it is going nowhere.
#[derive(Debug, Clone, Copy)]
pub struct NkTuning {
    /// Ceiling, not a target. Termination should come from convergence.
    pub time_limit_ms: u64,
    pub n_workers: u32,
    pub seed: u64,
    /// Fraction the strip shrinks after each successful separation.
    pub shrink_step: f32,
    /// Consecutive failed separations at a width before exploration gives up.
    /// This is what "converged" means: the search has stopped finding room.
    ///
    /// Measured on an 8-part instance: 3, 5 and 10 all reach an identical
    /// 61.894mm, but exploration takes 20.4s, 27.4s and 41.2s respectively, so
    /// everything above 3 is pure waiting. 2 gives up early at 63.398mm, which
    /// is measurably worse. Hence 3.
    pub max_conseq_failed_attempts: usize,
    /// Rate at which the compression step decays per failed attempt. Compression
    /// terminates once the step decays below sparrow's floor, which is what
    /// makes it convergence-bound rather than clock-bound.
    pub compression_decay: f32,
    /// Share of the budget given to exploration; the rest goes to compression.
    pub exploration_share: f32,
}

impl Default for NkTuning {
    fn default() -> Self {
        Self {
            time_limit_ms: 60_000,
            n_workers: 4,
            seed: 0xC0FFEE,
            shrink_step: DEFAULT_SPARROW_CONFIG.expl_cfg.shrink_step,
            max_conseq_failed_attempts: 3,
            compression_decay: 0.5,
            exploration_share: 0.75,
        }
    }
}

fn run_nest(input_json: &str, tuning: &NkTuning) -> Result<String, String> {
    let ext: ExtSPInstance =
        serde_json::from_str(input_json).map_err(|e| format!("input JSON: {e}"))?;

    let mut cfg = DEFAULT_SPARROW_CONFIG;

    let total = Duration::from_millis(tuning.time_limit_ms.max(200));
    cfg.expl_cfg.time_limit = total.mul_f32(tuning.exploration_share);
    cfg.cmpr_cfg.time_limit = total.mul_f32(1.0 - tuning.exploration_share);

    cfg.expl_cfg.shrink_step = tuning.shrink_step;
    cfg.expl_cfg.max_conseq_failed_attempts = match tuning.max_conseq_failed_attempts {
        usize::MAX => None,
        n => Some(n),
    };

    // sparrow ships TimeBased decay, which derives the shrink step from
    // elapsed/time_limit. That makes compression purely clock-bound: it
    // succeeds trivially at microscopic steps -- the trace shows "success at
    // 0.004%" with zero evals and zero moves -- until its allowance runs out.
    // Worse, with no wall-clock budget at all the ratio grows without bound and
    // the loop's step >= floor guard never trips. FailureBased decays on failed
    // attempts instead, so compression stops when it stops making progress.
    cfg.cmpr_cfg.shrink_decay = ShrinkDecayStrategy::FailureBased(tuning.compression_decay);

    if tuning.n_workers > 0 {
        cfg.expl_cfg.separator_config.n_workers = tuning.n_workers as usize;
        cfg.cmpr_cfg.separator_config.n_workers = tuning.n_workers as usize;
    }

    let importer = Importer::new(
        cfg.cde_config,
        cfg.poly_simpl_tolerance,
        cfg.narrow_concavity_cutoff_ratio,
    );
    let instance = jagua_rs::probs::spp::io::import_instance(&importer, &ext)
        .map_err(|e| format!("import_instance: {e}"))?;

    let solution = optimize(
        instance,
        Xoshiro256PlusPlus::seed_from_u64(tuning.seed),
        &mut FfiListener,
        &mut FfiTerminator { timeout: None },
        &cfg.expl_cfg,
        &cfg.cmpr_cfg,
        None,
    )
    .map_err(|e| format!("optimize: {e}"))?;

    // The listener already published this exact solution as a snapshot when
    // optimize() reported it as ReportType::Final, so there is nothing to store
    // here -- only the JSON to hand back.
    let ext_sol = jagua_rs::probs::spp::io::export(&solution, *sparrow::EPOCH);
    serde_json::to_string(&ext_sol).map_err(|e| format!("output JSON: {e}"))
}

/// Blocking nest. 0 on success, -1 caught panic, -2 bad arguments, -3 solver or
/// parse error (see `nk_last_error`). On success the result JSON is available
/// through `nk_result_len` / `nk_result_copy`.
#[unsafe(no_mangle)]
pub extern "C" fn nk_nest(
    input_json: *const c_char,
    time_limit_ms: u64,
    n_workers: u32,
    seed: u64,
) -> i32 {
    install_panic_hook();
    install_logger();

    #[cfg(target_os = "macos")]
    unsafe {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    }

    if input_json.is_null() {
        store(&ERROR, "input_json was null".into());
        return -2;
    }
    let input = match unsafe { CStr::from_ptr(input_json) }.to_str() {
        Ok(s) => s,
        Err(e) => {
            store(&ERROR, format!("input_json not UTF-8: {e}"));
            return -2;
        }
    };

    CANCEL.store(false, Ordering::Relaxed);
    ITERATION.store(0, Ordering::Relaxed);
    SEPARATIONS.store(0, Ordering::Relaxed);
    PHASE.store(0, Ordering::Relaxed);
    set_f32(&DENSITY_BITS, 0.0);
    set_f32(&STRIP_WIDTH_BITS, 0.0);
    store(&RESULT, String::new());
    store(&ERROR, String::new());
    if let Ok(mut g) = SNAPSHOT.lock() {
        g.placements.clear();
        g.strip_width = 0.0;
        g.density = 0.0;
    }
    // Not reset to zero: a monotonic counter means a stale generation held by
    // the main thread can never accidentally match a new solve's first
    // snapshot, which a reset-to-zero scheme would allow.
    SNAPSHOT_GEN.fetch_add(1, Ordering::Release);
    RUNNING.store(true, Ordering::Relaxed);

    let tuning = NkTuning {
        time_limit_ms,
        n_workers,
        seed,
        ..NkTuning::default()
    };
    let outcome = catch_unwind(AssertUnwindSafe(|| run_nest(input, &tuning)));

    RUNNING.store(false, Ordering::Relaxed);
    PHASE.store(0, Ordering::Relaxed);

    match outcome {
        Ok(Ok(json)) => {
            store(&RESULT, json);
            0
        }
        Ok(Err(msg)) => {
            store(&ERROR, msg);
            -3
        }
        Err(_) => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_cancel() {
    CANCEL.store(true, Ordering::Relaxed);
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_progress(out: *mut NkProgress) -> i32 {
    if out.is_null() {
        return -2;
    }
    let p = NkProgress {
        phase: PHASE.load(Ordering::Relaxed),
        iteration: ITERATION.load(Ordering::Relaxed),
        separations: SEPARATIONS.load(Ordering::Relaxed),
        density: get_f32(&DENSITY_BITS),
        strip_width: get_f32(&STRIP_WIDTH_BITS),
        running: RUNNING.load(Ordering::Relaxed) as u32,
        cancelled: CANCEL.load(Ordering::Relaxed) as u32,
    };
    unsafe { *out = p };
    0
}

fn copy_out(slot: &Mutex<String>, buf: *mut u8, len: usize) -> usize {
    if buf.is_null() || len == 0 {
        return 0;
    }
    let s = match slot.lock() {
        Ok(g) => g.clone(),
        Err(_) => return 0,
    };
    let bytes = s.as_bytes();
    let n = bytes.len().min(len - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, n);
        *buf.add(n) = 0;
    }
    n
}

/// The current snapshot's generation. Cheap; safe at any time, and the only
/// call needed to decide whether anything has changed.
#[unsafe(no_mangle)]
pub extern "C" fn nk_snapshot_generation() -> u64 {
    SNAPSHOT_GEN.load(Ordering::Acquire)
}

/// Copies the published layout and its own measurements under one lock.
/// Returns the number of placements written, 0 if nothing is published yet.
///
/// This replaces a count-then-index-each-placement loop, which was safe only
/// while placements were written once at the end of a solve. Now that they are
/// republished on every improvement, reading them one at a time can splice two
/// layouts together and produce a drawing of neither.
#[unsafe(no_mangle)]
pub extern "C" fn nk_snapshot_copy(
    out: *mut NkPlacement,
    max: usize,
    info: *mut NkSnapshotInfo,
) -> usize {
    let Ok(guard) = SNAPSHOT.lock() else {
        return 0;
    };
    let n = if out.is_null() { 0 } else { guard.placements.len().min(max) };
    if n > 0 {
        unsafe { std::ptr::copy_nonoverlapping(guard.placements.as_ptr(), out, n) };
    }
    if !info.is_null() {
        unsafe {
            *info = NkSnapshotInfo {
                generation: SNAPSHOT_GEN.load(Ordering::Acquire),
                count: guard.placements.len() as u64,
                strip_width: guard.strip_width,
                density: guard.density,
            };
        }
    }
    n
}


#[unsafe(no_mangle)]
pub extern "C" fn nk_result_len() -> usize {
    RESULT.lock().map(|g| g.len()).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_result_copy(buf: *mut u8, len: usize) -> usize {
    copy_out(&RESULT, buf, len)
}

/// Sets how much detail reaches the log. Info is the cycle-by-cycle narrative;
/// Debug adds sparrow's per-iteration separator chatter, which is far heavier.
/// 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace.
#[unsafe(no_mangle)]
pub extern "C" fn nk_log_set_level(level: u32) {
    install_logger();
    log::set_max_level(match level {
        0 => LevelFilter::Off,
        1 => LevelFilter::Error,
        2 => LevelFilter::Warn,
        4 => LevelFilter::Debug,
        5 => LevelFilter::Trace,
        _ => LevelFilter::Info,
    });
}

/// Drains captured log lines into `buf` as newline-terminated text. Returns
/// bytes written, 0 when there is nothing left. Writes only whole lines, except
/// for a single line too long to ever fit, which is truncated so a caller
/// looping until 0 always makes progress.
#[unsafe(no_mangle)]
pub extern "C" fn nk_log_drain(buf: *mut u8, len: usize) -> usize {
    if buf.is_null() || len == 0 {
        return 0;
    }
    let Ok(mut lines) = LOG_LINES.lock() else {
        return 0;
    };

    let mut written = 0usize;
    while let Some(line) = lines.front() {
        let needed = line.len() + 1;
        if written + needed > len {
            if written > 0 {
                break; // come back for this one on the next call
            }
            // Single oversized line: truncate at a char boundary rather than
            // stalling forever.
            let mut cut = len - 1;
            while cut > 0 && !line.is_char_boundary(cut) {
                cut -= 1;
            }
            let bytes = &line.as_bytes()[..cut];
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, cut);
                *buf.add(cut) = b'\n';
            }
            lines.pop_front();
            return cut + 1;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(line.as_ptr(), buf.add(written), line.len());
            *buf.add(written + line.len()) = b'\n';
        }
        written += needed;
        lines.pop_front();
    }
    written
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_last_error(buf: *mut u8, len: usize) -> usize {
    copy_out(&ERROR, buf, len)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_last_panic(buf: *mut u8, len: usize) -> usize {
    copy_out(&LAST_PANIC, buf, len)
}

// --- offline harness ----------------------------------------------------
// Reproducing a solve outside Fusion is the only way to study convergence
// without a GUI in the loop. Not compiled into the shipped staticlib.
#[cfg(test)]
mod tests {
    use super::*;


    fn drain_to_stdout() {
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            let n = nk_log_drain(buf.as_mut_ptr(), buf.len());
            if n == 0 {
                return;
            }
            print!("{}", String::from_utf8_lossy(&buf[..n]));
        }
    }

    /// Confirms feasible layouts are published as the solve runs, not just at
    /// the end, and that the generation advances with them.
    #[test]
    fn snapshots_stream() {
        let json = std::fs::read_to_string("../fusenest_instance.json").expect("instance json");
        nk_log_set_level(0);

        let seen = std::sync::Arc::new(std::sync::Mutex::new(Vec::<(u64, usize, f32)>::new()));
        let watcher = {
            let seen = seen.clone();
            std::thread::spawn(move || {
                let mut last = 0u64;
                let mut buf = vec![NkPlacement::default(); 4096];
                while RUNNING.load(Ordering::Relaxed) || last == 0 {
                    let mut info = NkSnapshotInfo::default();
                    let n = nk_snapshot_copy(buf.as_mut_ptr(), buf.len(), &mut info);
                    if info.generation != last && n > 0 {
                        last = info.generation;
                        seen.lock().unwrap().push((info.generation, n, info.strip_width));
                    }
                    std::thread::sleep(std::time::Duration::from_millis(20));
                }
            })
        };

        RUNNING.store(true, Ordering::Relaxed);
        let tuning = NkTuning { time_limit_ms: 20_000, ..NkTuning::default() };
        let out = run_nest(&json, &tuning);
        RUNNING.store(false, Ordering::Relaxed);
        watcher.join().unwrap();
        assert!(out.is_ok(), "solve failed: {out:?}");

        let seen = seen.lock().unwrap();
        println!("observed {} distinct snapshots mid-solve", seen.len());
        for (gen, n, w) in seen.iter().take(4) {
            println!("  gen {gen}: {n} placements, width {w:.3}");
        }
        if let Some((gen, n, w)) = seen.last() {
            println!("  ...  gen {gen}: {n} placements, width {w:.3}");
        }

        assert!(seen.len() > 5, "expected a stream, saw {}", seen.len());
        let widths: Vec<f32> = seen.iter().map(|(_, _, w)| *w).collect();
        // Monotonic by construction: exploration reports a feasible solution
        // only when it beats the best width, and compression only on success.
        // This assertion earned its keep -- it failed while the width was being
        // read from the progress atomics instead of from the snapshot, which is
        // exactly the bug of describing one layout while drawing another.
        for pair in widths.windows(2) {
            let tolerance = pair[0] * 1e-6;
            assert!(
                pair[1] <= pair[0] + tolerance,
                "width grew beyond rounding: {} -> {}",
                pair[0],
                pair[1]
            );
        }
        let counts: Vec<usize> = seen.iter().map(|(_, n, _)| *n).collect();
        assert!(
            counts.iter().all(|c| *c == counts[0]),
            "every snapshot must place every part: {counts:?}"
        );
    }

    /// cargo test --release -- --nocapture trace
    /// FUSENEST_INSTANCE / FUSENEST_BUDGET_MS / FUSENEST_LEVEL override defaults.
    #[test]
    fn trace() {
        let path = std::env::var("FUSENEST_INSTANCE")
            .unwrap_or_else(|_| "../fusenest_instance.json".into());
        let budget: u64 = std::env::var("FUSENEST_BUDGET_MS")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(10_000);
        let level: u32 = std::env::var("FUSENEST_LEVEL")
            .ok()
            .and_then(|s| s.parse().ok())
            .unwrap_or(3);

        let json = std::fs::read_to_string(&path).expect("instance json");

        let env_f32 = |k: &str, d: f32| -> f32 {
            std::env::var(k).ok().and_then(|s| s.parse().ok()).unwrap_or(d)
        };
        let tuning = NkTuning {
            time_limit_ms: budget,
            shrink_step: env_f32("FUSENEST_SHRINK", NkTuning::default().shrink_step),
            max_conseq_failed_attempts: std::env::var("FUSENEST_STAGNATION")
                .ok()
                .and_then(|s| s.parse().ok())
                .unwrap_or(NkTuning::default().max_conseq_failed_attempts),
            compression_decay: env_f32("FUSENEST_CMPR_DECAY", NkTuning::default().compression_decay),
            exploration_share: env_f32("FUSENEST_EXPL_SHARE", 0.75),
            ..NkTuning::default()
        };

        nk_log_set_level(level);
        install_panic_hook();
        println!("--- {path}, level {level}, {tuning:?} ---");

        RUNNING.store(true, Ordering::Relaxed);
        CANCEL.store(false, Ordering::Relaxed);
        SEPARATIONS.store(0, Ordering::Relaxed);
        let t0 = std::time::Instant::now();
        let result = run_nest(&json, &tuning);
        let elapsed = t0.elapsed();
        RUNNING.store(false, Ordering::Relaxed);
        let rc = if result.is_ok() { 0 } else { -3 };
        if let Err(e) = &result {
            println!("ERROR: {e}");
        }

        drain_to_stdout();

        let mut p = NkProgress {
            phase: 0,
            iteration: 0,
            separations: 0,
            density: 0.0,
            strip_width: 0.0,
            running: 0,
            cancelled: 0,
        };
        nk_progress(&mut p);
        println!(
            "=== rc {rc}  {:.2}s wall  density {:.2}%  width {:.3}  {} separations ===",
            elapsed.as_secs_f32(),
            p.density * 100.0,
            p.strip_width,
            p.separations
        );
        assert_eq!(rc, 0, "solve failed");
    }
}
