#pragma once
// verbs.hpp — the index-based subcommands, which consume the PreWalk reference
// graph directly rather than the path matcher (they need cross-graph
// correlation or non-flat output, so they can't be expressed as queries):
//
//   placement-tree  the geometry placement hierarchy from <world>, each volume
//                   expanded once (shared subtrees collapsed with "see above")
//   dead-defs       names defined but never referenced
//   dangling        names referenced but never defined
//   find-usages     the sites that reference a given name
//
// dead-defs also scans expression `identifier` nodes, since constants/variables
// are referenced from expressions (value="pi/2.") rather than via a ref field.
#include "document.hpp"
#include "prewalk.hpp"
#include "ts.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
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

inline std::string stripQuotes(std::string_view t) {
    if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
    return std::string(t);
}

// The value of a node's field for `-o`: "type" -> the element type; a real
// field (name/ref); else a value/string_attribute child (x, rmax, unit, ...).
inline std::optional<std::string> fieldOf(const Document &doc, TSNode n, const std::string &field) {
    if (field == "type") return std::string(ts_node_type(n));
    static const TSLanguage *lang = tree_sitter_gdml();
    static TSSymbol va = ts_language_symbol_for_name(lang, "value_attribute", 15, true);
    static TSSymbol sa = ts_language_symbol_for_name(lang, "string_attribute", 16, true);
    TSNode f = ts_node_child_by_field_name(n, field.c_str(), static_cast<uint32_t>(field.size()));
    if (!ts_node_is_null(f)) return stripQuotes(doc.text(f));
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
        TSNode ch = ts_node_named_child(n, i);
        TSSymbol s = ts_node_symbol(ch);
        if (s != va && s != sa) continue;
        TSNode name = ts_node_named_child(ch, 0);
        if (ts_node_is_null(name) || doc.text(name) != field) continue;
        TSNode val = ts_node_child_by_field_name(ch, "value", 5);
        if (!ts_node_is_null(val)) return stripQuotes(doc.text(val));
    }
    return std::nullopt;
}

class Verbs {
public:
    Verbs(const Document &doc, const PreWalk &index, bool pretty = false)
        : doc_(doc), index_(index), pretty_(pretty) {
        const TSLanguage *lang = tree_sitter_gdml();
        volumeref_ = sym(lang, "volumeref");
        physvol_ = sym(lang, "physvol");
        divisionvol_ = sym(lang, "divisionvol");
        replicavol_ = sym(lang, "replicavol");
        paramvol_ = sym(lang, "paramvol");
        world_ = sym(lang, "world");
        identifier_ = sym(lang, "identifier");
        attribute_ = sym(lang, "Attribute");
    }

    void findUsages(const std::string &name) const { emitSorted(index_.uses(name)); }

    void dangling() const {
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (const auto &[name, nodes] : index_.allUses())
            if (index_.definitions(name).empty())
                for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second, pretty_);
    }

    void deadDefs() const {
        std::set<std::string> used;
        collectUsed(doc_.root(), used);
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (const auto &[name, nodes] : index_.allDefs())
            if (index_.uses(name).empty() && !used.count(name))
                for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second, pretty_);
    }

    void placementTree() {
        TSNode world = findFirst(doc_.root(), world_);
        if (ts_node_is_null(world)) return;
        for (TSNode v : index_.definitions(refField(world))) emitNode(v, "", 0);
    }

private:
    const Document &doc_;
    const PreWalk &index_;
    bool pretty_;
    TSSymbol volumeref_, physvol_, divisionvol_, replicavol_, paramvol_, world_, identifier_, attribute_;
    std::set<std::string> visited_;  // placement-tree: volumes already expanded

    static TSSymbol sym(const TSLanguage *l, const char *n) {
        return ts_language_symbol_for_name(l, n, static_cast<uint32_t>(std::strlen(n)), true);
    }

    void emitSorted(const std::vector<TSNode> &nodes) const {
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second, pretty_);
    }

    std::string fieldText(TSNode n, const char *field) const {
        TSNode f = ts_node_child_by_field_name(n, field, static_cast<uint32_t>(std::strlen(field)));
        if (ts_node_is_null(f)) return {};
        std::string_view t = doc_.text(f);
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
        return std::string(t);
    }
    std::string nameField(TSNode n) const { return fieldText(n, "name"); }
    std::string refField(TSNode n) const { return fieldText(n, "ref"); }

    bool isPlacement(TSSymbol s) const {
        return s == physvol_ || s == divisionvol_ || s == replicavol_ || s == paramvol_;
    }

    // The volume/assembly a placement node refers to, via its volumeref child.
    std::string placedName(TSNode placement) const {
        uint32_t c = ts_node_named_child_count(placement);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(placement, i);
            if (ts_node_symbol(ch) == volumeref_) return refField(ch);
        }
        return {};
    }

    // tree-style placement hierarchy. kind: 0 = root, 1 = mid sibling (|--),
    // 2 = last sibling (`--). Shared subtrees collapse with "(see above)".
    void emitNode(TSNode logical, const std::string &prefix, int kind) {
        const char *conn = kind == 0 ? "" : kind == 2 ? "└── " : "├── ";
        std::string name = nameField(logical);
        if (pretty_) std::printf("%s%s%s%s%s", prefix.c_str(), conn, ansi::tag, name.c_str(), ansi::reset);
        else std::printf("%s%s%s", prefix.c_str(), conn, name.c_str());
        if (!visited_.insert(name).second) {
            std::printf("%s (see above)%s\n", pretty_ ? ansi::dim : "", pretty_ ? ansi::reset : "");
            return;
        }
        std::printf("\n");
        std::vector<TSNode> placed;
        uint32_t c = ts_node_named_child_count(logical);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(logical, i);
            if (!isPlacement(ts_node_symbol(ch))) continue;
            for (TSNode v : index_.definitions(placedName(ch))) placed.push_back(v);
        }
        std::string childPrefix = prefix + (kind == 0 ? "" : kind == 2 ? "    " : "│   ");
        for (size_t i = 0; i < placed.size(); ++i)
            emitNode(placed[i], childPrefix, i + 1 == placed.size() ? 2 : 1);
    }

    // Names referenced by something other than a captured `ref` field:
    // expression identifiers (value="pi/2.") and generic `ref` attributes on
    // non-ref-element tags (<fraction ref="U235">, <composite ref=...>).
    void collectUsed(TSNode n, std::set<std::string> &out) const {
        TSSymbol s = ts_node_symbol(n);
        if (s == identifier_) {
            out.insert(std::string(doc_.text(n)));
        } else if (s == attribute_ && ts_node_named_child_count(n) >= 2) {
            TSNode name = ts_node_named_child(n, 0);
            if (doc_.text(name) == "ref") {
                std::string_view v = doc_.text(ts_node_named_child(n, 1));
                if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
                out.insert(std::string(v));
            }
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) collectUsed(ts_node_named_child(n, i), out);
    }

    TSNode findFirst(TSNode n, TSSymbol s) const {
        if (ts_node_symbol(n) == s) return n;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode r = findFirst(ts_node_named_child(n, i), s);
            if (!ts_node_is_null(r)) return r;
        }
        return TSNode{};
    }
};

}  // namespace gg
