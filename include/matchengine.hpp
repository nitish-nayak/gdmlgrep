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
// targets. Matched (accept) nodes are deduped and returned in document order.
//
// A position matches a node when its type matches AND its value-predicate
// guards pass. Guard results are tri-state {true,false,unevaluable}: an
// unevaluable guard (e.g. a numeric compare against an expression we can't yet
// evaluate) is never a silent no-match — it is excluded but recorded for a
// diagnostic. Existence guards `[subpath]` run a sub-engine rooted at the node.
#include "document.hpp"
#include "nfa.hpp"
#include "prewalk.hpp"
#include "ts.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        stopAtFirst_ = false;
        matches_.clear();
        visited_.clear();
        found_ = false;
        sweep(doc_.root(), {});
        std::vector<TSNode> out;
        out.reserve(matches_.size());
        for (auto &kv : matches_) out.push_back(kv.second);
        return out;
    }

    // Predicates that could not be evaluated (predicate text, node) — for diagnostics.
    const std::vector<std::pair<std::string, TSNode>> &unevaluable() const { return uneval_; }

private:
    const Document &doc_;
    const Nfa &nfa_;
    std::unique_ptr<PreWalk> indexOwned_;
    PreWalk *index_ = nullptr;
    TSSymbol valueAttrSym_ = 0, stringAttrSym_ = 0;
    std::set<TSSymbol> skip_;                                   // non-element node types
    std::map<std::pair<uint32_t, uint32_t>, TSNode> matches_;   // (start,end) -> node
    std::set<std::pair<std::string, uint32_t>> visited_;        // (active-set, start byte)
    std::vector<std::pair<std::string, TSNode>> uneval_;
    std::map<const Predicate *, std::regex> reCache_;
    std::map<const Predicate *, std::unique_ptr<Nfa>> subNfas_;      // declared before subEngines_
    std::map<const Predicate *, std::unique_ptr<MatchEngine>> subEngines_;
    bool stopAtFirst_ = false;
    bool found_ = false;

    // ---- structural + guarded match ----

    bool isElement(TSNode n) const { return skip_.find(ts_node_symbol(n)) == skip_.end(); }

    bool matchType(int p, TSNode n) const {
        const Nfa::Position &pos = nfa_.positions()[p];
        if (pos.wildcard) return isElement(n);
        return pos.symbol != 0 && pos.symbol == ts_node_symbol(n);
    }

    bool matches(int p, TSNode n) {
        if (!matchType(p, n)) return false;
        Tri g = guardsPass(nfa_.positions()[p], n);
        if (g == Tri::Unknown) return false;  // recorded in evalCompare; excluded, not silent
        return g == Tri::True;
    }

    void record(TSNode n) {
        matches_.emplace(std::make_pair(ts_node_start_byte(n), ts_node_end_byte(n)), n);
        if (stopAtFirst_) found_ = true;
    }

    std::vector<int> advance(const std::vector<int> &active, LinkAxis axis) const {
        std::vector<int> out;
        for (int p : active)
            for (const Nfa::Edge &e : nfa_.follow(p))
                if (e.axis == axis) out.push_back(e.to);
        return out;
    }

    void handle(TSNode n, const std::vector<int> &active) {
        for (int p : active)
            if (nfa_.positions()[p].accept) { record(n); break; }
        if (index_) {
            std::vector<int> derefIn = advance(active, LinkAxis::Deref);
            if (!derefIn.empty())
                for (TSNode d : gatherDeref(n)) { continueAt(d, derefIn); if (stopAtFirst_ && found_) return; }
        }
    }

    void sweep(TSNode n, const std::vector<int> &childIn) {
        if (stopAtFirst_ && found_) return;
        std::vector<int> active;
        for (int p : childIn)
            if (matches(p, n)) active.push_back(p);
        if (nfa_.floating() || ts_node_eq(n, doc_.root()))
            for (int s : nfa_.start())
                if (matches(s, n)) active.push_back(s);
        handle(n, active);
        std::vector<int> childContinue = advance(active, LinkAxis::Child);
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            sweep(ts_node_named_child(n, i), childContinue);
            if (stopAtFirst_ && found_) return;
        }
    }

    void continueAt(TSNode n, const std::vector<int> &incoming) {
        std::vector<int> active;
        for (int p : incoming)
            if (matches(p, n)) active.push_back(p);
        if (active.empty() || !visit(active, n)) return;
        handle(n, active);
        std::vector<int> childContinue = advance(active, LinkAxis::Child);
        if (childContinue.empty()) return;
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            continueAt(ts_node_named_child(n, i), childContinue);
            if (stopAtFirst_ && found_) return;
        }
    }

    bool visit(const std::vector<int> &active, TSNode n) {
        std::string key;
        for (int p : active) { key += std::to_string(p); key += ','; }
        return visited_.emplace(std::move(key), ts_node_start_byte(n)).second;
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
        double avn, pvn;
        bool an = parseNum(av, avn), pn = parseNum(p.value, pvn);
        switch (p.op) {
            case CmpOp::Eq: return (an && pn) ? tri(avn == pvn) : tri(av == p.value);
            case CmpOp::Ne: return (an && pn) ? tri(avn != pvn) : tri(av != p.value);
            case CmpOp::Lt: case CmpOp::Le: case CmpOp::Gt: case CmpOp::Ge:
                if (!(an && pn)) { recordUneval(p, n); return Tri::Unknown; }
                if (p.op == CmpOp::Lt) return tri(avn < pvn);
                if (p.op == CmpOp::Le) return tri(avn <= pvn);
                if (p.op == CmpOp::Gt) return tri(avn > pvn);
                return tri(avn >= pvn);
            default: return Tri::False;
        }
    }

    // Attribute value text (quote-stripped): a real field (name/ref), else a
    // value_attribute / string_attribute child whose Name matches (C2).
    std::optional<std::string> attrText(TSNode n, const std::string &field) const {
        TSNode f = ts_node_child_by_field_name(n, field.c_str(), static_cast<uint32_t>(field.size()));
        if (!ts_node_is_null(f)) return unquote(doc_.text(f));
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) {
            TSNode child = ts_node_named_child(n, i);
            TSSymbol sym = ts_node_symbol(child);
            if (sym != valueAttrSym_ && sym != stringAttrSym_) continue;
            TSNode nameN = ts_node_named_child(child, 0);
            if (ts_node_is_null(nameN) || doc_.text(nameN) != field) continue;
            TSNode val = ts_node_child_by_field_name(child, "value", 5);
            if (!ts_node_is_null(val)) return unquote(doc_.text(val));
        }
        return std::nullopt;
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
        stopAtFirst_ = true;
        found_ = false;
        visited_.clear();
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c && !found_; ++i) sweep(ts_node_named_child(n, i), {});
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
        std::string_view t = doc_.text(f);
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
        for (TSNode d : index_->definitions(t)) out.push_back(d);
    }

    // ---- setup / helpers ----

    static Tri tri(bool b) { return b ? Tri::True : Tri::False; }

    static std::string unquote(std::string_view t) {
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'')) t = t.substr(1, t.size() - 2);
        return std::string(t);
    }

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
        const TSLanguage *lang = tree_sitter_gdml();
        valueAttrSym_ = ts_language_symbol_for_name(lang, "value_attribute", 15, true);
        stringAttrSym_ = ts_language_symbol_for_name(lang, "string_attribute", 16, true);
        static const char *nonElements[] = {
            "document", "prolog", "XMLDecl", "doctypedecl", "content",
            "CharData", "Comment", "PI", "CDSect", "Reference", "EntityRef", "CharRef",
            "AttValue", "Name", "Attribute", "value_attribute", "string_attribute",
            "gdml_value", "number", "identifier", "binary_expression", "unary_expression",
            "parenthesized_expression", "call_expression", "VersionNum", "EncName",
            "STag", "ETag", "EmptyElemTag",
        };
        for (const char *name : nonElements) {
            TSSymbol s = ts_language_symbol_for_name(lang, name, static_cast<uint32_t>(std::strlen(name)), true);
            if (s != 0) skip_.insert(s);
        }
    }
};

}  // namespace gg
