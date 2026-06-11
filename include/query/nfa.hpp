#pragma once
// nfa.hpp — the query compiled to a Glushkov position automaton.
//
// Every query Step is one position (NfaPosition): the node type it matches (or a
// wildcard), the value-predicate guards checked there, and whether it accepts. The
// follow relation (NfaEdge) connects positions, each edge labelled by the hop that
// reaches the target — Child (descend to a child) or Deref (follow a reference).
//
// The closure connectors desugar to a starred wildcard between the operands, so
// edges only ever carry a single Child/Deref hop and arbitrary depth falls out of
// the Kleene star:
//   A // B   ==   A /(*)%/ B          (proper descendant, >=1 child hop)
//   A ==> B  ==   A =>(*)%=> B        (>=1 deref hop)
//
// Floating (the default) is not encoded as edges: the engine re-attempts a start
// at every node during its single O(n) DFS. Anchored queries (leading `/`) start
// only at the document root; floating() reports which.
//
// Lifetime: a position's guards point into the Query's AST, so the Query must
// outlive the Nfa and stay unmodified after construction.
#include "ast.hpp"
#include "grammar.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace gg {

enum class Hop { Child, Deref };  // a single edge hop: descend to a child (/) or dereference (=>)

// The Hop each Connector's edges traverse. The closure forms (Descendant //,
// DerefClosure ==>) loop over the same hop as their single-step forms (/, =>).
static const std::map<Connector, Hop> kHop = {
    {Connector::Child,        Hop::Child},
    {Connector::Descendant,   Hop::Child},
    {Connector::Deref,        Hop::Deref},
    {Connector::DerefClosure, Hop::Deref},
};

// One automaton state: the node type it matches (or a wildcard), the value
// predicates to check there, and whether it is an accepting (final) position.
struct NfaPosition {
    const Step *step = nullptr;             // source Step (null for synthetic wildcards)
    TSSymbol    symbol = 0;                 // resolved node type; 0 = wildcard / unknown
    bool        wildcard = false;           // matches any element
    bool        accept = false;             // a final step of the query
    std::vector<const Predicate *> guards;  // value predicates, ANDed

    NfaPosition() = default;
    explicit NfaPosition(const Step &s) : step(&s), wildcard(s.wildcard) {
        if (s.wildcard) return;
        // `element` is renamed to `gdml_element` in the grammar (node_renames in
        // rules.mjs) to dodge the inherited XML element rule; accept the bare
        // GDML spelling at the query surface so callers needn't know it.
        std::string_view t = s.type == "element" ? std::string_view("gdml_element") : s.type;
        symbol = sym(t.data());
    }
};

// A follow edge: the target position and the hop that reaches it.
struct NfaEdge { int to; Hop hop; };

// The compiled automaton: the positions, their follow (edge) relation, the
// query's start positions, and a few derived flags. Built from a Query in the
// constructor, then read-only.
class Nfa {
public:
    Nfa(const Query &q) : Nfa(*q.root, q.anchored) {}

    // Builds the automaton for root; also used to compile a predicate's sub-path.
    Nfa(const Node &root, bool anchored) {
        is_start_floating = !anchored;
        Sets s = build(root, Hop::Child);  // entry hop only matters for a bare top-level star
        start_position = std::move(s.first);
        for (int p : s.last) positions_list[p].accept = true;
    }

    const std::vector<NfaPosition> &positions() const { return positions_list; }
    const std::vector<NfaEdge> &follow(int p) const { return follow_position[p]; }
    const std::vector<int> &start() const { return start_position; }
    bool floating() const { return is_start_floating; }
    bool needsDeref() const { return needs_deref; }
    const std::vector<std::string> &unknownTypes() const { return unknown_types; }

    // Human-readable dump of the automaton, for diagnostics.
    void dump(std::FILE *out = stdout) const {
        std::fprintf(out, "nfa: %zu positions, floating=%d, needsDeref=%d\n",
                     positions_list.size(), is_start_floating, needs_deref);

        std::fprintf(out, "start:");
        for (int p : start_position) std::fprintf(out, " %d", p);

        std::fprintf(out, "\n");
        for (size_t i = 0; i < positions_list.size(); ++i) {
            const NfaPosition &p = positions_list[i];
            std::string name;
            if (p.wildcard) name = "*";
            else if (p.symbol == 0) name = p.step ? p.step->type + "(?)" : "?";
            else name = sym_name(p.symbol);

            std::fprintf(out, "  %2zu  %-14s%s%s", i, name.c_str(),
                         p.accept ? " [accept]" : "", p.guards.empty() ? "" : "  guards:");
            for (const Predicate *g : p.guards) std::fprintf(out, " %s", g->toString().c_str());

            std::fprintf(out, "\n");
            for (const NfaEdge &e : follow_position[i])
                std::fprintf(out, "        --%s--> %d\n", e.hop == Hop::Child ? "/" : "=>", e.to);
        }
    }

private:
    std::vector<NfaPosition> positions_list;
    std::vector<std::vector<NfaEdge>> follow_position;
    std::vector<int> start_position;
    bool is_start_floating = true;
    bool needs_deref = false;
    std::vector<std::string> unknown_types;

