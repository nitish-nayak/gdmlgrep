#pragma once

#include "query/ts.hpp"
#include "document.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace gg {

namespace ansi {
constexpr const char *reset = "\033[0m", *dim = "\033[2m", *path = "\033[35m",
                     *lineno = "\033[32m", *tag = "\033[1;33m", *attr = "\033[36m",
                     *value = "\033[32m";
}

// Light syntax highlight of one GDML element line: dim punctuation, bold tag
// name, cyan attribute names, green quoted values.
inline std::string highlightGdml(std::string_view s) {
    auto ident = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':'; };
    std::string out;
    for (size_t i = 0, n = s.size(); i < n;) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && s[j] != c) ++j;
            if (j < n) ++j;  // include closing quote
            out += ansi::value; out.append(s.substr(i, j - i)); out += ansi::reset;
            i = j;
        } else if (c == '<') {
            out += ansi::dim; out += '<'; ++i;
            if (i < n && s[i] == '/') { out += '/'; ++i; }
            out += ansi::reset;
            size_t j = i;
            while (j < n && ident(s[j])) ++j;
            if (j > i) { out += ansi::tag; out.append(s.substr(i, j - i)); out += ansi::reset; i = j; }
        } else if (c == '>' || c == '/' || c == '=') {
            out += ansi::dim; out += c; out += ansi::reset; ++i;
        } else if (ident(c)) {
            size_t j = i;
            while (j < n && ident(s[j])) ++j;
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            if (k < n && s[k] == '=') { out += ansi::attr; out.append(s.substr(i, j - i)); out += ansi::reset; }
            else out.append(s.substr(i, j - i));
            i = j;
        } else {
            out += c; ++i;
        }
    }
    return out;
}

inline void emitLine(const Document &doc, TSNode n, bool pretty = false) {
    std::string_view t = doc.text(n);
    t = t.substr(0, t.find('\n'));
    if (!pretty) {
        std::printf("%s:%u: %.*s\n", doc.name().c_str(), doc.line(n),
                    static_cast<int>(t.size()), t.data());
        return;
    }
    std::printf("%s%s%s:%s%u%s: %s\n", ansi::path, doc.name().c_str(), ansi::reset,
                ansi::lineno, doc.line(n), ansi::reset, highlightGdml(t).c_str());
}

// A node's attribute value (quote-stripped): a real field (name/ref), else a
// value_attribute / string_attribute child whose Name matches (x, rmax, unit...).
inline std::optional<std::string> attrValue(const Document &doc, TSNode n, const std::string &field) {
    TSNode f = ts_node_child_by_field_name(n, field.c_str(), static_cast<uint32_t>(field.size()));
    if (!ts_node_is_null(f)) return std::string(doc.text(f, true));
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
        TSNode ch = ts_node_named_child(n, i);
        TSSymbol s = ts_node_symbol(ch);
        if (s != kVALUE && s != kSTRING) continue;
        TSNode name = ts_node_named_child(ch, 0);
        if (ts_node_is_null(name) || doc.text(name) != field) continue;
        TSNode val = ts_node_child_by_field_name(ch, "value", 5);
        if (!ts_node_is_null(val)) return std::string(doc.text(val, true));
    }
    return std::nullopt;
}

// The expression node inside node's `field` value attribute (a value_attribute
// whose gdml_value wraps an expression), or a null node if there isn't one.
inline TSNode valueExprNode(const Document &doc, TSNode n, const std::string &field) {
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
        TSNode ch = ts_node_named_child(n, i);
        if (ts_node_symbol(ch) != kVALUE) continue;
        TSNode name = ts_node_named_child(ch, 0);
        if (ts_node_is_null(name) || doc.text(name) != field) continue;
        TSNode val = ts_node_child_by_field_name(ch, "value", 5);  // gdml_value
        if (!ts_node_is_null(val) && ts_node_named_child_count(val) > 0)
            return ts_node_named_child(val, 0);
        return TSNode{};
    }
    return TSNode{};
}

// The value of a node's field for `-o`: "type" -> the element type; otherwise
// the attribute value (attrValue).
inline std::optional<std::string> fieldOf(const Document &doc, TSNode n, const std::string &field) {
    if (field == "type") return std::string(ts_node_type(n));
    return attrValue(doc, n, field);
}

}  // namespace gg
