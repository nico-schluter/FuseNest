#include "settings.h"

#include "paths.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fusenest {
namespace {

const char* kFileName = "settings.json";

/// Bigger than any legitimate settings file by three orders of magnitude. The
/// cap is here so that a file that is not a settings file at all -- a log, a
/// mesh, a disk full of zeros -- costs a stat and nothing else.
constexpr std::size_t kMaxTextBytes = 64 * 1024;

/// Nested values are skipped rather than parsed, but skipping still recurses,
/// so a file of nothing but open braces needs a floor.
constexpr int kMaxDepth = 32;

// --- a deliberately small JSON reader -------------------------------------
//
// Flat object of scalars, which is all this file ever holds. Nested objects and
// arrays are SKIPPED rather than refused: a newer build may write a key this
// build knows nothing about, and the old build must still read the keys it does
// know. Numbers go through std::from_chars rather than strtod because
// from_chars does not consult the locale -- Fusion runs under the user's, and
// a decimal comma would otherwise turn 2.0 into 2.

struct Value {
    enum class Kind { Number, Bool, String, Null, Skipped } kind = Kind::Null;
    double number = 0.0;
    bool boolean = false;
    std::string text;
};

class Reader {
  public:
    explicit Reader(const std::string& text) : s_(text) {}

    /// False means the text is not a usable JSON object. Partial results are
    /// kept: a file truncated halfway through still yields the keys that
    /// arrived before the truncation.
    bool readObject(std::map<std::string, Value>& out) { return object(out, 0); }

  private:
    const std::string& s_;
    std::size_t i_ = 0;

    bool done() const { return i_ >= s_.size(); }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    void ws() {
        while (!done()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == 0xEF && i_ + 2 < s_.size() &&
                       static_cast<unsigned char>(s_[i_ + 1]) == 0xBB &&
                       static_cast<unsigned char>(s_[i_ + 2]) == 0xBF) {
                // A UTF-8 BOM, which is what a Windows text editor leaves
                // behind. Skipping it costs three bytes and saves a support
                // round trip.
                i_ += 3;
            } else {
                return;
            }
        }
    }

    bool literal(const char* word) {
        const std::size_t n = std::string(word).size();
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    bool string(std::string& out) {
        if (peek() != '"') return false;
        ++i_;
        out.clear();
        while (!done()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (done()) return false;
            const char e = s_[i_++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'u': {
                    // No settings value is ever non-ASCII, so the code point is
                    // consumed and dropped rather than decoded. Four hex digits
                    // or the string is malformed.
                    if (i_ + 4 > s_.size()) return false;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_ + k];
                        const bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                                         (h >= 'A' && h <= 'F');
                        if (!hex) return false;
                    }
                    i_ += 4;
                    out.push_back('?');
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(double& out) {
        const std::size_t start = i_;
        if (peek() == '-' || peek() == '+') ++i_;
        while (!done()) {
            const char c = s_[i_];
            const bool part = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                              ((c == '-' || c == '+') && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E'));
            if (!part) break;
            ++i_;
        }
        if (i_ == start) return false;

        // A stream imbued with the classic locale rather than std::from_chars
        // or strtod. from_chars is the obvious choice and compiles, but libc++
        // marks its floating-point overload unavailable before macOS 26 while
        // the add-in targets 14 -- so it fails only in the real build. strtod
        // reads the decimal separator from the locale, and Fusion runs under
        // the user's, so on a German machine it would stop at the dot in 2.5.
        std::istringstream is(s_.substr(start, i_ - start));
        is.imbue(std::locale::classic());
        double v = 0.0;
        is >> v;
        // eof as well as success: "1.2.3" otherwise yields 1.2 and silently
        // leaves the rest, which would accept nonsense as a value.
        if (is.fail() || !is.eof()) return false;
        out = v;
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        switch (peek()) {
            case '"': {
                out.kind = Value::Kind::String;
                return string(out.text);
            }
            case 't':
                if (!literal("true")) return false;
                out.kind = Value::Kind::Bool;
                out.boolean = true;
                return true;
            case 'f':
                if (!literal("false")) return false;
                out.kind = Value::Kind::Bool;
                out.boolean = false;
                return true;
            case 'n':
                if (!literal("null")) return false;
                out.kind = Value::Kind::Null;
                return true;
            case '{': {
                std::map<std::string, Value> ignored;
                out.kind = Value::Kind::Skipped;
                return object(ignored, depth + 1);
            }
            case '[': {
                out.kind = Value::Kind::Skipped;
                return array(depth + 1);
            }
            default:
                out.kind = Value::Kind::Number;
                return number(out.number);
        }
    }

    bool array(int depth) {
        if (depth > kMaxDepth) return false;
        if (peek() != '[') return false;
        ++i_;
        ws();
        if (peek() == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            Value v;
            if (!value(v, depth)) return false;
            ws();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == ']') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    bool object(std::map<std::string, Value>& out, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (peek() != '{') return false;
        ++i_;
        ws();
        if (peek() == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (peek() != ':') return false;
            ++i_;
            Value v;
            if (!value(v, depth)) return false;
            // Last one wins, which is what every JSON reader does with a
            // duplicate key.
            out[key] = v;
            ws();
            if (peek() == ',') {
                ++i_;
                continue;
            }
            if (peek() == '}') {
                ++i_;
                return true;
            }
            return false;
        }
    }
};

// --- typed reads, each one total -----------------------------------------

void readDouble(const std::map<std::string, Value>& got, const char* key, double lo, double hi,
                double& field, SettingsLoad& load) {
    const auto it = got.find(key);
    if (it == got.end()) return;
    if (it->second.kind != Value::Kind::Number) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": not a number, using the default");
        return;
    }
    const double v = it->second.number;
    if (!std::isfinite(v)) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": not a finite number, using the default");
        return;
    }
    if (v < lo || v > hi) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": out of range, clamped");
        field = v < lo ? lo : hi;
        return;
    }
    field = v;
}

