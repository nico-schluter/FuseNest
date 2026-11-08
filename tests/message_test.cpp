// Checks that a part name survives the trip into a message box.
//
// This file exists because of a Fusion behaviour that is invisible until it
// bites: a message body containing any tag is parsed as HTML. Every check here
// is about a character a user can legally put in a component name, or that
// macOS can put in a path, arriving in the dialog looking like itself.
#include "message.h"

#include <cstdio>
#include <string>

using namespace fusenest;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++g_failures;
}

void checkEq(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    std::printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) {
        std::printf("        want: %s\n        got:  %s\n", want.c_str(), got.c_str());
        ++g_failures;
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void escaping() {
    std::printf("\n-- escaping --\n");
    checkEq(htmlEscape(""), "", "empty text stays empty");
    checkEq(htmlEscape("Bracket"), "Bracket", "an ordinary name is untouched");
    checkEq(htmlEscape("Frame & Panel"), "Frame &amp; Panel", "an ampersand survives");
    checkEq(htmlEscape("Bracket <rev2>"), "Bracket &lt;rev2&gt;",
            "angle brackets survive, which is the name that would otherwise vanish");
    checkEq(htmlEscape("5\" plate"), "5&quot; plate", "a quote survives");
    checkEq(htmlEscape("Bob's part"), "Bob&#39;s part", "an apostrophe survives");
    checkEq(htmlEscape("&amp;"), "&amp;amp;",
            "already-escaped text is escaped again rather than passed through");
    checkEq(htmlEscape("a&b<c>d\"e'f"), "a&amp;b&lt;c&gt;d&quot;e&#39;f",
            "all five at once, in order");
    // A name is not ASCII just because the developer's is.
    checkEq(htmlEscape("Trägerplatte"), "Trägerplatte", "non-ASCII passes through unchanged");
    checkEq(htmlEscape("<script>alert(1)</script>"),
            "&lt;script&gt;alert(1)&lt;/script&gt;", "a tag-shaped name is inert");
}

void links() {
    std::printf("\n-- file links --\n");
    checkEq(fileLink("/tmp/fusenest.log", "fusenest.log"),
            "<a href=\"file:///tmp/fusenest.log\">fusenest.log</a>",
            "a plain POSIX path makes a three-slash file URL");

    // The real one. macOS puts the add-in's data under a directory with a
    // space in it, so this is not a corner case, it is every single user.
    checkEq(fileLink("/Users/x/Library/Application Support/FuseNest/fusenest.log", "the log"),
            "<a href=\"file:///Users/x/Library/Application%20Support/FuseNest/fusenest.log\">"
            "the log</a>",
            "a space in the path is percent-encoded");

    check(contains(fileLink("/tmp/a#b/c.log", "x"), "/tmp/a%23b/c.log"),
          "a hash is encoded rather than starting a fragment");
    check(contains(fileLink("/tmp/a?b.log", "x"), "/tmp/a%3Fb.log"),
          "a question mark is encoded rather than starting a query");
    check(contains(fileLink("/tmp/100%/x.log", "x"), "/tmp/100%25/x.log"),
          "a percent sign is encoded rather than introducing an escape");
    check(contains(fileLink("/Users/José/x.log", "x"), "/Users/Jos%C3%A9/x.log"),
          "a non-ASCII path is encoded as its UTF-8 bytes");

    checkEq(fileLink("C:\\Users\\x\\FuseNest\\fusenest.log", "the log"),
            "<a href=\"file:///C:/Users/x/FuseNest/fusenest.log\">the log</a>",
            "a Windows path gets the leading slash and forward separators");

    checkEq(fileLink("/tmp/x.log", "Frame & Panel"),
            "<a href=\"file:///tmp/x.log\">Frame &amp; Panel</a>",
            "the label is escaped even though the path is encoded");
    checkEq(fileLink("", "nowhere"), "nowhere",
            "no path means no link rather than a link to file:///");
}

void bodies() {
    std::printf("\n-- bodies --\n");
    checkEq(Message().str(), "", "an empty message is empty, not a stray break");
    checkEq(Message().line("one").str(), "one", "a single line carries no break");
    checkEq(Message().line("one").line("two").str(), "one<br>two",
            "lines are joined with br, because the newline they used to rely on does nothing");
    checkEq(Message().line("one").gap().line("two").str(), "one<br><br>two",
            "a gap is one blank line");
    checkEq(Message().gap().line("one").str(), "one",
            "a leading gap does not indent the first line");

    checkEq(Message().line("Cannot nest:").line("  Frame & Panel: open profile").str(),
            "Cannot nest:<br>  Frame &amp; Panel: open profile",
            "a blocker line escapes the part name it names");

    checkEq(Message().markupLine(fileLink("/tmp/x.log", "fusenest.log")).str(),
            "<a href=\"file:///tmp/x.log\">fusenest.log</a>",
            "markupLine passes a built link through intact");

    checkEq(Message().line("<b>not bold</b>").str(), "&lt;b&gt;not bold&lt;/b&gt;",
            "line never trusts its argument, however tag-like");
}

}  // namespace

int main() {
    escaping();
    links();
    bodies();
    std::printf("\n%s\n", g_failures ? "FAILURES" : "all passed");
    return g_failures ? 1 : 0;
}
