#include "solver_input.h"

#include <iomanip>
#include <sstream>

namespace fusenest {
namespace {

/// jagua-rs parses into f32, so more than ~7 significant digits is noise. Nine
/// keeps the round trip lossless at the point where it is still a double.
constexpr int kPrecision = 9;

void writeRotation(std::ostringstream& os, RotationMode mode) {
    // ExtRotation is #[serde(tag = "mode", deny_unknown_fields)], so the shape
    // of this object has to be exact -- no extra keys tolerated. And the
    // discrete variant requires zero to be listed explicitly.
    switch (mode) {
        case RotationMode::None:
            os << R"({"mode":"discrete","angles":[0.0]})";
            break;
        case RotationMode::Half:
            os << R"({"mode":"discrete","angles":[0.0,180.0]})";
            break;
        case RotationMode::Quarter:
            os << R"({"mode":"discrete","angles":[0.0,90.0,180.0,270.0]})";
            break;
        case RotationMode::Continuous:
            os << R"({"mode":"continuous"})";
            break;
    }
}

void writeRing(std::ostringstream& os, const std::vector<Point2>& ring) {
    os << '[';
    for (size_t i = 0; i < ring.size(); ++i) {
        if (i) os << ',';
        os << '[' << ring[i].x << ',' << ring[i].y << ']';
    }
    os << ']';
}

}  // namespace

SolverInput buildSolverInput(const NestRequest& request,
                             const std::vector<Profile>& profiles,
                             const std::string& instanceName) {
    SolverInput out;

    std::ostringstream os;
    os << std::setprecision(kPrecision);

    os << "{\n  \"name\": \"" << instanceName << "\",\n"
       << "  \"strip_height\": " << request.sheetHeightMm << ",\n"
       // A single global on the instance, not a per-item property.
       << "  \"min_item_separation\": " << request.partSpacingMm << ",\n"
       << "  \"items\": [\n";

    bool first = true;
    for (size_t i = 0; i < profiles.size(); ++i) {
        const Profile& p = profiles[i];
        if (p.outer.size() < 3) continue;

        if (!first) os << ",\n";
        first = false;

        // One item per profile, demand 1. Deduplicating identical parts into
        // demand > 1 is the known performance win, but it also makes several
        // placements share an item_id, so write-back needs a scheme for which
        // occurrence gets which transform. Deferred deliberately.
        os << "    {\"id\": " << i << ", \"demand\": 1, \"orientation\": {\"rotation\": ";
        writeRotation(os, request.rotation);
        os << "}, \"shape\": {\"type\": \"simple_polygon\", \"data\": ";
        writeRing(os, p.outer);
        os << "}}";

        out.itemCount++;
        out.holesDropped += static_cast<int>(p.holes.size());
    }

    os << "\n  ]\n}\n";

    out.json = os.str();
    return out;
}

}  // namespace fusenest
