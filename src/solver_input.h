// Profiles -> the JSON instance sparrow actually consumes.
//
// This is the lossy boundary of the whole pipeline: a jagua-rs *item* must be a
// single simple polygon, so every hole captured during extraction is dropped
// here. Items with holes are a hard import error upstream, not a warning, so
// dropping them is the only option short of refusing the part.
#pragma once

#include "nest_request.h"
#include "profile.h"

#include <string>
#include <vector>

namespace fusenest {

struct SolverInput {
    std::string json;
    int itemCount = 0;
    int holesDropped = 0;
};

/// Builds an ExtSPInstance document. Coordinates stay in millimetres: jagua-rs
/// is unit-agnostic and `min_item_separation` is documented as being "in input
/// units", so as long as one unit is used throughout it does not care which.
SolverInput buildSolverInput(const NestRequest& request,
                             const std::vector<Profile>& profiles,
                             const std::string& instanceName);

}  // namespace fusenest
