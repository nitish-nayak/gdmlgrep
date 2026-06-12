#pragma once
// verbs.hpp — the index-based subcommands, which consume the PreWalk reference
// graph directly rather than the path matcher (they need cross-graph
// correlation or non-flat output, so they can't be expressed as queries):
//
//   placement-tree  the geometry placement hierarchy from <world> (or a named
//                   volume), each volume expanded once (shared subtrees
//                   collapsed with "repeated")
//   dead-defs       names defined but never referenced
//   dangling        names referenced but never defined
//   find-usages     the sites that reference a given name
//
// dead-defs also scans expression `identifier` nodes, since constants/variables
// are referenced from expressions (value="pi/2.") rather than via a ref field.
#include "query/grammar.hpp"
#include "document.hpp"
#include "prewalk.hpp"
#include "utils/output.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace gg {

class Verbs {
public:
    Verbs(const Document &doc, const PreWalk &index, bool pretty = false)
        : doc(doc), index(index), pretty(pretty) {}

    void find_usages(const std::string &name) const { emit_sorted(index.uses(name)); }

    void dangling() const {
        std::vector<TSNode> out;
        for (const auto &[name, nodes] : index.allUses())
            if (index.definitions(name).empty())
                for (TSNode n : nodes) out.push_back(n);
        emit_sorted(out);
    }

    void dead_defs() const {
        std::set<std::string> used;
        collect_used(doc.root(), used);
        std::vector<TSNode> out;
        for (const auto &[name, nodes] : index.allDefs())
            if (index.uses(name).empty() && !used.count(name))
                for (TSNode n : nodes) out.push_back(n);
        emit_sorted(out);
    }

    // Root at <world> by default, or at a named volume when one is given.
    void placement_tree(const std::string &root = "") {
        if (!root.empty()) {
            const std::vector<TSNode> &defs = index.definitions(root);
            if (defs.empty()) {
                std::fprintf(stderr, "gg: no volume named '%s'\n", root.c_str());
                return;
            }
            for (TSNode v : defs) emit_node(v, "", 0, 0);
            return;
        }
        TSNode world = find_first(doc.root(), kWORLD.id());
        if (ts_node_is_null(world)) return;
        auto ref_field = std::string(index.fieldText(world, "ref"));
        for (TSNode v : index.definitions(ref_field)) emit_node(v, "", 0, 0);
    }

private:
    const Document &doc;
    const PreWalk &index;
    bool pretty;
    std::set<std::string> visited;  // placement-tree: volumes already expanded

    // Dedup by source range (document order), then emit each.
    void emit_sorted(const std::vector<TSNode> &nodes) const {
        std::map<std::pair<uint32_t, uint32_t>, TSNode> out;
        for (TSNode n : nodes) out.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        for (auto &kv : out) emitLine(doc, kv.second, pretty);
    }

    // The volume/assembly a placement node refers to, via its volumeref child.
    std::string placed_name(TSNode placement) const {
        uint32_t c = ts_node_named_child_count(placement);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(placement, i);
            if (ts_node_symbol(ch) == kVOLUMEREF) return std::string(index.fieldText(ch, "ref"));
        }
        return {};
    }

    // Placement hierarchy. kind: 0 root, 1 mid-sibling ├──, 2 last └── (for
    // --pretty; plain mode indents by depth). Shared subtrees collapse "(repeated)".
    void emit_node(TSNode logical, const std::string &prefix, int kind, int depth) {
        std::string name = std::string(index.fieldText(logical, "name"));
        if (pretty) {
            const char *conn = kind == 0 ? "" : kind == 2 ? "└── " : "├── ";
            std::printf("%s%s%s%s%s", prefix.c_str(), conn, ansi::tag, name.c_str(), ansi::reset);
        } else {
            std::printf("%*s%s", depth * 2, "", name.c_str());
        }
        if (!visited.insert(name).second) {
            std::printf("%s (repeated)%s\n", pretty ? ansi::dim : "", pretty ? ansi::reset : "");
            return;
        }
        std::printf("\n");
        std::vector<TSNode> placed;
        uint32_t c = ts_node_named_child_count(logical);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(logical, i);
            TSSymbol s = ts_node_symbol(ch);
            if((s != kPHYSVOL) && (s != kDIVISIONVOL) && (s != kREPLICAVOL) && (s != kPARAMVOL))
                continue;
            for (TSNode v : index.definitions(placed_name(ch))) placed.push_back(v);
        }
        std::string child_prefix = prefix + (kind == 0 ? "" : kind == 2 ? "    " : "│   ");
        for (size_t i = 0; i < placed.size(); ++i)
            emit_node(placed[i], child_prefix, i + 1 == placed.size() ? 2 : 1, depth + 1);
    }

    // Names referenced outside captured `ref` fields: expression identifiers
    // (value="pi/2.") and generic ref= attributes (<fraction ref="U235">).
    void collect_used(TSNode n, std::set<std::string> &out) const {
        TSSymbol s = ts_node_symbol(n);
        if (s == kIDENTIFIER) {
            out.insert(std::string(doc.text(n)));
        } else if (s == kATTRIBUTE && ts_node_named_child_count(n) >= 2) {
            TSNode name = ts_node_named_child(n, 0);
            if (doc.text(name) == "ref")
                out.insert(std::string(doc.text(ts_node_named_child(n, 1), true)));
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) collect_used(ts_node_named_child(n, i), out);
    }

    // First node of type s in n's subtree (preorder), or a null node.
    TSNode find_first(TSNode n, TSSymbol s) const {
        if (ts_node_symbol(n) == s) return n;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode r = find_first(ts_node_named_child(n, i), s);
            if (!ts_node_is_null(r)) return r;
        }
        return TSNode{};
    }
};

}  // namespace gg
