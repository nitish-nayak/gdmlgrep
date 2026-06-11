#pragma once
// walker.hpp — drives the DFA over the document tree (and, through the PreWalk
// reference index, the deref graph), collecting the accept nodes. One DFS threads
// a single DFA state id; each structural transition is a memoized table lookup.
//
// Walker knows nothing about predicates: the active state at each node is produced
// by a Filter callable — int(int structural_state, TSNode) — which applies the
// value-predicate guards (see filter_guards in predeval.hpp). Floating queries
// re-attempt a start at every node during the single pass; deref-reached subgraphs
// are visited-guarded on (state, node) to break cycles and shared targets.
#include "query/dfa.hpp"
#include "document.hpp"
#include "prewalk.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace gg {

template <class Filter>
class Walker {
public:
    Walker(Dfa &dfa, const Document &doc, const PreWalk *index, Filter filter)
        : dfa(dfa), doc(doc), index(index), filter(std::move(filter)) {}

    // The matched (accept) nodes, deduped and in document order.
    std::vector<TSNode> collect(TSNode root) {
        reset(/*stop=*/false);
        int s = dfa.transition(dfa.empty(), ts_node_symbol(root), Hop::Child, true);
        walk(root, filter(s, root), /*via_deref=*/false);
        std::vector<TSNode> out;
        out.reserve(matches.size());
        for (auto &kv : matches) out.push_back(kv.second);
        return out;
    }

    // Does the query match strictly below n? Early-out at the first accept.
    bool exists_under(TSNode n) {
        reset(/*stop=*/true);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c && !found; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int s = dfa.transition(dfa.empty(), ts_node_symbol(ch), Hop::Child, true);
            walk(ch, filter(s, ch), /*via_deref=*/false);
        }
        return found;
    }

private:
    Dfa &dfa;
    const Document &doc;
    const PreWalk *index;
    Filter filter;
    std::map<std::pair<uint32_t, uint32_t>, TSNode> matches;   // (start,end) -> node
    std::set<std::pair<int, uint32_t>> visited;                // (dfa state, start byte)
    bool stop_at_first = false;
    bool found = false;

    void reset(bool stop) { matches.clear(); visited.clear(); stop_at_first = stop; found = false; }

    void record(TSNode n) {
        matches.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        if (stop_at_first) found = true;
    }

    // via_deref: reached through a deref edge — visited-guarded, no floating
    // restart, and dead (empty) states are pruned.
    void walk(TSNode n, int active, bool via_deref) {
        if (stop_at_first && found) return;
        if (via_deref && !visited.emplace(active, ts_node_start_byte(n)).second) return;

        const DfaState &st = dfa.state(active);
        if (st.accept) record(n);

        if (st.hasDeref && index)
            for (TSNode d : deref_targets(n)) {
                int da = filter(dfa.transition(active, ts_node_symbol(d), Hop::Deref, false), d);
                if (!dfa.state(da).pos.empty()) walk(d, da, /*via_deref=*/true);
                if (stop_at_first && found) return;
            }

        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            bool with_start = !via_deref && dfa.nfa().floating();
            int a = filter(dfa.transition(active, ts_node_symbol(ch), Hop::Child, with_start), ch);
            // The floating tree pass descends even into empty states — a fresh start
            // may match deeper. A deref subgraph has no restart, so prune dead ends.
            if (!via_deref || !dfa.state(a).pos.empty()) walk(ch, a, via_deref);
            if (stop_at_first && found) return;
        }
    }

    // Deref targets: the ref of n plus the refs of its immediate ref-bearing
    // children, resolved to their definitions through the index.
    std::vector<TSNode> deref_targets(TSNode n) const {
        std::vector<TSNode> out;
        auto add = [&](TSNode x) {
            TSNode f = ts_node_child_by_field_name(x, "ref", 3);
            if (ts_node_is_null(f)) return;
            for (TSNode d : index->definitions(doc.text(f, true))) out.push_back(d);
        };
        add(n);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) add(ts_node_named_child(n, i));
        return out;
    }
};

}  // namespace gg