void readInt(const std::map<std::string, Value>& got, const char* key, int lo, int hi, int& field,
             SettingsLoad& load) {
    const auto it = got.find(key);
    if (it == got.end()) return;
    if (it->second.kind != Value::Kind::Number) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": not a number, using the default");
        return;
    }
    const double v = it->second.number;
    if (!std::isfinite(v)) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": not a finite number, using the default");
        return;
    }
    // Rounded rather than truncated, and clamped in double before the cast:
    // 1e300 would otherwise be undefined behaviour on the way to an int.
    const double r = std::nearbyint(v);
    if (r < static_cast<double>(lo) || r > static_cast<double>(hi)) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": out of range, clamped");
        field = r < static_cast<double>(lo) ? lo : hi;
        return;
    }
    field = static_cast<int>(r);
}

void readBool(const std::map<std::string, Value>& got, const char* key, bool& field,
              SettingsLoad& load) {
    const auto it = got.find(key);
    if (it == got.end()) return;
    if (it->second.kind != Value::Kind::Bool) {
        load.recovered = true;
        load.notes.push_back(std::string(key) + ": not true or false, using the default");
        return;
    }
    field = it->second.boolean;
}

/// "normal" or "trace". Anything else is the default, noted.
void readLogLevel(const std::map<std::string, Value>& got, bool& trace, SettingsLoad& load) {
    const auto it = got.find("logLevel");
    if (it == got.end()) return;
    if (it->second.kind != Value::Kind::String) {
        load.recovered = true;
        load.notes.push_back("logLevel: not a string, using the default");
        return;
    }
    if (it->second.text == "trace") {
        trace = true;
        return;
    }
    if (it->second.text == "normal") {
        trace = false;
        return;
    }
    load.recovered = true;
    load.notes.push_back("logLevel: unknown value, using the default");
}

Settings g_current;

}  // namespace

