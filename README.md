# FuseNest

2D nesting for Autodesk Fusion, built on [sparrow](https://github.com/JeroenGar/sparrow).

A C++ add-in: native threads and a Rust solver linked straight in. The reasoning
behind that choice, and the measurements supporting it, are in
`../CPPTest/FUSION_CPP_API.md`.

## Layout

```
src/                  pure source; nothing in here ships
  entry.cpp             the dylib's exported surface: run() and stop()
  nest_command.{h,cpp}  the FuseNest command: dialog, validation, execute
  nest_request.{h,cpp}  what the dialog gathers
  profile.{h,cpp}       Fusion geometry -> flat 2D outlines
  solver_input.{h,cpp}  outlines -> the JSON instance sparrow consumes
  solver.{h,cpp}        runs the kernel off-thread, returns via CustomEvent
  nest_kernel.h         hand-written C ABI for the Rust side
  writeback.{h,cpp}     placements -> Fusion move features
  transform4.{h,cpp}    4x4 rigid maths, Fusion-free so it can be unit tested
  log.{h,cpp}           file logging (a GUI process has no stdout)
nest_kernel/          Rust staticlib wrapping sparrow
  src/sheets.rs         sheet nesting on top of a strip packer
vendor/sparrow/       our fork of sparrow; UPSTREAM.md records every patch
packaging/            the non-code parts of the deliverable
  FuseNest.manifest.in  manifest template; the version comes from CMake
  AddInIcon.svg
  resources/            F1 help page + generated third-party notices
  licenses/             canonical licence texts, SOURCES.md records provenance
  make_bundle.cmake     assembles dist/FuseNest.bundle and reports what is in it
  PackageContents.xml.in  scaffold only -- Autodesk generates the real one
tests/                what can be checked without Fusion
tools/plot_nest.py    renders AND geometrically verifies a solution
tools/gen_notices.py  generates resources/notices.html from the build graph
dist/FuseNest/        GENERATED -- the add-in itself. Point Fusion here.
LICENSE               MIT
CMakeLists.txt        source of truth for the build, and for the version
CMakePresets.json     dev | dist (macOS) | win-dev | win-dist | win-vs
```

## The solver

`nest_kernel` is a Rust staticlib linked straight into the add-in, wrapping
[sparrow](https://github.com/JeroenGar/sparrow) (MIT) on
[jagua-rs](https://github.com/JeroenGar/jagua-rs) (MPL-2.0). sparrow is not
published on crates.io and has no tags, so it is **vendored** in
`vendor/sparrow/` -- a snapshot of one `main` commit, with the pin and every
patch recorded in `vendor/sparrow/UPSTREAM.md`. jagua-rs stays an unmodified
crates.io dependency, which keeps the whole fork MIT.

The ABI is JSON in, JSON out, because that is sparrow's own native boundary --
no polygon is hand-marshalled. A solve runs on a worker thread so Fusion stays
responsive, and the result comes back through a CustomEvent, the only legal way
for a worker to reach the Fusion API. Progress lives in atomics the main thread
can poll.

**A jagua-rs item must be one simple polygon**, so holes are captured during
extraction but dropped when building solver input. Items with holes are a hard
import error upstream, not a warning.

## Sheets

sparrow solves an infinite strip; FuseNest nests onto sheets. The bridge is to
lay the sheets end to end into one strip and drop an immovable rectangle -- a
**separator** -- into each inter-sheet gap, so no part can straddle a boundary.
That needs two things upstream sparrow cannot express, which is why the fork
exists: items that never move, and a width objective that ignores them.

Sheet count is an **output**, the way strip width always was, and it is
deliberately not an input either: a count that binds turns the nest into a
different problem -- which parts fit in N bins, rather than how little stock the
set needs -- and a strip packer is not the tool for that. So the dialog asks only
for a sheet width, and the kernel allocates enough sheets to spill onto
(`SheetSpec::allocation`: part area over sheet area at an assumed density, plus a
spare sheet, capped at one sheet per part). The result says how many it came to.

Parts that still fit nowhere are reported as overflow and left where they are
rather than moved into the open space past the last sheet. A sheet width of zero
keeps the unbounded strip, which is still what you want for coil stock.

`nest_kernel/src/sheets.rs` has the geometry, including why a separator whose
ring is exactly the gap rectangle makes sheet assignment a division.

## The deliverable

A Fusion add-in is a **folder**, not an archive format. `dist/FuseNest/` is
exactly it:

```
FuseNest/
  FuseNest.manifest
  FuseNest.dylib      macOS, universal (x86_64 + arm64)
  FuseNest.dll        Windows -- built separately, dropped in beside it
  AddInIcon.svg
```

No source ships. Both platform binaries live side by side and Fusion loads
whichever matches the OS.

`dist/FuseNest/` is also the **merge point** for the two platform binaries:
each is built on its own machine, and the Windows one is copied in beside the
dylib before packaging. Nothing cross-compiles.

### Packaging

- **`--target package-bundle`** -> `dist/FuseNest.bundle/` (`PackageContents.xml`
  + `Contents/`). **This is the publication route.** It reports which platform
  binaries actually made it in, because "it built" and "it is complete" are
  different questions and only the second matters at submission.
- **`--target package-zip`** -> `dist/FuseNest-<version>.zip`. For your own
  testing and beta builds **only**, not for publication -- see below.

`PackageContents.xml` is generated from `packaging/PackageContents.xml.in`,
which is a transcription of **the file Autodesk shipped inside FuseNest 1.0.0**
(pulled from the store 2026-11-09). The schema is therefore observed, not
guessed -- which matters, because four things in it are counter-intuitive:

| | |
|---|---|
| `ProductCode="*"` | A literal asterisk, **not** a GUID. Autodesk substitutes a per-release code at submission. |
| One GUID, not two | The manifest's `id` and the `UpgradeCode` are the **same** GUID -- unbraced in the manifest, braced in the XML. |
| `OS="win64\|mac"` | Lower case, and *not* the manifest's spelling, which is `supportedOS: "windows\|mac"`. |
| `"Fusion360"` | `AutodeskProduct` and `Platform` here, while the add-in manifest says `"Fusion"`. Not interchangeable. |

That one GUID is the **identity of the existing store listing**, so
`FUSENEST_APP_ID` defaults to it and building with it makes this an *update* to
that listing. A different GUID publishes a second, unrelated app. The same value
is written into the staged manifest's `id`, so the two cannot disagree.

Generating rather than retaining a copy is deliberate: the guidelines require a
new version number on every submission, so a hand-maintained `AppVersion` going
stale is the likeliest mistake. If Autodesk ever hands back a different file,
drop it at `packaging/PackageContents.xml` and *that* is staged verbatim instead.

Publisher-facing strings (`FUSENEST_STORE_NAME`, `_AUTHOR`, `_COMPANY`) default
to what 1.0.0 shipped, because renaming a published listing is a decision about
the listing rather than about the build.

`Info.plist` really is generated by Autodesk's packaging -- the shipped 1.0.0
bundle confirms it, including `AdminAuthorization`, which settles the
contradiction between the two Autodesk docs on whether install needs elevation.
It does.

### Why the zip is not the publication route

A downloaded-and-unzipped add-in carries `com.apple.quarantine`, and a
quarantined ad-hoc-signed dylib is refused outright by a host with library
validation -- which Fusion has -- with *library load disallowed by system
policy*. Terminal `unzip` propagates the attribute just as Archive Utility does,
and `xattr -dr` was measured **not** to be enough; the bytes had to be rewritten
to a fresh file. The App Store installer never applies the attribute, which is
why the bundle is the route and why **no code signing certificate is needed** on
either platform. See `../CPPTest/FUSION_CPP_API.md` §11.1-11.3 for the
measurements.

## Requirements

**macOS:** Command Line Tools (no Xcode needed), CMake, Ninja, ccache, Rust, and
-- once -- `sudo DevToolsSecurity -enable` so lldb can attach. VS Code wants
clangd, CMake Tools and CodeLLDB; **not** `ms-vscode.cpptools`, which fights
clangd.

**Windows:** Visual Studio 2022 with the C++ workload (v143), CMake, Ninja and
rustup. No `rustup target add` is needed -- `rust-toolchain.toml` pins the
channel and declares all three targets, so rustup installs what it needs on
first use. That pin exists because two machines building one shipped binary each
must not be two different compilers; a drift between them would be a difference
in the solver that nothing in the build would report.

Fusion must have been launched once on the machine so it stages the C++ API to
`%APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP`; CMake fails with that path
in the message if it is not there.

**The Rust half cross-compiles.** `cargo build --release --target
x86_64-pc-windows-msvc` produces `nest_kernel.lib` on macOS, and sparrow's whole
dependency tree builds clean that way -- verified 2026-11-09. So the Windows
machine is needed for the **C++ DLL** only, which is what actually requires MSVC
and Fusion's Windows link stubs.

## Build

macOS:

```bash
cmake --preset dev  && cmake --build --preset dev     # ~3.5s cold, <1s incremental
cmake --preset dist && cmake --build --preset dist    # universal; run before shipping
```

Windows, from an **x64 Native Tools / Developer PowerShell** prompt (the Ninja
presets need `cl.exe` on `PATH`):

```powershell
cmake --preset win-dev  ; cmake --build --preset win-dev
cmake --preset win-dist ; cmake --build --preset win-dist   # run before shipping
```

`win-vs` is the fallback and the IDE route: the Visual Studio generator finds
its own toolchain, so it works from a plain shell with no vcvars -- use it when
`win-dev` cannot find `cl.exe`, and when you want to attach the VS debugger to
Fusion.

All presets write to `dist/FuseNest/`, so whichever ran last is what is on disk.
Every macOS build prints `lipo -info` so a single-architecture artifact is
obvious.

Three things about the Windows build are link failures rather than preferences,
and they are pinned in `CMakeLists.txt` rather than left to a preset:

- **CRT.** Rust's `x86_64-pc-windows-msvc` links the *dynamic* CRT and the
  kernel is always built `--release`. A Debug configure would hand `/MDd`
  objects to a `/MD` archive and link dies on duplicate CRT symbols and an
  `_ITERATOR_DEBUG_LEVEL` mismatch, so `MSVC_RUNTIME_LIBRARY` is pinned to
  `MultiThreadedDLL` for every configuration.
- **System libraries.** A staticlib carries no link directives, so everything
  Rust's std needs is an unresolved symbol unless named explicitly. The list is
  measured, not guessed: `kernel32 ntdll userenv ws2_32 dbghelp`. The same
  output's `/defaultlib:msvcrt` is Rust confirming the CRT choice above.
  Re-check after any toolchain bump with
  `cargo rustc --release --target x86_64-pc-windows-msvc --crate-type staticlib -- --print native-static-libs`.
- **`/utf-8`.** MSVC otherwise decodes source as the system codepage, and
  `message_test` holds non-ASCII literals *on purpose* -- a mis-decoded literal
  turns a real escaping test into a silent pass.

## Development loop

1. **Stop the add-in in Fusion** -- it holds the dylib open, and a build that
   cannot overwrite leaves you debugging stale code.
2. Build (Cmd-Shift-B).
3. Run from Scripts and Add-Ins, then **Utilities -> Add-Ins -> FuseNest**.
4. `fusenest.log` in the repo root has the detail; the message box has the
   summary.

To debug: F5, pick "Autodesk Fusion", then Run the add-in.

To check a solution without Fusion:

```bash
python3 -I tools/plot_nest.py fusenest_instance.json   # writes nest.svg
```

It reconstructs each placement and verifies the geometry -- no overlaps,
everything inside the strip, separation honoured, and every part wholly within
one sheet -- before drawing anything. That last one is the invariant sheet
nesting rests on, and the only check that would catch a separator in the wrong
place, so it is deliberately done from the geometry rather than from anything
the solver says about itself.

Asking the solver whether it is happy is not a check. sparrow decides a layout
is separated by reading a tracker it maintains incrementally across millions of
moves; a layout it has lost track of looks exactly like a good one from the
outside, with a perfectly ordinary width, density and sheet count. So every
solve also calls `unresolved_collisions`, which rebuilds the layout and its
collision engine from the solution and quantifies it from scratch -- anything
but zero is logged as a warning. The probe prints it, and
`the_returned_layout_is_separated` asserts it.

To solve without Fusion at all, including onto sheets:

```bash
cd nest_kernel
cargo run --release --example probe -- ../fusenest_instance.json 8000 50x4+10 > sol.json
```

`50` is 50 mm sheets, as many as the kernel allocates; `50x4+10` pins the count
at four with a 10 mm gap, which the add-in never does and which is only there to
study the allocation. Drop the argument for the plain strip.

The probe prints what jagua-rs made of each item on import, which is the other
thing that cannot be asked from inside Fusion, and the unresolved collision loss
of both the warm start and the returned layout.

## Things that will bite

- **A green build does not mean new code is live.** The linker writes a new file
  and renames, so a rebuild during a run leaves Fusion on the old image.
- **Fusion must be restarted after every rebuild.** This is now live: the Rust
  kernel is linked, Rust's std uses thread-local storage, and dyld refuses to
  unload any image containing TLS -- `dlclose` returns success and the old code
  stays resident. Stop + rebuild + Run will silently keep running the previous
  build. Statics also survive Stop/Run, so reset state in `run()` rather than
  relying on a fresh load.
- **Never measure performance on a `Debug` build.** It is `-O0` and roughly
  2.5x slow.
- **Set `ARCHS` for release builds.** An arm64-only add-in loads fine on Apple
  silicon and silently fails on Intel Macs. The `dist` preset handles it.
- **The version lives in exactly one place:** `project()` in `CMakeLists.txt`.
  The manifest is generated from it. Do not edit a staged manifest.
- **Windows has its own stale-code question.** The dyld/TLS behaviour above is a
  macOS mechanism; Windows has its own history of restrictions on loading DLLs
  that use static TLS. Assume nothing -- re-characterise it on the first real
  Windows run rather than expecting it to match.

## Licence

FuseNest is MIT -- see `LICENSE`. The solver's dependencies are a separate
matter: `packaging/resources/notices.html` is generated from the build graph by
`tools/gen_notices.py` and carries the licence text of all 107 packages. The one
real obligation there is **jagua-rs (MPL-2.0)**, which ships no copy of its own
licence; it is used unmodified, so the text plus a source link satisfies it.
sparrow is MIT and *is* modified, which the help page states.
