#pragma once
// nfa.hpp — Glushkov position automaton built from a parsed Query.
//
// Each query Step becomes one position; a position carries the node-type it
// matches (a tree-sitter TSSymbol, or a wildcard), the value-predicate guards
// to check at match time, and an accept flag. The follow relation (the edges)
// is labelled by the axis you traverse to reach the target — Child or Deref.
//
// The two closure axes are handled inline as if desugared:
//   A // B   ==   A /(*)%/ B          (proper descendant, >=1 child hop)
//   A ==> B  ==   A =>(*)%=> B        (>=1 deref hop)
// i.e. a synthetic starred-wildcard position sits between the operands, so the
// automaton only ever deals with single Child/Deref hops and arbitrary depth
// falls out of the Kleene star.
//
// Floating (the default) is NOT baked in as edges: the engine attempts a fresh
// start at every node during its single DFS, which is the O(n) single-pass
// behaviour. Anchored queries (leading `/`) start only at the document root;
// `floating()` records which.
//
// Lifetime: guards point into the Query's AST, so the Query must outlive the
// Nfa and must not be modified after construction.
#include "query.hpp"
#include "ts.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gg {

enum class LinkAxis { Child, Deref };

class Nfa {
public:
    struct Position {
        const Node *step = nullptr;             // source Step (null for synthetic wildcards)
        TSSymbol    symbol = 0;                 // resolved node type; 0 = wildcard / unknown
        bool        wildcard = false;           // matches any element
        bool        accept = false;             // a final step of the query
        std::vector<const Predicate *> guards;  // value predicates, ANDed
    };
    struct Edge { int to; LinkAxis axis; };

    Nfa(const Query &q, const TSLanguage *lang) : Nfa(*q.root, q.anchored, lang) {}

    // Core constructor — also used to compile a predicate's subpath (C3).
    Nfa(const Node &root, bool anchored, const TSLanguage *lang) : lang_(lang) {
        floating_ = !anchored;
        Sets s = build(root, LinkAxis::Child);  // entry axis only matters for a bare top-level star
        start_ = std::move(s.first);
        for (int p : s.last) pos_[p].accept = true;
    }

    const std::vector<Position> &positions() const { return pos_; }
    const std::vector<Edge> &follow(int p) const { return follow_[p]; }
    const std::vector<int> &start() const { return start_; }
    bool floating() const { return floating_; }
    bool needsDeref() const { return needsDeref_; }
    const std::vector<std::string> &unknownTypes() const { return unknown_; }

    void dump(std::FILE *out = stdout) const {
        std::fprintf(out, "nfa: %zu positions, floating=%d, needsDeref=%d\n",
                     pos_.size(), floating_, needsDeref_);
        std::fprintf(out, "start:");
        for (int p : start_) std::fprintf(out, " %d", p);
        std::fprintf(out, "\n");
        for (size_t i = 0; i < pos_.size(); ++i) {
            const Position &p = pos_[i];
            std::fprintf(out, "  %2zu  %-14s%s%s", i, typeName(p).c_str(),
                         p.accept ? " [accept]" : "", p.guards.empty() ? "" : "  guards:");
            for (const Predicate *g : p.guards) std::fprintf(out, " %s", toString(*g).c_str());
            std::fprintf(out, "\n");
            for (const Edge &e : follow_[i])
                std::fprintf(out, "        --%s--> %d\n", e.axis == LinkAxis::Child ? "/" : "=>", e.to);
        }
    }

private:
    const TSLanguage *lang_;
    std::vector<Position> pos_;
    std::vector<std::vector<Edge>> follow_;
    std::vector<int> start_;
    bool floating_ = true;
    bool needsDeref_ = false;
    std::vector<std::string> unknown_;

    struct Sets { bool nullable; std::vector<int> first; std::vector<int> last; };

    static LinkAxis toLink(Axis a) { return a == Axis::Deref ? LinkAxis::Deref : LinkAxis::Child; }
    static void concat(std::vector<int> &dst, const std::vector<int> &src) {
        dst.insert(dst.end(), src.begin(), src.end());
    }

