#pragma once
// matchengine.hpp — runs a query over a Document, driven by the lazily-built
// DFA (dfa.hpp). One DFS over the tree threads a single DFA state id; each
// structural transition is a memoized table lookup.
//
// Floating queries (the default) attempt a fresh start at every node during
// that single pass (P1: O(n), not a re-walk per node); anchored queries start
// only at the root. Deref edges jump through the PreWalk reference index (built
// lazily, only when the query uses a deref connector), so the walk is over a graph —
// `continue_dfa_walk` is visited-guarded on (state, node) to break deref cycles
// and shared targets. Matched (accept) nodes are deduped and returned in
// document order.
//
// A position matches a node when its type matches AND its value-predicate
// guards pass. Guard results are tri-state {Pass, Fail, NaN}: a NaN guard (e.g. a
// numeric compare against an expression we can't yet evaluate) is never a silent
// no-match — it is excluded but recorded for a diagnostic. Existence guards
// `[subpath]` run a sub-engine rooted at the node.
#include "query/ast.hpp"
#include "query/dfa.hpp"
#include "query/nfa.hpp"
#include "query/grammar.hpp"
#include "document.hpp"
#include "expreval.hpp"
#include "prewalk.hpp"
#include "utils/nodeattr.hpp"

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

enum class GuardResult { Pass, Fail, NaN };

class MatchEngine {
public:
    MatchEngine(const Document &doc, const Nfa &nfa, PreWalk *shared_index = nullptr)
        : doc(doc), nfa(nfa) {
        if (shared_index) index = shared_index;
        else if (nfa.needsDeref()) { owned_index = std::make_unique<PreWalk>(doc); index = owned_index.get(); }
    }

    // The matched (final-step) nodes, deduped and in document order.
    std::vector<TSNode> run() {
        if (!dfa) dfa = std::make_unique<Dfa>(nfa, kSkipSymbols);
        stop_at_first = false;
        matches.clear();
        visited.clear();
        nans.clear();
        found = false;
        TSNode root = doc.root();
        int s = dfa->transition(dfa->empty(), ts_node_symbol(root), Hop::Child, true);
        walk_dfa(root, guard_filter_dfa(s, root));
        return collect();
    }

    // Predicates that could not be evaluated (predicate text, node) — for diagnostics.
    const std::vector<std::pair<std::string, TSNode>> &unevaluable() const { return nans; }

private:
    const Document &doc;
    const Nfa &nfa;
    std::unique_ptr<PreWalk> owned_index;
    PreWalk *index = nullptr;
    std::map<std::pair<uint32_t, uint32_t>, TSNode> matches;    // (start,end) -> node
    std::vector<std::pair<std::string, TSNode>> nans;
    std::map<const Predicate *, std::regex> regex_cache;
    std::map<const Predicate *, std::unique_ptr<Nfa>> sub_nfas;      // declared before sub_engines
    std::map<const Predicate *, std::unique_ptr<MatchEngine>> sub_engines;
    std::unique_ptr<Dfa> dfa;
    std::unique_ptr<Evaluator> evaluator;                           // easy-T3 expression evaluator (lazy)
    std::set<std::pair<int, uint32_t>> visited;                     // (dfa state, start byte)
    bool stop_at_first = false;
    bool found = false;

    std::vector<TSNode> collect() {
        std::vector<TSNode> out;
        out.reserve(matches.size());
        for (auto &kv : matches) out.push_back(kv.second);
        return out;
    }

    void record(TSNode n) {
        matches.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        if (stop_at_first) found = true;
    }

    // ---- DFA-driven walk ----

    void walk_dfa(TSNode n, int active) {
        if (stop_at_first && found) return;
        const DfaState &st = dfa->state(active);
        if (st.accept) record(n);
        if (st.hasDeref && index) {
            for (TSNode d : gather_deref(n)) {
                int da = guard_filter_dfa(dfa->transition(active, ts_node_symbol(d), Hop::Deref, false), d);
                if (!dfa->state(da).pos.empty()) continue_dfa_walk(d, da);
                if (stop_at_first && found) return;
            }
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int s = dfa->transition(active, ts_node_symbol(ch), Hop::Child, nfa.floating());
            walk_dfa(ch, guard_filter_dfa(s, ch));
            if (stop_at_first && found) return;
        }
    }

    void continue_dfa_walk(TSNode n, int active) {
        if (!visited.emplace(active, ts_node_start_byte(n)).second) return;
        const DfaState &st = dfa->state(active);
        if (st.accept) record(n);
        if (st.hasDeref && index) {
            for (TSNode d : gather_deref(n)) {
                int da = guard_filter_dfa(dfa->transition(active, ts_node_symbol(d), Hop::Deref, false), d);
                if (!dfa->state(da).pos.empty()) continue_dfa_walk(d, da);
                if (stop_at_first && found) return;
            }
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int a = guard_filter_dfa(dfa->transition(active, ts_node_symbol(ch), Hop::Child, false), ch);
            if (!dfa->state(a).pos.empty()) continue_dfa_walk(ch, a);
            if (stop_at_first && found) return;
        }
    }

    // Refine a structural state by dropping guard-failing positions at n. A
    // state with no guarded positions returns unchanged (the table-driven path).
    int guard_filter_dfa(int struct_id, TSNode n) {
        const DfaState &st = dfa->state(struct_id);
        if (st.guarded.empty()) return struct_id;
        std::vector<int> keep;
        keep.reserve(st.pos.size());
        for (int q : st.pos) {
            const NfaPosition &P = nfa.positions()[q];
            if (P.guards.empty() || guards_pass(P, n) == GuardResult::Pass) keep.push_back(q);
        }
        if (keep.size() == st.pos.size()) return struct_id;
        return dfa->state_from_positions(std::move(keep));
    }

