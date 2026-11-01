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
    double partSpacingMm = 0.0;
    RotationMode rotation = RotationMode::Half;
    int timeBudgetSeconds = 10;
    int workers = 4;

    std::string describe() const;
};

}  // namespace fusenest
