#pragma once
// dfa.hpp — lazy subset construction over the Nfa's STRUCTURAL transitions.
//
// A DFA state is a set of NFA positions (sorted, interned to an int id). The
// transition `step(s, sym, hop, withStart)` returns the id of the set reached
// from `s` by following `hop` edges to positions matching node-type `sym`
// (plus the start positions, for floating Child steps). Transitions are
// memoized, so once warmed each is an O(1) lookup — that is the whole point of
// the DFA versus re-deriving the position set per node in NFA-simulation.
//
// Value predicates are NOT part of the alphabet: `step` is purely structural.
// The engine refines a structural state by dropping guard-failing positions at
// the actual node (and re-interns the result), so guard-free states stay fully
// table-driven. Each state caches whether it accepts, whether it has any deref
// edge, and which of its positions carry guards.
#include "nfa.hpp"
#include "grammar.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace gg {

class Dfa {
public:
    struct State {
        std::vector<int> pos;      // sorted NFA positions
        bool accept = false;
        bool hasDeref = false;
        std::vector<int> guarded;  // the positions in `pos` that carry guards
    };

    Dfa(const Nfa &nfa, const std::set<TSSymbol> &skip) : nfa_(nfa), skip_(skip) {
        intern({});  // state 0 = the empty set
    }

    int empty() const { return 0; }
    const State &state(int id) const { return states_[id]; }
    std::size_t numStates() const { return states_.size(); }

    int step(int s, TSSymbol sym, Hop hop, bool withStart) {
        Key k{s, sym, hop == Hop::Deref, withStart};
        auto it = trans_.find(k);
        if (it != trans_.end()) return it->second;
        std::set<int> out;
        for (int p : states_[s].pos)
            for (const NfaEdge &e : nfa_.follow(p))
                if (e.hop == hop && matchSym(e.to, sym)) out.insert(e.to);
        if (withStart && hop == Hop::Child)
            for (int q : nfa_.start())
                if (matchSym(q, sym)) out.insert(q);
        int id = intern(std::vector<int>(out.begin(), out.end()));
        trans_.emplace(k, id);
        return id;
    }

    // Intern a sorted position set (used by step and by the engine's guard
    // refinement, which produces sub-states of an existing structural state).
    int intern(std::vector<int> pos) {
        auto it = index_.find(pos);
        if (it != index_.end()) return it->second;
        State st;
        st.pos = pos;
        for (int p : pos) {
            const NfaPosition &P = nfa_.positions()[p];
            if (P.accept) st.accept = true;
            if (!P.guards.empty()) st.guarded.push_back(p);
            for (const NfaEdge &e : nfa_.follow(p))
                if (e.hop == Hop::Deref) { st.hasDeref = true; break; }
        }
        int id = static_cast<int>(states_.size());
        states_.push_back(std::move(st));
        index_.emplace(std::move(pos), id);
        return id;
    }

private:
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

    const Nfa &nfa_;
    const std::set<TSSymbol> &skip_;
    std::vector<State> states_;
    std::map<std::vector<int>, int> index_;
    std::map<Key, int> trans_;

    bool matchSym(int q, TSSymbol sym) const {
        const NfaPosition &P = nfa_.positions()[q];
        if (P.wildcard) return skip_.find(sym) == skip_.end();
        return P.symbol != 0 && P.symbol == sym;
    }
};

}  // namespace gg
