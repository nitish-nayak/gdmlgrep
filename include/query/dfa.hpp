#pragma once
// dfa.hpp — a lazily-built DFA over the Nfa's structural transitions.
//
// A DFA state is a set of NFA positions, sorted and given an int id so that equal
// sets share one state. transition(s, sym, hop, withStart) returns the state
// reached from `s` by following `hop` edges to positions matching node type `sym`,
// plus the query's start positions on a floating Child step. Each transition is
// memoized, so once warmed it is an O(1) lookup — the point of the DFA over
// re-deriving the position set at every node (NFA simulation).
//
// Value predicates are not part of the alphabet: transition() is purely
// structural. The match engine refines a structural state by dropping the
// guard-failing positions at the actual node (re-deriving the smaller state), so
// guard-free states stay entirely table-driven. Each state also caches whether it
// accepts, whether any position has a deref out-edge, and which positions carry
// guards.
#include "nfa.hpp"
#include "grammar.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace gg {

// A DFA state: the set of live NFA positions, plus the per-state facts the match
// engine reads at each node.
struct DfaState {
    std::vector<int> pos;      // the live NFA positions, sorted
    bool accept = false;       // some position accepts (a final step of the query)
    bool hasDeref = false;     // some position has a deref out-edge
    std::vector<int> guarded;  // the positions in `pos` that carry value-predicate guards
};

// The lazily-built DFA: it interns states on demand as transition() is called
// during the walk, and memoizes each transition. It holds references to the source
// Nfa and the skip set (the node types treated as trivia), so both must outlive it.
class Dfa {
public:
    Dfa(const Nfa &nfa, const std::set<TSSymbol> &skip) : source_nfa(nfa), skip_symbols(skip) {
        state_from_positions({});  // state 0 = the empty set
    }

    int empty() const { return 0; }  // the empty state (no positions) — id 0, where a walk starts
    const DfaState &state(int id) const { return state_list[id]; }
    std::size_t numStates() const { return state_list.size(); }

    // The transition function: the state reached from `s` on input (sym, hop),
    // memoized in transition_cache. withStart also seeds the query's start
    // positions — the floating default's fresh start re-attempted at every node.
    int transition(int s, TSSymbol sym, Hop hop, bool withStart) {
        Key k{s, sym, hop == Hop::Deref, withStart};

        auto it = transition_cache.find(k);
        if (it != transition_cache.end()) return it->second;

        std::set<int> out;
        for (int p : state_list[s].pos)
            for (const NfaEdge &e : source_nfa.follow(p))
                if (e.hop == hop && match_symbol(e.to, sym)) out.insert(e.to);
        if (withStart && hop == Hop::Child)
            for (int q : source_nfa.start())
                if (match_symbol(q, sym)) out.insert(q);

        int id = state_from_positions(std::vector<int>(out.begin(), out.end()));
        transition_cache.emplace(k, id);
        return id;
    }

    // The state id for a sorted position set, creating the state if new (used by
    // transition and by the engine's guard refinement, which produces sub-states
    // of an existing structural state).
    int state_from_positions(std::vector<int> pos) {
        auto it = state_index.find(pos);
        if (it != state_index.end()) return it->second;

        DfaState st;
        st.pos = pos;
        for (int p : pos) {
            const NfaPosition &P = source_nfa.positions()[p];
            if (P.accept) st.accept = true;
            if (!P.guards.empty()) st.guarded.push_back(p);
            for (const NfaEdge &e : source_nfa.follow(p))
                if (e.hop == Hop::Deref) {
                    st.hasDeref = true; break;
                }
        }

        int id = static_cast<int>(state_list.size());
        state_list.push_back(std::move(st));
        state_index.emplace(std::move(pos), id);
        return id;
    }

private:
    // Memo key for transition(): source state, input symbol, whether the hop is a
    // deref, and whether start positions were seeded.
    struct Key {
        int s;
        TSSymbol sym;
        bool deref;
        bool withStart;
        bool operator<(const Key &o) const {
            if (s != o.s) return s < o.s;
            if (sym != o.sym) return sym < o.sym;
            if (deref != o.deref) return deref < o.deref;
            return withStart < o.withStart;
        }
    };

    const Nfa &source_nfa;
    const std::set<TSSymbol> &skip_symbols;
    std::vector<DfaState> state_list;
    std::map<std::vector<int>, int> state_index;
    std::map<Key, int> transition_cache;

    // Does NFA position q match a node of type sym? A wildcard matches any element
    // except the skipped trivia types; a typed position matches its exact symbol.
    bool match_symbol(int q, TSSymbol sym) const {
        const NfaPosition &P = source_nfa.positions()[q];
        if (P.wildcard) return skip_symbols.find(sym) == skip_symbols.end();
        return P.symbol != 0 && P.symbol == sym;
    }
};

}  // namespace gg