SettingsLoad parseSettings(const std::string& text) {
    SettingsLoad load;
    load.fileFound = true;

    if (text.size() > kMaxTextBytes) {
        load.recovered = true;
        load.notes.push_back("the file is far too large to be a settings file; ignoring it");
        return load;
    }

    std::map<std::string, Value> got;
    Reader reader(text);
    if (!reader.readObject(got)) {
        load.recovered = true;
        // Partial results are kept on purpose: a file truncated by a crash
        // still carries the keys that made it to disk, and recovering six of
        // seven values beats recovering none.
        load.notes.push_back(got.empty() ? "not readable as JSON; using defaults"
                                         : "readable only up to a point; the rest is defaulted");
    }

    // Ranges match the dialog's own limits, so a hand-edited file cannot put
    // the dialog into a state it would refuse. Sheet width 0 is the unbounded
    // strip, which is a real setting rather than a missing one.
    readDouble(got, "sheetHeightMm", 0.001, 100000.0, load.values.sheetHeightMm, load);
    readDouble(got, "sheetWidthMm", 0.0, 100000.0, load.values.sheetWidthMm, load);
    readDouble(got, "partSpacingMm", 0.0, 1000.0, load.values.partSpacingMm, load);
    readInt(got, "rotationIndex", 0, 3, load.values.rotationIndex, load);
    readInt(got, "timeBudgetSeconds", 1, 600, load.values.timeBudgetSeconds, load);
    readInt(got, "workers", 1, 64, load.values.workers, load);
    readBool(got, "captureDiagnostics", load.values.captureDiagnostics, load);
    readLogLevel(got, load.values.traceLog, load);

    return load;
}

std::string formatSettings(const Settings& s) {
    std::ostringstream os;
    // Classic locale, explicitly: the add-in runs inside Fusion under whatever
    // locale the user has, and a decimal comma would write a file that this
    // same code then refuses to read.
    os.imbue(std::locale::classic());
    os.setf(std::ios::fixed);
    os.precision(4);
    os << "{\n"
       << "  \"sheetHeightMm\": " << s.sheetHeightMm << ",\n"
       << "  \"sheetWidthMm\": " << s.sheetWidthMm << ",\n"
       << "  \"partSpacingMm\": " << s.partSpacingMm << ",\n"
       << "  \"rotationIndex\": " << s.rotationIndex << ",\n"
       << "  \"timeBudgetSeconds\": " << s.timeBudgetSeconds << ",\n"
       << "  \"workers\": " << s.workers << ",\n"
       << "  \"captureDiagnostics\": " << (s.captureDiagnostics ? "true" : "false") << ",\n"
       << "  \"logLevel\": \"" << (s.traceLog ? "trace" : "normal") << "\"\n"
       << "}\n";
    return os.str();
}

SettingsLoad loadSettings() {
    SettingsLoad load;  // defaults, fileFound false

    const std::string path = userDataPath(kFileName);
    if (path.empty()) return load;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return load;

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        load.fileFound = true;
        load.recovered = true;
        load.notes.push_back("the settings file could not be measured; using defaults");
        return load;
    }
    if (size > kMaxTextBytes) {
        load.fileFound = true;
        load.recovered = true;
        load.notes.push_back("the settings file is far too large; using defaults");
        return load;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        load.fileFound = true;
        load.recovered = true;
        load.notes.push_back("the settings file could not be opened; using defaults");
        return load;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    // A read that fails partway leaves whatever arrived, and the parser's own
    // partial recovery handles it from there.
    load = parseSettings(buf.str());

    if (load.recovered) {
        // Moved aside rather than left to be overwritten by the next save: if
        // someone hand-edited this file, the thing they typed is the evidence.
        std::error_code moveEc;
        std::filesystem::rename(path, path + ".bad", moveEc);
        load.notes.push_back(moveEc ? "could not move the unusable file aside"
                                    : "the unusable file was kept as settings.json.bad");
    }
    return load;
}

bool saveSettings(const Settings& s) {
    const std::string path = userDataPath(kFileName);
    if (path.empty()) return false;
    if (!ensureDirectory(userDataDir())) return false;

    // Temp then rename, so an interrupted write cannot leave a half-file that
    // the next session would have to recover from.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << formatSettings(s);
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

const Settings& settings() { return g_current; }

void applySettings(const Settings& s) { g_current = s; }

}  // namespace fusenest
