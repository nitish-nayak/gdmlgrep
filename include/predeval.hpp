#pragma once
// predeval.hpp — evaluates a query's value-predicate guards at a node, and owns
// the sub-queries that `[subpath]` existence guards need.
//
// A guard's result is tri-state, GuardResult { Pass, Fail, NaN }: a NaN guard
// (e.g. a numeric compare against an expression we can't yet evaluate, or an
// ordering against non-numeric text) is never a silent no-match — it is excluded
// but recorded for a diagnostic. NaN also propagates through `!` so a negated
// unevaluable guard can't masquerade as a confident match.
//
// One PredEval serves an entire query — the top level and every nested sub-query —
// because its caches key on Predicate*, which is unique across the whole AST.
#include "query/ast.hpp"
#include "query/nfa.hpp"
#include "query/dfa.hpp"
#include "query/grammar.hpp"
#include "document.hpp"
#include "prewalk.hpp"
#include "expreval.hpp"
#include "utils/nodeattr.hpp"
#include "walker.hpp"

#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace gg {

enum class GuardResult { Pass, Fail, NaN };

class PredEval;
// Refine a structural DFA state by dropping guard-failing positions at n (the
// Filter the Walker calls). A state with no guarded positions passes through
// unchanged — the table-driven path.
inline int filter_guards(Dfa &dfa, PredEval &pred, int struct_id, TSNode n);

class PredEval {
public:
    PredEval(const Document &doc, const PreWalk *index) : doc(doc), index(index) {}

    // Tri-state AND of a position's guards at n: Fail short-circuits; a NaN among
    // Passes yields NaN. (NaN guards record themselves for diagnostics en route.)
    GuardResult eval_guards(const std::vector<const Predicate *> &guards, TSNode n) {
        GuardResult acc = GuardResult::Pass;
        for (const Predicate *g : guards) {
            GuardResult r = eval_pred(*g, n);
            if (r == GuardResult::Fail) return GuardResult::Fail;
            if (r == GuardResult::NaN) acc = GuardResult::NaN;
        }
        return acc;
    }

    // Predicates that could not be evaluated (predicate text, node) — for diagnostics.
    const std::vector<std::pair<std::string, TSNode>> &unevaluable() const { return nans; }

private:
    const Document &doc;
    const PreWalk *index;
    std::unique_ptr<PreWalk> owned_index;                       // built lazily if a sub-query derefs
    std::unique_ptr<Evaluator> evaluator;                       // easy-T3 expression evaluator (lazy)
    std::map<const Predicate *, std::regex> regex_cache;
    std::vector<std::pair<std::string, TSNode>> nans;

    struct SubQuery { std::unique_ptr<Nfa> nfa; std::unique_ptr<Dfa> dfa; };
    std::map<const Predicate *, SubQuery> sub_queries;          // one [subpath] -> its compiled query

    GuardResult eval_pred(const Predicate &p, TSNode n) {
        return std::visit(overloaded{
            [&](const Compare &c) { return eval_compare(p, c, n); },
            [&](const Exists &e)  { return exists_match(p, e, n) ? GuardResult::Pass : GuardResult::Fail; },
            [&](const Not &nt) {
                GuardResult r = eval_pred(*nt.neg, n);
                return r == GuardResult::NaN ? r : (r == GuardResult::Pass ? GuardResult::Fail : GuardResult::Pass);
            },
        }, p.value);
    }

    GuardResult eval_compare(const Predicate &p, const Compare &c, TSNode n) {
        std::optional<std::string> at = attrValue(doc, n, c.field);
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

    // Compile [subpath] once (cached), then boolean-walk under n. PredEval is its
    // own guard evaluator, so nested predicates recurse straight back here.
    bool exists_match(const Predicate &p, const Exists &e, TSNode n) {
        SubQuery &sq = sub_queries[&p];
        if (!sq.dfa) {
            sq.nfa = std::make_unique<Nfa>(*e.sub, /*anchored=*/false);
            sq.dfa = std::make_unique<Dfa>(*sq.nfa, kSkipSymbols);
        }
        return Walker(*sq.dfa, doc, index_for(*sq.nfa),
                      [&](int s, TSNode m) { return filter_guards(*sq.dfa, *this, s, m); }).exists_under(n);
    }

    // The index a sub-query walk needs: the shared one, or a lazily-built owned one
    // when the sub-query derefs but none was provided — a deref that lives only
    // inside a predicate doesn't mark the outer query as needing an index.
    const PreWalk *index_for(const Nfa &sub) {
        if (index || !sub.needsDeref()) return index;
        if (!owned_index) owned_index = std::make_unique<PreWalk>(doc);
        return owned_index.get();
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

    const std::regex &regex_for(const Predicate &p, const Compare &c) {
        auto it = regex_cache.find(&p);
        if (it == regex_cache.end()) it = regex_cache.emplace(&p, std::regex(c.value)).first;
        return it->second;
    }

    void record_nan(const Predicate &p, TSNode n) { nans.emplace_back(p.toString(), n); }

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

inline int filter_guards(Dfa &dfa, PredEval &pred, int struct_id, TSNode n) {
    const DfaState &st = dfa.state(struct_id);
    if (st.guarded.empty()) return struct_id;
    std::vector<int> keep;
    keep.reserve(st.pos.size());
    for (int q : st.pos) {
        const NfaPosition &P = dfa.nfa().positions()[q];
        if (P.guards.empty() || pred.eval_guards(P.guards, n) == GuardResult::Pass) keep.push_back(q);
    }
    return keep.size() == st.pos.size() ? struct_id : dfa.state_from_positions(std::move(keep));
}

}  // namespace gg
