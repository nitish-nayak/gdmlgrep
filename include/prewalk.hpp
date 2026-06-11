#pragma once
// prewalk.hpp — the pre-pass over the tree that builds the reference index the
// MatchEngine consumes for the `=>` / `==>` deref connectors and the find-usages verb.
//
// GDML expresses nesting and composition by *reference*: a definition carries a
// `name` field, and a reference element (volumeref, solidref, materialref,
// positionref, world, first, second, …) carries a `ref` field naming its
// target. One walk — run before matching — builds both directions:
//
//   defs: name -> defining node(s)     (any node with a `name` field)
//   uses: name -> referencing node(s)  (any node with a `ref` field)
//
// GDML names are scoped per category (a position and a volume may share a
// name), so callers that care disambiguate by node type at query time; the
// index keys on the bare name and keeps the node, hence its type.
//
// Scope note: only structural `ref`-field references are captured here.
// Constants referenced inside expressions (via `identifier` nodes, not a `ref`
// field) are resolved separately by expression evaluation, not this index.
#include "query/grammar.hpp"
#include "document.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gg {

// A bare GDML name -> the nodes carrying it (a category may hold several).
using TSNodeIndex = std::unordered_map<std::string, std::vector<TSNode>>;

class PreWalk {
public:
    explicit PreWalk(const Document &doc) : doc(doc) { build(doc.root()); }

    // The defining / referencing nodes for a bare name (the shared empty vector if
    // none). Caller disambiguates by node type when a name spans categories.
    const std::vector<TSNode> &definitions(std::string_view name) const {
        auto it = defs_by_name.find(std::string(name));
        return it == defs_by_name.end() ? empty : it->second;
    }
    const std::vector<TSNode> &uses(std::string_view name) const {
        auto it = uses_by_name.find(std::string(name));
        return it == uses_by_name.end() ? empty : it->second;
    }

    // The whole index, for verbs that sweep every name (unused defs, all usages).
    const TSNodeIndex &allDefs() const { return defs_by_name; }
    const TSNodeIndex &allUses() const { return uses_by_name; }

    // Text of a node's named field (e.g. name="World" -> World), quote-stripped.
    std::string_view fieldText(TSNode n, const char *field) const {
        TSNode f = ts_node_child_by_field_name(n, field, static_cast<uint32_t>(std::strlen(field)));
        if (ts_node_is_null(f)) return {};
        return doc.text(f, true);
    }

private:
    const Document &doc;
    TSNodeIndex defs_by_name;
    TSNodeIndex uses_by_name;
    static inline const std::vector<TSNode> empty{};  // returned by reference on a miss

    // Index every node that carries a name and/or ref field, then recurse. A single
    // node can do both (e.g. a defining element that also references another).
    void build(TSNode n) {
        if (auto name = fieldText(n, "name"); !name.empty()) defs_by_name[std::string(name)].push_back(n);
        if (auto ref  = fieldText(n, "ref");  !ref.empty())  uses_by_name[std::string(ref)].push_back(n);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) build(ts_node_named_child(n, i));
    }
};

}  // namespace gg
