#pragma once
// matchengine.hpp — runs a query over a Document, driven by the lazily-built
// DFA (dfa.hpp). One DFS over the tree threads a single DFA state id; each
// structural transition is a memoized table lookup.
//
// Floating queries (the default) attempt a fresh start at every node during
// that single pass (P1: O(n), not a re-walk per node); anchored queries start
// only at the root. Deref edges jump through the PreWalk reference index (built
// lazily, only when the query uses a deref axis), so the walk is over a graph —
// `continueDfaWalk` is visited-guarded on (state, node) to break deref cycles
// and shared targets. Matched (accept) nodes are deduped and returned in
// document order.
//
// A position matches a node when its type matches AND its value-predicate
// guards pass. Guard results are tri-state {true,false,unevaluable}: an
// unevaluable guard (e.g. a numeric compare against an expression we can't yet
// evaluate) is never a silent no-match — it is excluded but recorded for a
// diagnostic. Existence guards `[subpath]` run a sub-engine rooted at the node.
#include "query/dfa.hpp"
#include "query/nfa.hpp"
#include "query/grammar.hpp"
#include "document.hpp"
#include "expreval.hpp"
#include "prewalk.hpp"
#include "utils.hpp"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gg {

enum class Tri { False, True, Unknown };

class MatchEngine {
public:
    MatchEngine(const Document &doc, const Nfa &nfa, PreWalk *sharedIndex = nullptr)
        : doc_(doc), nfa_(nfa) {
        resolveSymbols();
        if (sharedIndex) index_ = sharedIndex;
        else if (nfa_.needsDeref()) { indexOwned_ = std::make_unique<PreWalk>(doc_); index_ = indexOwned_.get(); }
    }

    // The matched (final-step) nodes, deduped and in document order.
    std::vector<TSNode> run() {
        if (!dfa_) dfa_ = std::make_unique<Dfa>(nfa_, skip_);
        stopAtFirst_ = false;
        matches_.clear();
        visitedDfa_.clear();
        uneval_.clear();
        found_ = false;
        TSNode root = doc_.root();
        int s = dfa_->step(dfa_->empty(), ts_node_symbol(root), LinkAxis::Child, true);
        walkDfa(root, guardFilterDfa(s, root));
        return collect();
    }

    // Predicates that could not be evaluated (predicate text, node) — for diagnostics.
    const std::vector<std::pair<std::string, TSNode>> &unevaluable() const { return uneval_; }

private:
    const Document &doc_;
    const Nfa &nfa_;
    std::unique_ptr<PreWalk> indexOwned_;
    PreWalk *index_ = nullptr;
    std::set<TSSymbol> skip_;                                   // non-element node types
    std::map<std::pair<uint32_t, uint32_t>, TSNode> matches_;   // (start,end) -> node
    std::vector<std::pair<std::string, TSNode>> uneval_;
    std::map<const Predicate *, std::regex> reCache_;
    std::map<const Predicate *, std::unique_ptr<Nfa>> subNfas_;      // declared before subEngines_
    std::map<const Predicate *, std::unique_ptr<MatchEngine>> subEngines_;
    std::unique_ptr<Dfa> dfa_;
    std::unique_ptr<Evaluator> eval_;                               // easy-T3 expression evaluator (lazy)
    std::set<std::pair<int, uint32_t>> visitedDfa_;                  // (dfa state, start byte)
    bool stopAtFirst_ = false;
    bool found_ = false;

    std::vector<TSNode> collect() {
        std::vector<TSNode> out;
        out.reserve(matches_.size());
        for (auto &kv : matches_) out.push_back(kv.second);
        return out;
    }

    void record(TSNode n) {
        matches_.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        if (stopAtFirst_) found_ = true;
    }

    // ---- DFA-driven walk ----

