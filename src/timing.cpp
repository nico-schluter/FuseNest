#include "timing.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace fusenest {
namespace {

struct Span {
    long count = 0;
    double totalMs = 0.0;
    double maxMs = 0.0;
};

std::map<std::string, Span> g_spans;

}  // namespace

void timingAdd(const char* name, double ms) {
    Span& s = g_spans[name];
    ++s.count;
    s.totalMs += ms;
    s.maxMs = std::max(s.maxMs, ms);
}

void timingReset() { g_spans.clear(); }

std::string timingReport() {
    if (g_spans.empty()) return "timings: nothing recorded";

    std::vector<std::pair<std::string, Span>> rows(g_spans.begin(), g_spans.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

    std::ostringstream os;
    os << "timings (total ms, calls, avg, max):";
    for (const auto& [name, s] : rows) {
        os << "\n  " << name << ": " << static_cast<long>(s.totalMs) << " ms, " << s.count
           << " calls, avg " << static_cast<long>(s.totalMs / s.count) << " ms, max "
           << static_cast<long>(s.maxMs) << " ms";
    }
    return os.str();
}

}  // namespace fusenest
