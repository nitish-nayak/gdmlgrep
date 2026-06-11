#pragma once
// nodeattr.hpp — structured attribute access over a GDML element node: the
// value of an attribute (a real field, or a value_attribute/string_attribute
// child) and the expression node behind a value attribute.
#include "query/grammar.hpp"
#include "document.hpp"

#include <optional>
#include <string>

namespace gg {

// A node's attribute value (quote-stripped): a real field (name/ref), else a
// value_attribute / string_attribute child whose Name matches (x, rmax, unit...).
inline std::optional<std::string> attrValue(const Document &doc, TSNode n, const std::string &field) {
    // A real grammar field first (name=, ref=) ...
    TSNode f = ts_node_child_by_field_name(n, field.c_str(), static_cast<uint32_t>(field.size()));
    if (!ts_node_is_null(f)) return std::string(doc.text(f, true));
    // ... otherwise a generic value/string attribute child whose Name is `field`.
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
        TSNode ch = ts_node_named_child(n, i);
        TSSymbol s = ts_node_symbol(ch);
        if (s != kVALUE && s != kSTRING) continue;

        TSNode name = ts_node_named_child(ch, 0);  // child 0 is the attribute's Name
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
