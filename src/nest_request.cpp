#include "nest_request.h"

#include <sstream>
#include <string>

namespace fusenest {

const char* rotationModeLabel(RotationMode mode) {
    switch (mode) {
        case RotationMode::None: return "Fixed (0 degrees)";
        case RotationMode::Half: return "0 / 180 degrees";
        case RotationMode::Quarter: return "90 degree steps";
        default: return "Continuous";
    }
}

std::string NestRequest::describe() const {
    std::ostringstream os;
    os << "parts:        " << parts.size() << "\n"
       << "sheet height: " << sheetHeightMm << " mm\n"
       << "sheet width:  "
       << (sheetWidthMm > 0.0 ? std::to_string(sheetWidthMm) + " mm" : std::string("unbounded"))
       << "\n"
       << "max sheets:   " << maxSheets << "\n"
       << "spacing:      " << partSpacingMm << " mm\n"
       << "rotation:     " << rotationModeLabel(rotation) << "\n"
       << "budget:       " << timeBudgetSeconds << " s\n"
       << "workers:      " << workers << "\n"
       << "verbose log:  " << (verboseLog ? "yes" : "no") << "\n";
    return os.str();
}

}  // namespace fusenest
