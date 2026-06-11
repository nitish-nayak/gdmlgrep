#pragma once
// output.hpp — terminal output for matches: ANSI color codes, a light GDML
// syntax highlighter, and the grep-style `path:line: text` line emitter.
#include "document.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

namespace gg {

// SGR color escapes for the pretty (-p) output.
namespace ansi {
constexpr const char *reset = "\033[0m", *dim = "\033[2m", *path = "\033[35m",
                     *lineno = "\033[32m", *tag = "\033[1;33m", *attr = "\033[36m",
                     *value = "\033[32m";
}

// Light syntax highlight of one GDML element line: dim punctuation, bold tag
// name, cyan attribute names, green quoted values.
inline std::string highlightGdml(std::string_view s) {
    auto ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
    };
    std::string out;
    for (size_t i = 0, n = s.size(); i < n;) {
        char c = s[i];
        if (c == '"' || c == '\'') {  // quoted value, up to the matching quote
            size_t j = i + 1;
            while (j < n && s[j] != c) ++j;
            if (j < n) ++j;  // include closing quote
            out += ansi::value; out.append(s.substr(i, j - i)); out += ansi::reset;
            i = j;
        } else if (c == '<') {  // '<' or '</' followed by the tag name
            out += ansi::dim; out += '<'; ++i;
            if (i < n && s[i] == '/') { out += '/'; ++i; }
            out += ansi::reset;
            size_t j = i;
            while (j < n && ident(s[j])) ++j;
            if (j > i) {
                out += ansi::tag; out.append(s.substr(i, j - i)); out += ansi::reset; i = j;
            }
        } else if (c == '>' || c == '/' || c == '=') {  // structural punctuation
            out += ansi::dim; out += c; out += ansi::reset; ++i;
        } else if (ident(c)) {  // bare word: an attribute name iff '=' follows, else plain text
            size_t j = i;
            while (j < n && ident(s[j])) ++j;
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < n && s[k] == '=') {
                out += ansi::attr; out.append(s.substr(i, j - i)); out += ansi::reset;
            }
            else
                out.append(s.substr(i, j - i));
            i = j;
        } else {
            out += c; ++i;
        }
    }
    return out;
}

// Print one matched node as a grep-style `path:line: text` line; pretty adds color
// and syntax highlighting. Only the node's first source line is shown.
inline void emitLine(const Document &doc, TSNode n, bool pretty = false) {
    std::string_view t = doc.text(n);
    t = t.substr(0, t.find('\n'));  // a multi-line element collapses to its opening line
    if (!pretty) {
        std::printf("%s:%u: %.*s\n", doc.get_name().c_str(), doc.line(n),
                    static_cast<int>(t.size()), t.data());
        return;
    }

    std::printf("%s%s%s:%s%u%s: %s\n", ansi::path, doc.get_name().c_str(), ansi::reset,
                ansi::lineno, doc.line(n), ansi::reset, highlightGdml(t).c_str());
}

}  // namespace gg
