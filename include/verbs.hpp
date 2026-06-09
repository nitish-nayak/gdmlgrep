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

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace gg {

inline void emitLine(const Document &doc, TSNode n) {
    std::string_view t = doc.text(n);
    t = t.substr(0, t.find('\n'));
    std::printf("%s:%u: %.*s\n", doc.name().c_str(), doc.line(n),
                static_cast<int>(t.size()), t.data());
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
    Verbs(const Document &doc, const PreWalk &index) : doc_(doc), index_(index) {
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
        for (auto &kv : out) emitLine(doc_, kv.second);
    }

    void deadDefs() const {
        std::set<std::string> used;
        collectUsed(doc_.root(), used);
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (const auto &[name, nodes] : index_.allDefs())
            if (index_.uses(name).empty() && !used.count(name))
                for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second);
    }

    void placementTree() {
        TSNode world = findFirst(doc_.root(), world_);
        if (ts_node_is_null(world)) return;
        for (TSNode v : index_.definitions(refField(world))) emitPlacement(v, 0);
    }

private:
    const Document &doc_;
    const PreWalk &index_;
    TSSymbol volumeref_, physvol_, divisionvol_, replicavol_, paramvol_, world_, identifier_, attribute_;
    std::set<std::string> visited_;  // placement-tree: volumes already expanded

    static TSSymbol sym(const TSLanguage *l, const char *n) {
        return ts_language_symbol_for_name(l, n, static_cast<uint32_t>(std::strlen(n)), true);
    }

    void emitSorted(const std::vector<TSNode> &nodes) const {
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second);
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

    void emitPlacement(TSNode logical, int depth) {
        std::string name = nameField(logical);
        std::printf("%*s%s", depth * 2, "", name.c_str());
        if (!visited_.insert(name).second) { std::printf(" (see above)\n"); return; }
        std::printf("\n");
        uint32_t c = ts_node_named_child_count(logical);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(logical, i);
            if (!isPlacement(ts_node_symbol(ch))) continue;
            for (TSNode placed : index_.definitions(placedName(ch)))
                emitPlacement(placed, depth + 1);
        }
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
