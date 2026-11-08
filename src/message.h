// Message box bodies, which are HTML whether or not anyone meant them to be.
//
// Fusion's messageBox is a Qt dialog, and Qt decides per string whether it is
// looking at rich text. A body with no tags in it is plain text and behaves as
// expected. Put ONE tag in -- a link to the log, say -- and the whole body
// becomes markup: every "\n" collapses, and every value interpolated into it is
// parsed. Part names are the hazard. A component called "Frame & Panel" loses
// its ampersand, and one called "Bracket <rev2>" loses half its name, in the
// dialog that exists to tell the user which part went wrong.
//
// So a body is built here or not at all. Message::line escapes; markupLine is
// for strings this header's own helpers produced. The rule is that nothing
// reaches messageBox without having gone through one or the other.
#pragma once

#include <string>

namespace fusenest {

/// Escapes the five characters that matter, so `text` renders as itself.
std::string htmlEscape(const std::string& text);

/// A file:// link to `path`, labelled `label`. Both are escaped, and the path
/// is percent-encoded -- the data directory has a space in it on macOS, and a
/// user's folder may hold a # or a non-ASCII name.
std::string fileLink(const std::string& path, const std::string& label);

/// Accumulates lines and joins them with <br>, since the newlines a plain-text
/// body relied on do nothing once the body is markup.
class Message {
public:
    /// One line of text from anywhere: a part name, a path, a string from
    /// Fusion. Escaped.
    Message& line(const std::string& text);

    /// One line of ready-made markup. Only for strings built by fileLink or by
    /// an explicit literal in our own source -- never for anything that came
    /// from the model, the file system or the Fusion API.
    Message& markupLine(const std::string& html);

    /// A blank line between paragraphs.
    Message& gap();

    /// The body, ready for messageBox. Empty if nothing was added.
    std::string str() const;

private:
    std::string html_;
};

}  // namespace fusenest
