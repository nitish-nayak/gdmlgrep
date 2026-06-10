#pragma once
// prewalk.hpp — the pre-pass over the tree that builds the reference index the
// MatchEngine consumes for the `=>` / `==>` deref axes and the find-usages verb.
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
#include "query/ast.hpp"
#include "document.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gg {

class PreWalk {
public:
    explicit PreWalk(const Document &doc) : doc_(doc) { build(doc.root()); }

    const std::vector<TSNode> &definitions(std::string_view name) const { return lookup(defs_, name); }
    const std::vector<TSNode> &uses(std::string_view name)        const { return lookup(uses_, name); }

    const std::unordered_map<std::string, std::vector<TSNode>> &allDefs() const { return defs_; }
    const std::unordered_map<std::string, std::vector<TSNode>> &allUses() const { return uses_; }

    // Text of a node's named field (e.g. name="World" -> World), quote-stripped.
    std::string_view fieldText(TSNode n, const char *field) const {
        TSNode f = ts_node_child_by_field_name(n, field, (uint32_t)std::strlen(field));
        if (ts_node_is_null(f)) return {};
        return doc_.text(f, true);
    }


private:
    const Document &doc_;
    std::unordered_map<std::string, std::vector<TSNode>> defs_;
    std::unordered_map<std::string, std::vector<TSNode>> uses_;
    static inline const std::vector<TSNode> empty_{};

    void build(TSNode n) {
        if (auto name = fieldText(n, "name"); !name.empty()) defs_[std::string(name)].push_back(n);
        if (auto ref  = fieldText(n, "ref");  !ref.empty())  uses_[std::string(ref)].push_back(n);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) build(ts_node_named_child(n, i));
    }

    static const std::vector<TSNode> &lookup(
        const std::unordered_map<std::string, std::vector<TSNode>> &m, std::string_view k) {
        auto it = m.find(std::string(k));
        return it == m.end() ? empty_ : it->second;
    }
};

}  // namespace gg
