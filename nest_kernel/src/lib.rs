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
use sparrow::config::DEFAULT_SPARROW_CONFIG;
use sparrow::optimizer::optimize;
use sparrow::util::listener::{OptimizationPhase, ReportType, SeparationProgress, SolutionListener};
use sparrow::util::terminator::Terminator;

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
static PLACEMENTS: Mutex<Vec<NkPlacement>> = Mutex::new(Vec::new());

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

struct FfiListener;

impl SolutionListener for FfiListener {
    fn report(&mut self, _r: ReportType, s: &jagua_rs::probs::spp::entities::SPSolution) {
        set_f32(&DENSITY_BITS, s.density());
        set_f32(&STRIP_WIDTH_BITS, s.strip_width());
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

fn run_nest(input_json: &str, time_limit_ms: u64, n_workers: u32, seed: u64) -> Result<String, String> {
    let ext: ExtSPInstance =
        serde_json::from_str(input_json).map_err(|e| format!("input JSON: {e}"))?;

    let mut cfg = DEFAULT_SPARROW_CONFIG;

    // Split the budget between sparrow's two phases, 3:1 exploration to
    // compression, mirroring the proportions of the shipped defaults.
    let total = Duration::from_millis(time_limit_ms.max(200));
    cfg.expl_cfg.time_limit = total.mul_f32(0.75);
    cfg.cmpr_cfg.time_limit = total.mul_f32(0.25);

    if n_workers > 0 {
        cfg.expl_cfg.separator_config.n_workers = n_workers as usize;
        cfg.cmpr_cfg.separator_config.n_workers = n_workers as usize;
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
        Xoshiro256PlusPlus::seed_from_u64(seed),
        &mut FfiListener,
        &mut FfiTerminator { timeout: None },
        &cfg.expl_cfg,
        &cfg.cmpr_cfg,
        None,
    )
    .map_err(|e| format!("optimize: {e}"))?;

    let ext_sol = jagua_rs::probs::spp::io::export(&solution, *sparrow::EPOCH);

    let placements = ext_sol
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
    if let Ok(mut g) = PLACEMENTS.lock() {
        *g = placements;
    }
    set_f32(&STRIP_WIDTH_BITS, ext_sol.strip_width);

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
    if let Ok(mut g) = PLACEMENTS.lock() {
        g.clear();
    }
    RUNNING.store(true, Ordering::Relaxed);

    let outcome = catch_unwind(AssertUnwindSafe(|| {
        run_nest(input, time_limit_ms, n_workers, seed)
    }));

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

#[unsafe(no_mangle)]
pub extern "C" fn nk_placement_count() -> usize {
    PLACEMENTS.lock().map(|g| g.len()).unwrap_or(0)
}

/// Copies placement `index` into `out`. 0 on success, -1 out of range,
/// -2 null pointer.
#[unsafe(no_mangle)]
pub extern "C" fn nk_placement_at(index: usize, out: *mut NkPlacement) -> i32 {
    if out.is_null() {
        return -2;
    }
    match PLACEMENTS.lock() {
        Ok(g) => match g.get(index) {
            Some(p) => {
                unsafe { *out = *p };
                0
            }
            None => -1,
        },
        Err(_) => -1,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_strip_width() -> f32 {
    get_f32(&STRIP_WIDTH_BITS)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_result_len() -> usize {
    RESULT.lock().map(|g| g.len()).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_result_copy(buf: *mut u8, len: usize) -> usize {
    copy_out(&RESULT, buf, len)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_last_error(buf: *mut u8, len: usize) -> usize {
    copy_out(&ERROR, buf, len)
}

#[unsafe(no_mangle)]
pub extern "C" fn nk_last_panic(buf: *mut u8, len: usize) -> usize {
    copy_out(&LAST_PANIC, buf, len)
}
