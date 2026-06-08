#pragma once
// matchengine.hpp — runs an Nfa over a Document by NFA-simulation. This is the
// correctness oracle; a DFA layer will optimize the transition step later.
//
// One DFS over the tree carries a set of active positions. Floating queries
// (the default) attempt a fresh start at every node during that single pass
// (P1: O(n), not a re-walk per node); anchored queries start only at the root.
// Deref edges jump through the PreWalk reference index (built lazily, only when
// the query uses a deref axis), so the walk is over a graph — `continueAt` is
// visited-guarded on (active-set, node) to break deref cycles and shared
// targets. A matched node is the one matching a final (accept) position;
// matches are deduped and returned in document order (C5).
//
// NOTE: value-predicate guards are not evaluated yet (next increment) — only
// the structural skeleton (types, wildcards, axes, alternation, closures) is
// matched here, so queries carrying `[...]` predicates currently over-match.
#include "document.hpp"
#include "nfa.hpp"
#include "prewalk.hpp"
#include "ts.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gg {

class MatchEngine {
public:
    MatchEngine(const Document &doc, const Nfa &nfa) : doc_(doc), nfa_(nfa) {
        buildSkip();
        if (nfa_.needsDeref()) index_ = std::make_unique<PreWalk>(doc_);
    }

    // The matched (final-step) nodes, deduped and in document order.
    std::vector<TSNode> run() {
        matches_.clear();
        visited_.clear();
        sweep(doc_.root(), {});
        std::vector<TSNode> out;
        out.reserve(matches_.size());
        for (auto &kv : matches_) out.push_back(kv.second);
        return out;
    }

private:
    const Document &doc_;
    const Nfa &nfa_;
    std::unique_ptr<PreWalk> index_;
    std::set<TSSymbol> skip_;                                   // non-element node types
    std::map<std::pair<uint32_t, uint32_t>, TSNode> matches_;   // (start,end) -> node
    std::set<std::pair<std::string, uint32_t>> visited_;        // (active-set, start byte)

    bool isElement(TSNode n) const { return skip_.find(ts_node_symbol(n)) == skip_.end(); }

    bool matchType(int p, TSNode n) const {
        const Nfa::Position &pos = nfa_.positions()[p];
        if (pos.wildcard) return isElement(n);
        return pos.symbol != 0 && pos.symbol == ts_node_symbol(n);
    }

    void record(TSNode n) {
        matches_.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
    }

    std::vector<int> advance(const std::vector<int> &active, LinkAxis axis) const {
        std::vector<int> out;
        for (int p : active)
            for (const Nfa::Edge &e : nfa_.follow(p))
                if (e.axis == axis) out.push_back(e.to);
        return out;
    }

    // Common per-node work: emit on accept, then fan out along deref edges.
    void handle(TSNode n, const std::vector<int> &active) {
        for (int p : active)
            if (nfa_.positions()[p].accept) { record(n); break; }
        if (index_) {
            std::vector<int> derefIn = advance(active, LinkAxis::Deref);
            if (!derefIn.empty())
                for (TSNode d : gatherDeref(n)) continueAt(d, derefIn);
        }
    }

    // Main tree DFS. Visits every named node; injects starts where allowed.
    void sweep(TSNode n, const std::vector<int> &childIn) {
        std::vector<int> active;
        for (int p : childIn)
            if (matchType(p, n)) active.push_back(p);
        if (nfa_.floating() || ts_node_eq(n, doc_.root()))
            for (int s : nfa_.start())
                if (matchType(s, n)) active.push_back(s);
        handle(n, active);
        std::vector<int> childContinue = advance(active, LinkAxis::Child);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i)
            sweep(ts_node_named_child(n, i), childContinue);
    }

    // Continuation after a deref jump: no fresh starts; visited-guarded.
    void continueAt(TSNode n, const std::vector<int> &incoming) {
        std::vector<int> active;
        for (int p : incoming)
            if (matchType(p, n)) active.push_back(p);
        if (active.empty() || !visit(active, n)) return;
        handle(n, active);
        std::vector<int> childContinue = advance(active, LinkAxis::Child);
        if (childContinue.empty()) return;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i)
            continueAt(ts_node_named_child(n, i), childContinue);
    }

    bool visit(const std::vector<int> &active, TSNode n) {
        std::string key;
        for (int p : active) { key += std::to_string(p); key += ','; }
        return visited_.emplace(std::move(key), ts_node_start_byte(n)).second;
    }

    // Deref targets: the ref of n itself plus the refs of its immediate
    // ref-bearing children (self + children are disjoint by the grammar).
    std::vector<TSNode> gatherDeref(TSNode n) const {
        std::vector<TSNode> out;
        addRefTargets(n, out);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i)
            addRefTargets(ts_node_named_child(n, i), out);
        return out;
    }

    void addRefTargets(TSNode n, std::vector<TSNode> &out) const {
        TSNode f = ts_node_child_by_field_name(n, "ref", 3);
        if (ts_node_is_null(f)) return;
        std::string_view t = doc_.text(f);
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
        for (TSNode d : index_->definitions(t)) out.push_back(d);
    }

    // The inherited XML/attribute node types that `*` and `//` must not treat
    // as GDML elements (tracks the base tree-sitter-xml grammar + our attrs).
    void buildSkip() {
        static const char *nonElements[] = {
            "document", "prolog", "XMLDecl", "doctypedecl", "content",
            "CharData", "Comment", "PI", "CDSect", "Reference", "EntityRef", "CharRef",
            "AttValue", "Name", "Attribute", "value_attribute", "string_attribute",
            "gdml_value", "number", "identifier", "binary_expression", "unary_expression",
            "parenthesized_expression", "call_expression", "VersionNum", "EncName",
            "STag", "ETag", "EmptyElemTag",
        };
        const TSLanguage *lang = tree_sitter_gdml();
        for (const char *name : nonElements) {
            TSSymbol s = ts_language_symbol_for_name(lang, name, static_cast<uint32_t>(std::strlen(name)), true);
            if (s != 0) skip_.insert(s);
        }
    }
};

}  // namespace gg
