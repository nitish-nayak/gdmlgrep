#pragma once
// verbs.hpp — the index-based subcommands, which consume the PreWalk reference
// graph directly rather than the path matcher (they need cross-graph
// correlation or non-flat output, so they can't be expressed as queries):
//
//   placement-tree  the geometry placement hierarchy from <world> (or a named
//                   volume), each volume expanded once (shared subtrees
//                   collapsed with "see above")
//   dead-defs       names defined but never referenced
//   dangling        names referenced but never defined
//   find-usages     the sites that reference a given name
//
// dead-defs also scans expression `identifier` nodes, since constants/variables
// are referenced from expressions (value="pi/2.") rather than via a ref field.
#include "query/ts.hpp"
#include "document.hpp"
#include "prewalk.hpp"
#include "utils.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace gg {

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
        std::vector<TSNode> out;
        for (const auto &[name, nodes] : index_.allUses())
            if (index_.definitions(name).empty())
                for (TSNode n : nodes) out.push_back(n);
        emitSorted(out);
    }

    void deadDefs() const {
        std::set<std::string> used;
        collectUsed(doc_.root(), used);
        std::vector<TSNode> out;
        for (const auto &[name, nodes] : index_.allDefs())
            if (index_.uses(name).empty() && !used.count(name))
                for (TSNode n : nodes) out.push_back(n);
        emitSorted(out);
    }

    // Root at <world> by default, or at a named volume when one is given.
    void placementTree(const std::string &root = "") {
        if (!root.empty()) {
            const std::vector<TSNode> &defs = index_.definitions(root);
            if (defs.empty()) {
                std::fprintf(stderr, "gg: no volume named '%s'\n", root.c_str());
                return;
            }
            for (TSNode v : defs) emitNode(v, "", 0, 0);
            return;
        }
        TSNode world = findFirst(doc_.root(), world_);
        if (ts_node_is_null(world)) return;
        auto refField = std::string(index_.fieldText(world, "ref"));
        for (TSNode v : index_.definitions(refField)) emitNode(v, "", 0, 0);
    }

private:
    const Document &doc_;
    const PreWalk &index_;
    bool pretty_;
    TSSymbol volumeref_, physvol_, divisionvol_, replicavol_, paramvol_, world_, identifier_, attribute_;
    std::set<std::string> visited_;  // placement-tree: volumes already expanded

    void emitSorted(const std::vector<TSNode> &nodes) const {
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc_, kv.second, pretty_);
    }

    // The volume/assembly a placement node refers to, via its volumeref child.
    std::string placedName(TSNode placement) const {
        uint32_t c = ts_node_named_child_count(placement);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(placement, i);
            if (ts_node_symbol(ch) == volumeref_) return std::string(index_.fieldText(ch, "ref"));
        }
        return {};
    }

    // Placement hierarchy. With --pretty: tree(1)-style connectors (kind: 0 =
    // root, 1 = mid sibling |--, 2 = last `--). Plain: depth-indented names,
    // no Unicode, so it pipes cleanly. Shared subtrees collapse with "(see above)".
    void emitNode(TSNode logical, const std::string &prefix, int kind, int depth) {
        std::string name = std::string(index_.fieldText(logical, "name"));
        if (pretty_) {
            const char *conn = kind == 0 ? "" : kind == 2 ? "└── " : "├── ";
            std::printf("%s%s%s%s%s", prefix.c_str(), conn, ansi::tag, name.c_str(), ansi::reset);
        } else {
            std::printf("%*s%s", depth * 2, "", name.c_str());
        }
        if (!visited_.insert(name).second) {
            std::printf("%s (see above)%s\n", pretty_ ? ansi::dim : "", pretty_ ? ansi::reset : "");
            return;
        }
        std::printf("\n");
        std::vector<TSNode> placed;
        uint32_t c = ts_node_named_child_count(logical);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(logical, i);
            TSSymbol s = ts_node_symbol(ch);
            if((s != physvol_) && (s != divisionvol_) && (s != replicavol_) && (s != paramvol_))
                continue;
            for (TSNode v : index_.definitions(placedName(ch))) placed.push_back(v);
        }
        std::string childPrefix = prefix + (kind == 0 ? "" : kind == 2 ? "    " : "│   ");
        for (size_t i = 0; i < placed.size(); ++i)
            emitNode(placed[i], childPrefix, i + 1 == placed.size() ? 2 : 1, depth + 1);
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
            if (doc_.text(name) == "ref")
                out.insert(std::string(doc_.text(ts_node_named_child(n, 1), true)));
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
