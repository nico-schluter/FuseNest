#include "message.h"

namespace fusenest {
namespace {

/// True for the characters a file URL may carry unencoded. Everything else --
/// spaces, #, ?, %, and every byte of a non-ASCII name -- is percent-encoded,
/// because a path is not a URL and the difference only shows up on the one
/// machine whose user directory has an awkward character in it.
bool unreservedInPath(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

const char* kHex = "0123456789ABCDEF";

}  // namespace

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string fileLink(const std::string& path, const std::string& label) {
    if (path.empty()) return htmlEscape(label);

    std::string url = "file://";
    // A POSIX path already starts with the slash that separates the empty
    // authority from the path. A Windows one does not, and its separators are
    // backslashes, which a URL does not accept.
    if (path[0] != '/') url += '/';
    for (const char raw : path) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c == '\\') {
            url += '/';
        } else if (unreservedInPath(c)) {
            url += raw;
        } else {
            url += '%';
            url += kHex[c >> 4];
            url += kHex[c & 0x0F];
        }
    }

    // The URL is percent-encoded, so the only character left in it that HTML
    // cares about is none -- but it goes through the escaper anyway rather than
    // relying on that staying true.
    return "<a href=\"" + htmlEscape(url) + "\">" + htmlEscape(label) + "</a>";
}

Message& Message::line(const std::string& text) { return markupLine(htmlEscape(text)); }

Message& Message::markupLine(const std::string& html) {
    if (!html_.empty()) html_ += "<br>";
    html_ += html;
    return *this;
}

Message& Message::gap() {
    if (!html_.empty()) html_ += "<br>";
    return *this;
}

std::string Message::str() const { return html_; }

}  // namespace fusenest
