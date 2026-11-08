#include "nest_request.h"

#include <locale>
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
    // Classic locale so the numbers in the journal read the same whatever
    // Fusion is running under -- a log with "400,000 mm" in it invites the
    // reader to guess which half is the fraction.
    os.imbue(std::locale::classic());
    os << "parts:        " << parts.size() << "\n"
       << "sheet height: " << sheetHeightMm << " mm\n"
       << "sheet width:  ";
    // Through the stream, not std::to_string: that always gives six decimals,
    // so the width printed as 400.000000 next to a height of 400.
    if (sheetWidthMm > 0.0)
        os << sheetWidthMm << " mm";
    else
        os << "unbounded";
    os << "\n"
       << "spacing:      " << partSpacingMm << " mm\n"
       << "rotation:     " << rotationModeLabel(rotation) << "\n"
       << "budget:       " << timeBudgetSeconds << " s\n"
       << "workers:      " << workers << "\n"
       << "diagnostics:  " << (captureDiagnostics ? "capturing" : "off") << "\n";
    return os.str();
}

}  // namespace fusenest