    // Glushkov sets for a sub-expression: whether it matches the empty path, plus
    // its first/last position sets. (Follow edges are wired into follow_position.)
    struct Sets { bool matches_empty; std::vector<int> first; std::vector<int> last; };

    // Append src onto dst.
    static void concat(std::vector<int> &dst, const std::vector<int> &src) {
        dst.insert(dst.end(), src.begin(), src.end());
    }

    // Glushkov construction: returns the (matches_empty, first, last) sets for n,
    // wiring follow edges between positions as it descends.
    Sets build(const Node &n, Hop entry) {
        Sets s = std::visit(overloaded{
            // Step: a single position, which is its own first and last (never empty).
            [&](const Step &st) {
                NfaPosition p(st);
                if (!p.wildcard && p.symbol == 0) unknown_types.push_back(st.type);

                positions_list.push_back(std::move(p));
                follow_position.emplace_back();

                int i = static_cast<int>(positions_list.size()) - 1;
                return Sets{false, {i}, {i}};
            },
            // Alternation: first/last are the unions over the branches; empty if any branch is.
            [&](const Alt &a) {
                Sets r{false, {}, {}};
                for (const auto &child : a.branches) {
                    Sets k = build(*child, entry);
                    r.matches_empty = r.matches_empty || k.matches_empty;
                    concat(r.first, k.first);
                    concat(r.last, k.last);
                }
                return r;
            },
            // Quantifier: %/+ add last->first loop-back edges; %/? also match the empty path.
            [&](const Repeat &rep) {
                Sets x = build(*rep.inner, entry);
                Sets r{false, {}, {}};
                if (rep.quant != Quantifier::Opt) {     // Star/Plus: loop back, re-entering via the entry hop
                    for (int p : x.last)
                        for (int q : x.first)
                            follow_position[p].push_back({q, entry});
                }

                r.matches_empty = (rep.quant != Quantifier::Plus) || x.matches_empty;
                r.first = x.first;
                r.last = x.last;
                return r;
            },
            // Sequence: link a.last -> b.first by the hop; the closure forms (// ==>)
            // thread a starred wildcard between the operands (see the file header).
            [&](const Seq &sq) {
                Sets r{false, {}, {}};
                Hop hop = kHop.at(sq.connector);
                if (hop == Hop::Deref) needs_deref = true;

                if (sq.connector == Connector::Child || sq.connector == Connector::Deref) {
                    Sets a = build(*sq.lhs, entry);
                    Sets b = build(*sq.rhs, hop);
                    for (int p : a.last)
                        for (int q : b.first) follow_position[p].push_back({q, hop});

                    r.matches_empty = a.matches_empty && b.matches_empty;
                    r.first = a.first;
                    if (a.matches_empty) concat(r.first, b.first);
                    r.last = b.last;
                    if (b.matches_empty) concat(r.last, a.last);
                } else {  // Descendant / DerefClosure: A hop (w)% hop B, with (w)% matches_empty
                    Sets a = build(*sq.lhs, entry);
                    NfaPosition wp;
                    wp.wildcard = true;
                    positions_list.push_back(std::move(wp));
                    follow_position.emplace_back();
                    int w = static_cast<int>(positions_list.size()) - 1;
                    follow_position[w].push_back({w, hop});  // (w)% self-loop

                    Sets b = build(*sq.rhs, hop);
                    for (int p : a.last) {
                        follow_position[p].push_back({w, hop});
                        for (int q : b.first) follow_position[p].push_back({q, hop});
                    }
                    for (int q : b.first) follow_position[w].push_back({q, hop});

                    std::vector<int> innerFirst{w};
                    concat(innerFirst, b.first);
                    std::vector<int> innerLast = b.last;
                    if (b.matches_empty) innerLast.push_back(w);

                    r.matches_empty = a.matches_empty && b.matches_empty;
                    r.first = a.first;
                    if (a.matches_empty) concat(r.first, innerFirst);
                    r.last = innerLast;
                    if (b.matches_empty) concat(r.last, a.last);
                }
                return r;
            },
        }, n.value);
        // A node's predicates constrain the node it resolves to -> its terminal positions.
        for (const Predicate &pr : n.preds)
            for (int p : s.last) positions_list[p].guards.push_back(&pr);
        return s;
    }
};

}  // namespace gg
