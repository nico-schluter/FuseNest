// What the UI gathers and the solver adapter consumes. Kept separate from both
// so neither depends on the other.
#pragma once

#include <Core/CoreAll.h>

#include <string>
#include <vector>

namespace fusenest {

/// How a part may be turned on the sheet. Maps onto jagua-rs's
/// `orientation.rotation`: the discrete modes become an explicit angle list
/// (zero is NOT implicit there), Continuous becomes mode "continuous".
enum class RotationMode { None, Half, Quarter, Continuous };

const char* rotationModeLabel(RotationMode mode);

/// Lengths are in MILLIMETRES -- converted once, away from Fusion's internal
/// centimetres, where the dialog is read.
struct NestRequest {
    std::vector<adsk::core::Ptr<adsk::core::Base>> parts;
    double sheetHeightMm = 0.0;
    /// Zero nests onto an unbounded strip -- the behaviour before sheets, and
    /// still what you want when the stock is a coil rather than a sheet.
    double sheetWidthMm = 0.0;
    double partSpacingMm = 0.0;
    RotationMode rotation = RotationMode::Half;
    int timeBudgetSeconds = 10;
    int workers = 4;
    /// Raises the kernel's log level from info to debug. Info already covers
    /// every cycle; debug adds sparrow's per-iteration separator output.
    bool captureDiagnostics = false;

    std::string describe() const;
};

}  // namespace fusenest