    std::string typeName(const Position &p) const {
        if (p.wildcard) return "*";
        if (p.symbol == 0) return p.step ? p.step->type + "(?)" : "?";
        return ts_language_symbol_name(lang_, p.symbol);
    }

    int newPosition(const Node &step) {
        Position p;
        p.step = &step;
        p.wildcard = step.wildcard;
        if (!step.wildcard) {
            p.symbol = ts_language_symbol_for_name(
                lang_, step.type.c_str(), static_cast<uint32_t>(step.type.size()), true);
            if (p.symbol == 0) unknown_.push_back(step.type);
        }
        pos_.push_back(std::move(p));
        follow_.emplace_back();
        return static_cast<int>(pos_.size()) - 1;
    }

    int newWildcard() {
        pos_.push_back(Position{nullptr, 0, true, false, {}});
        follow_.emplace_back();
        return static_cast<int>(pos_.size()) - 1;
    }

    Sets build(const Node &n, LinkAxis entry) {
        Sets s{false, {}, {}};
        switch (n.kind) {
            case Node::Kind::Step:
                s = {false, {newPosition(n)}, {}};
                s.last = s.first;
                break;

            case Node::Kind::Alt:
                for (const auto &kid : n.kids) {
                    Sets k = build(*kid, entry);
                    s.nullable = s.nullable || k.nullable;
                    concat(s.first, k.first);
                    concat(s.last, k.last);
                }
                break;

            case Node::Kind::Repeat: {
                Sets x = build(*n.kids[0], entry);
                if (n.quant != Quant::Opt)  // Star/Plus: loop back, re-entering via the entry axis
                    for (int p : x.last)
                        for (int q : x.first)
                            follow_[p].push_back({q, entry});
                s.nullable = (n.quant != Quant::Plus) || x.nullable;
                s.first = x.first;
                s.last = x.last;
                break;
            }

            case Node::Kind::Seq:
                if (n.axis == Axis::Child || n.axis == Axis::Deref) {
                    LinkAxis ax = toLink(n.axis);
                    if (ax == LinkAxis::Deref) needsDeref_ = true;
                    Sets a = build(*n.kids[0], entry);
                    Sets b = build(*n.kids[1], ax);
                    for (int p : a.last)
                        for (int q : b.first) follow_[p].push_back({q, ax});
                    s.nullable = a.nullable && b.nullable;
                    s.first = a.first;
                    if (a.nullable) concat(s.first, b.first);
                    s.last = b.last;
                    if (b.nullable) concat(s.last, a.last);
                } else {  // Descendant / DerefClosure: A hop (w)% hop B, with (w)% nullable
                    LinkAxis ax = (n.axis == Axis::Descendant) ? LinkAxis::Child : LinkAxis::Deref;
                    if (ax == LinkAxis::Deref) needsDeref_ = true;
                    Sets a = build(*n.kids[0], entry);
                    int w = newWildcard();
                    follow_[w].push_back({w, ax});  // (w)% self-loop
                    Sets b = build(*n.kids[1], ax);
                    for (int p : a.last) {
                        follow_[p].push_back({w, ax});
                        for (int q : b.first) follow_[p].push_back({q, ax});
                    }
                    for (int q : b.first) follow_[w].push_back({q, ax});
                    std::vector<int> innerFirst{w};
                    concat(innerFirst, b.first);
                    std::vector<int> innerLast = b.last;
                    if (b.nullable) innerLast.push_back(w);
                    s.nullable = a.nullable && b.nullable;
                    s.first = a.first;
                    if (a.nullable) concat(s.first, innerFirst);
                    s.last = innerLast;
                    if (b.nullable) concat(s.last, a.last);
                }
                break;
        }
        // A node's predicates constrain the node it resolves to -> its terminal positions.
        for (const Predicate &pr : n.preds)
            for (int p : s.last) pos_[p].guards.push_back(&pr);
        return s;
    }
};

}  // namespace gg