    void walkDfa(TSNode n, int active) {
        if (stopAtFirst_ && found_) return;
        const Dfa::State &st = dfa_->state(active);
        if (st.accept) record(n);
        if (st.hasDeref && index_) {
            for (TSNode d : gatherDeref(n)) {
                int da = guardFilterDfa(dfa_->step(active, ts_node_symbol(d), LinkAxis::Deref, false), d);
                if (!dfa_->state(da).pos.empty()) continueDfaWalk(d, da);
                if (stopAtFirst_ && found_) return;
            }
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int s = dfa_->step(active, ts_node_symbol(ch), LinkAxis::Child, nfa_.floating());
            walkDfa(ch, guardFilterDfa(s, ch));
            if (stopAtFirst_ && found_) return;
        }
    }

    void continueDfaWalk(TSNode n, int active) {
        if (!visitedDfa_.emplace(active, ts_node_start_byte(n)).second) return;
        const Dfa::State &st = dfa_->state(active);
        if (st.accept) record(n);
        if (st.hasDeref && index_) {
            for (TSNode d : gatherDeref(n)) {
                int da = guardFilterDfa(dfa_->step(active, ts_node_symbol(d), LinkAxis::Deref, false), d);
                if (!dfa_->state(da).pos.empty()) continueDfaWalk(d, da);
                if (stopAtFirst_ && found_) return;
            }
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int a = guardFilterDfa(dfa_->step(active, ts_node_symbol(ch), LinkAxis::Child, false), ch);
            if (!dfa_->state(a).pos.empty()) continueDfaWalk(ch, a);
            if (stopAtFirst_ && found_) return;
        }
    }

    // Refine a structural state by dropping guard-failing positions at n. A
    // state with no guarded positions returns unchanged (the table-driven path).
    int guardFilterDfa(int structId, TSNode n) {
        const Dfa::State &st = dfa_->state(structId);
        if (st.guarded.empty()) return structId;
        std::vector<int> keep;
        keep.reserve(st.pos.size());
        for (int q : st.pos) {
            const Nfa::Position &P = nfa_.positions()[q];
            if (P.guards.empty() || guardsPass(P, n) == Tri::True) keep.push_back(q);
        }
        if (keep.size() == st.pos.size()) return structId;
        return dfa_->intern(std::move(keep));
    }

    // ---- guard evaluation ----

    Tri guardsPass(const Nfa::Position &pos, TSNode n) {
        Tri acc = Tri::True;
        for (const Predicate *g : pos.guards) {
            Tri r = evalPred(*g, n);
            if (r == Tri::False) return Tri::False;
            if (r == Tri::Unknown) acc = Tri::Unknown;
        }
        return acc;
    }

    Tri evalPred(const Predicate &p, TSNode n) {
        switch (p.kind) {
            case Predicate::Kind::Compare: return evalCompare(p, n);
            case Predicate::Kind::Exists:  return subEngine(p).existsUnder(n) ? Tri::True : Tri::False;
            case Predicate::Kind::Not: {
                Tri r = evalPred(*p.neg, n);
                return r == Tri::Unknown ? Tri::Unknown : (r == Tri::True ? Tri::False : Tri::True);
            }
        }
        return Tri::False;
    }

    Tri evalCompare(const Predicate &p, TSNode n) {
        std::optional<std::string> at = attrText(n, p.field);
        if (!at) return Tri::False;  // attribute absent -> does not match
        const std::string &av = *at;
        if (p.op == CmpOp::Regex)
            return std::regex_search(av, regexFor(p)) ? Tri::True : Tri::False;

        double pv;
        if (parseNum(p.value, pv)) {  // numeric intent: the LHS must reduce to a number
            double lv;
            if (!numericValue(n, p.field, av, lv)) { recordUneval(p, n); return Tri::Unknown; }
            switch (p.op) {
                case CmpOp::Eq: return tri(lv == pv);
                case CmpOp::Ne: return tri(lv != pv);
                case CmpOp::Lt: return tri(lv < pv);
                case CmpOp::Le: return tri(lv <= pv);
                case CmpOp::Gt: return tri(lv > pv);
                case CmpOp::Ge: return tri(lv >= pv);
                default: return Tri::False;
            }
        }
        switch (p.op) {  // textual intent (RHS is not a number)
            case CmpOp::Eq: return tri(av == p.value);
            case CmpOp::Ne: return tri(av != p.value);
            default: recordUneval(p, n); return Tri::Unknown;  // ordering on non-numeric
        }
    }

