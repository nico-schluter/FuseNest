// What a settings file is allowed to do to a launch: nothing.
//
// settings.json is a debug aid. A debug aid that can stop the add-in from
// loading is worse than not having one, so every case below asserts the same
// shape of outcome -- usable values come back, the damage is reported rather
// than thrown, and nothing is left for a caller to check before it can
// continue. The interesting inputs are the ones a real user produces: a
// half-written file from a crash, a hand-edit with a trailing comma, a value
// typed in the wrong unit, a file a Windows editor saved with a BOM.
#include "paths.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <string>

using namespace fusenest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

bool near(double a, double b, double eps = 1e-6) {
    const double d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

const Settings kDefaults;

/// Every field at its default, which is what every unusable input must produce.
bool allDefault(const Settings& s) {
    return near(s.sheetHeightMm, kDefaults.sheetHeightMm) &&
           near(s.sheetWidthMm, kDefaults.sheetWidthMm) &&
           near(s.partSpacingMm, kDefaults.partSpacingMm) &&
           s.rotationIndex == kDefaults.rotationIndex &&
           s.timeBudgetSeconds == kDefaults.timeBudgetSeconds && s.workers == kDefaults.workers &&
           s.captureDiagnostics == kDefaults.captureDiagnostics && s.traceLog == kDefaults.traceLog;
}

void unusableInputsYieldDefaults() {
    struct Case {
        const char* name;
        const char* text;
    } cases[] = {
        {"empty file", ""},
        {"whitespace only", "   \n\t\r\n "},
        {"not JSON at all", "this is not a settings file"},
        {"a log file by mistake", "[      0] context: {\"IsApplicationStartup\":false}"},
        {"binary rubbish", "\x01\x02\x00\x03\xff\xfe"},
        {"a bare array", "[1, 2, 3]"},
        {"a bare number", "42"},
        {"a bare string", "\"captureDiagnostics\""},
        {"null", "null"},
        {"only an opening brace", "{"},
        {"nothing but braces", "{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{{"},
        {"a key with no value", "{\"workers\":}"},
        {"a value with no key", "{: 4}"},
        {"single quotes", "{'workers': 4}"},
        {"unquoted key", "{workers: 4}"},
    };

    for (const Case& c : cases) {
        const SettingsLoad load = parseSettings(c.text);
        const bool ok = allDefault(load.values) && load.recovered && !load.notes.empty();
        check(ok, c.name);
    }
}

void aGoodFileReadsBack() {
    Settings s;
    s.sheetHeightMm = 1250.5;
    s.sheetWidthMm = 0.0;  // the unbounded strip, a real setting
    s.partSpacingMm = 3.25;
    s.rotationIndex = 2;
    s.timeBudgetSeconds = 45;
    s.workers = 8;
    s.captureDiagnostics = true;
    s.traceLog = true;

    const SettingsLoad load = parseSettings(formatSettings(s));
    check(!load.recovered && load.notes.empty(), "a file we wrote reads back clean");
    check(near(load.values.sheetHeightMm, s.sheetHeightMm) &&
              near(load.values.sheetWidthMm, s.sheetWidthMm) &&
              near(load.values.partSpacingMm, s.partSpacingMm) &&
              load.values.rotationIndex == s.rotationIndex &&
              load.values.timeBudgetSeconds == s.timeBudgetSeconds &&
              load.values.workers == s.workers &&
              load.values.captureDiagnostics == s.captureDiagnostics &&
              load.values.traceLog == s.traceLog,
          "every field survives a round trip");

    // Zero is not absence: a sheet width of 0 asks for the unbounded strip,
    // and defaulting it back to 2000 would silently change the nest.
    check(near(load.values.sheetWidthMm, 0.0), "a zero sheet width is kept, not defaulted");
}

void partialFilesKeepWhatArrived() {
    // A crash during a write, or a disk that filled up mid-line.
    const SettingsLoad load =
        parseSettings("{\n  \"workers\": 8,\n  \"captureDiagnostics\": true,\n  \"sheetHeig");
    check(load.recovered, "a truncated file is reported as recovered");
    check(load.values.workers == 8 && load.values.captureDiagnostics,
          "a truncated file keeps the values that made it to disk");
    check(near(load.values.sheetHeightMm, kDefaults.sheetHeightMm),
          "and defaults the one that was cut in half");

    // A trailing comma is the single most common hand-edit mistake.
    const SettingsLoad trailing = parseSettings("{\"workers\": 6,}");
    check(trailing.recovered && trailing.values.workers == 6,
          "a trailing comma costs the file nothing but a note");
}

void wrongTypesFallBackPerField() {
    const SettingsLoad load = parseSettings(
        "{\"workers\": \"eight\", \"captureDiagnostics\": \"yes\", \"timeBudgetSeconds\": null, "
        "\"logLevel\": 4, \"sheetHeightMm\": true, \"partSpacingMm\": 7.5}");
    check(load.recovered, "wrong types are reported");
    check(load.values.workers == kDefaults.workers &&
              load.values.captureDiagnostics == kDefaults.captureDiagnostics &&
              load.values.timeBudgetSeconds == kDefaults.timeBudgetSeconds &&
              !load.values.traceLog && near(load.values.sheetHeightMm, kDefaults.sheetHeightMm),
          "each wrong type falls back on its own");
    check(near(load.values.partSpacingMm, 7.5),
          "a good field beside five bad ones is still read");
    check(load.notes.size() == 5, "one note per wrong field");
}

void numbersAreClampedNotTrusted() {
    const SettingsLoad high = parseSettings(
        "{\"workers\": 9999, \"timeBudgetSeconds\": 100000, \"sheetHeightMm\": 1e9, "
        "\"partSpacingMm\": -5, \"rotationIndex\": 77}");
    check(high.recovered, "out-of-range values are reported");
    check(high.values.workers == 64 && high.values.timeBudgetSeconds == 600 &&
              near(high.values.sheetHeightMm, 100000.0) && near(high.values.partSpacingMm, 0.0) &&
              high.values.rotationIndex == 3,
          "every out-of-range value is clamped to what the dialog accepts");

    // 1e300 into an int is undefined behaviour if the clamp happens after the
    // cast rather than before it.
    const SettingsLoad absurd = parseSettings("{\"workers\": 1e300, \"rotationIndex\": -1e300}");
    check(absurd.values.workers == 64 && absurd.values.rotationIndex == 0,
          "a value far outside int range clamps without a cast");

    // NaN and Infinity are not JSON, so these are parse failures rather than
    // non-finite values -- but either way nothing non-finite may escape.
    const SettingsLoad nan = parseSettings("{\"sheetHeightMm\": NaN}");
    check(allDefault(nan.values) && nan.recovered, "NaN is not accepted as a number");
    const SettingsLoad inf = parseSettings("{\"workers\": Infinity}");
    check(allDefault(inf.values) && inf.recovered, "Infinity is not accepted as a number");

    const SettingsLoad fractional = parseSettings("{\"workers\": 4.7}");
    check(fractional.values.workers == 5, "a fractional count rounds rather than truncating");

    const SettingsLoad malformed = parseSettings("{\"workers\": 1.2.3}");
    check(malformed.values.workers == kDefaults.workers,
          "a number that only starts out valid is refused whole");
}

void toleratesWhatAnotherBuildMightWrite() {
    const SettingsLoad unknown =
        parseSettings("{\"workers\": 6, \"somethingNewer\": 12, \"nested\": {\"a\": [1, {\"b\": "
                      "2}]}, \"list\": [1, 2, 3], \"captureDiagnostics\": true}");
    check(!unknown.recovered && unknown.notes.empty(),
          "unknown keys, nested objects and arrays are not errors");
    check(unknown.values.workers == 6 && unknown.values.captureDiagnostics,
          "and the keys this build knows are still read past them");

    const SettingsLoad bom = parseSettings("\xEF\xBB\xBF{\"workers\": 3}");
    check(!bom.recovered && bom.values.workers == 3, "a UTF-8 BOM is skipped");

    const SettingsLoad dup = parseSettings("{\"workers\": 2, \"workers\": 9}");
    check(dup.values.workers == 9, "a duplicate key takes the last value");

    const SettingsLoad escapes = parseSettings("{\"logLevel\": \"tr\\u0061ce\"}");
    check(escapes.recovered && !escapes.values.traceLog,
          "an escape we do not decode defaults the field rather than guessing");

    const SettingsLoad empty = parseSettings("{}");
    check(!empty.recovered && allDefault(empty.values), "an empty object is a valid file");
}

void oversizedFilesAreIgnoredCheaply() {
    const SettingsLoad load = parseSettings(std::string(1024 * 1024, 'x'));
    check(allDefault(load.values) && load.recovered, "a megabyte of rubbish is ignored");
}

void logLevelIsTheOnlyStringField() {
    check(parseSettings("{\"logLevel\": \"trace\"}").values.traceLog, "logLevel trace is read");
    check(!parseSettings("{\"logLevel\": \"normal\"}").values.traceLog, "logLevel normal is read");
    const SettingsLoad odd = parseSettings("{\"logLevel\": \"verbose\"}");
    check(!odd.values.traceLog && odd.recovered, "an unknown logLevel defaults and is noted");
}

void writingIsLocaleIndependent() {
    // Fusion runs under the user's locale. If the writer used it, a German
    // machine would write "2,0" and this same code would then refuse to read
    // its own file.
    const char* locales[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8"};
    bool tried = false;
    for (const char* name : locales) {
        try {
            std::locale::global(std::locale(name));
            tried = true;
            break;
        } catch (const std::exception&) {
            continue;
        }
    }
    if (!tried) {
        std::printf("skip  no comma-decimal locale installed to test against\n");
        return;
    }

    Settings s;
    s.partSpacingMm = 2.5;
    s.sheetHeightMm = 1234.5;
    const SettingsLoad load = parseSettings(formatSettings(s));
    check(!load.recovered && near(load.values.partSpacingMm, 2.5) &&
              near(load.values.sheetHeightMm, 1234.5),
          "a round trip under a comma-decimal locale is unaffected");
    std::locale::global(std::locale::classic());
}

/// The one case that needs a real directory: no file at all must not read as
/// damage, and the file we write must be the file we read.
void theFileOnDiskBehaves() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "fusenest_settings_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Set before the first userDataDir() call: the answer is cached for the
    // life of the process, which is what the add-in wants and what a test has
    // to work around.
#ifdef _WIN32
    _putenv_s("FUSENEST_DATA_DIR", dir.string().c_str());
#else
    setenv("FUSENEST_DATA_DIR", dir.string().c_str(), 1);
#endif

    check(!userDataDir().empty(), "the data directory is created on demand");

    const SettingsLoad fresh = loadSettings();
    check(!fresh.fileFound && !fresh.recovered && allDefault(fresh.values),
          "a first run finds no file and reports no damage");

    Settings s;
    s.workers = 11;
    s.captureDiagnostics = true;
    check(saveSettings(s), "settings can be written");

    const SettingsLoad back = loadSettings();
    check(back.fileFound && !back.recovered && back.values.workers == 11 &&
              back.values.captureDiagnostics,
          "and read back");

    check(!std::filesystem::exists(userDataPath("settings.json.tmp"), ec),
          "the atomic write leaves no temp file behind");

    // A corrupt file is kept as evidence rather than quietly replaced.
    {
        std::ofstream f(userDataPath("settings.json"), std::ios::trunc);
        f << "{\"workers\": ";
    }
    const SettingsLoad broken = loadSettings();
    check(broken.recovered && broken.values.workers == kDefaults.workers,
          "a corrupt file loads as defaults");
    check(std::filesystem::exists(userDataPath("settings.json.bad"), ec),
          "and is moved aside as settings.json.bad");

    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    std::printf("-- unusable input --\n");
    unusableInputsYieldDefaults();
    std::printf("-- round trip --\n");
    aGoodFileReadsBack();
    std::printf("-- partial files --\n");
    partialFilesKeepWhatArrived();
    std::printf("-- wrong types --\n");
    wrongTypesFallBackPerField();
    std::printf("-- ranges --\n");
    numbersAreClampedNotTrusted();
    std::printf("-- forward compatibility --\n");
    toleratesWhatAnotherBuildMightWrite();
    std::printf("-- size --\n");
    oversizedFilesAreIgnoredCheaply();
    std::printf("-- logLevel --\n");
    logLevelIsTheOnlyStringField();
    std::printf("-- locale --\n");
    writingIsLocaleIndependent();
    std::printf("-- on disk --\n");
    theFileOnDiskBehaves();

    std::printf("%s\n", g_failures ? "FAILURES" : "all passed");
    return g_failures;
}