    // ---- guard evaluation ----

    GuardResult guards_pass(const NfaPosition &pos, TSNode n) {
        GuardResult acc = GuardResult::Pass;
        for (const Predicate *g : pos.guards) {
            GuardResult r = eval_pred(*g, n);
            if (r == GuardResult::Fail) return GuardResult::Fail;
            if (r == GuardResult::NaN) acc = GuardResult::NaN;
        }
        return acc;
    }

    GuardResult eval_pred(const Predicate &p, TSNode n) {
        return std::visit(overloaded{
            [&](const Compare &c) { return eval_compare(p, c, n); },
            [&](const Exists &e)  { return sub_engine(p, e).exists_under(n) ? GuardResult::Pass : GuardResult::Fail; },
            [&](const Not &nt) {
                GuardResult r = eval_pred(*nt.neg, n);
                return r == GuardResult::NaN ? r : (r == GuardResult::Pass ? GuardResult::Fail : GuardResult::Pass);
            },
        }, p.value);
    }

    GuardResult eval_compare(const Predicate &p, const Compare &c, TSNode n) {
        std::optional<std::string> at = attr_text(n, c.field);
        if (!at) return GuardResult::Fail;  // attribute absent -> does not match
        const std::string &av = *at;
        if (c.op == Comparator::Regex)
            return std::regex_search(av, regex_for(p, c)) ? GuardResult::Pass : GuardResult::Fail;

        double pv;
        if (parse_num(c.value, pv)) {  // numeric intent: the LHS must reduce to a number
            double lv;
            if (!numeric_value(n, c.field, av, lv)) { record_nan(p, n); return GuardResult::NaN; }
            switch (c.op) {
                case Comparator::Eq: return pass_if(lv == pv);
                case Comparator::Ne: return pass_if(lv != pv);
                case Comparator::Lt: return pass_if(lv < pv);
                case Comparator::Le: return pass_if(lv <= pv);
                case Comparator::Gt: return pass_if(lv > pv);
                case Comparator::Ge: return pass_if(lv >= pv);
                default: return GuardResult::Fail;
            }
        }
        switch (c.op) {  // textual intent (RHS is not a number)
            case Comparator::Eq: return pass_if(av == c.value);
            case Comparator::Ne: return pass_if(av != c.value);
            default: record_nan(p, n); return GuardResult::NaN;  // ordering on non-numeric
        }
    }

    // The numeric value of attribute `field`: a bare literal, else the parsed
    // value expression evaluated over <define> constants (easy-T3).
    bool numeric_value(TSNode n, const std::string &field, const std::string &av, double &out) {
        if (parse_num(av, out)) return true;
        TSNode e = valueExprNode(doc, n, field);
        if (ts_node_is_null(e)) return false;
        if (!evaluator) evaluator = std::make_unique<Evaluator>(doc);
        if (auto v = evaluator->eval(e)) { out = *v; return true; }
        return false;
    }

    // Attribute value text (quote-stripped): a real field (name/ref), else a
    // value_attribute / string_attribute child whose Name matches (C2).
    std::optional<std::string> attr_text(TSNode n, const std::string &field) const {
        return attrValue(doc, n, field);
    }

    const std::regex &regex_for(const Predicate &p, const Compare &c) {
        auto it = regex_cache.find(&p);
        if (it == regex_cache.end()) it = regex_cache.emplace(&p, std::regex(c.value)).first;
        return it->second;
    }

    void record_nan(const Predicate &p, TSNode n) { nans.emplace_back(p.toString(), n); }

    // ---- existence sub-queries (C3) ----

    MatchEngine &sub_engine(const Predicate &p, const Exists &e) {
        auto it = sub_engines.find(&p);
        if (it != sub_engines.end()) return *it->second;
        auto &slot = sub_nfas[&p];
        slot = std::make_unique<Nfa>(*e.sub, /*anchored=*/false);
        auto eng = std::make_unique<MatchEngine>(doc, *slot, index);
        MatchEngine &ref = *eng;
        sub_engines.emplace(&p, std::move(eng));
        return ref;
    }

    // Does the (sub-)query match anywhere strictly below n? Early-out.
    bool exists_under(TSNode n) {
        if (!dfa) dfa = std::make_unique<Dfa>(nfa, kSkipSymbols);
        stop_at_first = true;
        found = false;
        visited.clear();
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c && !found; ++i) {
            TSNode ch = ts_node_named_child(n, i);
            int s = dfa->transition(dfa->empty(), ts_node_symbol(ch), Hop::Child, true);
            walk_dfa(ch, guard_filter_dfa(s, ch));
        }
        return found;
    }

    // ---- deref targets: ref of n + refs of its immediate ref-bearing children ----

    std::vector<TSNode> gather_deref(TSNode n) const {
        std::vector<TSNode> out;
        add_ref_targets(n, out);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) add_ref_targets(ts_node_named_child(n, i), out);
        return out;
    }

    void add_ref_targets(TSNode n, std::vector<TSNode> &out) const {
        TSNode f = ts_node_child_by_field_name(n, "ref", 3);
        if (ts_node_is_null(f)) return;
        std::string_view t = doc.text(f, true);
        for (TSNode d : index->definitions(t)) out.push_back(d);
    }

    // ---- setup / helpers ----

    static GuardResult pass_if(bool b) { return b ? GuardResult::Pass : GuardResult::Fail; }

    static bool parse_num(const std::string &s, double &out) {
        if (s.empty()) return false;
        const char *b = s.c_str();
        char *e = nullptr;
        out = std::strtod(b, &e);
        if (e == b) return false;
        while (*e == ' ' || *e == '\t') ++e;
        return *e == '\0';
    }
};

}  // namespace gg