    // The numeric value of attribute `field`: a bare literal, else the parsed
    // value expression evaluated over <define> constants (easy-T3).
    bool numericValue(TSNode n, const std::string &field, const std::string &av, double &out) {
        if (parseNum(av, out)) return true;
        TSNode e = valueExprNode(doc_, n, field);
        if (ts_node_is_null(e)) return false;
        if (!eval_) eval_ = std::make_unique<Evaluator>(doc_);
        if (auto v = eval_->eval(e)) { out = *v; return true; }
        return false;
    }

    // Attribute value text (quote-stripped): a real field (name/ref), else a
    // value_attribute / string_attribute child whose Name matches (C2).
    std::optional<std::string> attrText(TSNode n, const std::string &field) const {
        return attrValue(doc_, n, field);
    }

    const std::regex &regexFor(const Predicate &p) {
        auto it = reCache_.find(&p);
        if (it == reCache_.end()) it = reCache_.emplace(&p, std::regex(p.value)).first;
        return it->second;
    }

    void recordUneval(const Predicate &p, TSNode n) { uneval_.emplace_back(toString(p), n); }

    // ---- existence sub-queries (C3) ----

    MatchEngine &subEngine(const Predicate &p) {
        auto it = subEngines_.find(&p);
        if (it != subEngines_.end()) return *it->second;
        auto &slot = subNfas_[&p];
        slot = std::make_unique<Nfa>(*p.sub, /*anchored=*/false, tree_sitter_gdml());
        auto eng = std::make_unique<MatchEngine>(doc_, *slot, index_);
        MatchEngine &ref = *eng;
        subEngines_.emplace(&p, std::move(eng));
        return ref;
    }

    // Does the (sub-)query match anywhere strictly below n? Early-out.
    bool existsUnder(TSNode n) {
        if (!dfa_) dfa_ = std::make_unique<Dfa>(nfa_, skip_);
        stopAtFirst_ = true;
        found_ = false;
        visitedDfa_.clear();
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c && !found_; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int s = dfa_->step(dfa_->empty(), ts_node_symbol(ch), LinkAxis::Child, true);
            walkDfa(ch, guardFilterDfa(s, ch));
        }
        return found_;
    }

    // ---- deref targets: ref of n + refs of its immediate ref-bearing children ----

    std::vector<TSNode> gatherDeref(TSNode n) const {
        std::vector<TSNode> out;
        addRefTargets(n, out);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) addRefTargets(ts_node_named_child(n, i), out);
        return out;
    }

    void addRefTargets(TSNode n, std::vector<TSNode> &out) const {
        TSNode f = ts_node_child_by_field_name(n, "ref", 3);
        if (ts_node_is_null(f)) return;
        std::string_view t = doc_.text(f, true);
        for (TSNode d : index_->definitions(t)) out.push_back(d);
    }

    // ---- setup / helpers ----

    static Tri tri(bool b) { return b ? Tri::True : Tri::False; }

    static bool parseNum(const std::string &s, double &out) {
        if (s.empty()) return false;
        const char *b = s.c_str();
        char *e = nullptr;
        out = std::strtod(b, &e);
        if (e == b) return false;
        while (*e == ' ' || *e == '\t') ++e;
        return *e == '\0';
    }

    void resolveSymbols() {
        static const char *nonElements[] = {
            "document", "prolog", "XMLDecl", "doctypedecl", "content",
            "CharData", "Comment", "PI", "CDSect", "Reference", "EntityRef", "CharRef",
            "AttValue", "Name", "Attribute", "value_attribute", "string_attribute",
            "gdml_value", "number", "identifier", "binary_expression", "unary_expression",
            "parenthesized_expression", "call_expression", "VersionNum", "EncName",
            "STag", "ETag", "EmptyElemTag",
        };
        for (const char *name : nonElements) {
            TSSymbol s = sym(name);
            if(s) skip_.insert(s);
        }
    }
};

}  // namespace gg
