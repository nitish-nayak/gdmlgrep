#pragma once
// matchengine.hpp — the top-level query entry point. Compiles the Nfa to a Dfa,
// then walks the document with a PredEval supplying the value-predicate guards
// (walker.hpp / predeval.hpp hold the two halves).
#include "query/nfa.hpp"
#include "query/dfa.hpp"
#include "query/grammar.hpp"
#include "document.hpp"
#include "prewalk.hpp"
#include "predeval.hpp"
#include "walker.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gg {

struct MatchResult {
    std::vector<TSNode> matches;                                  // accept nodes, document order
    std::vector<std::pair<std::string, TSNode>> unevaluable;      // guards we couldn't evaluate
};

// Run a compiled query over doc. `shared` lets several queries reuse one reference
// index across the same document; otherwise one is built here iff the query derefs.
inline MatchResult run_query(const Document &doc, const Nfa &nfa, PreWalk *shared = nullptr) {
    std::unique_ptr<PreWalk> owned;
    const PreWalk *index = shared;
    if (!index && nfa.needsDeref()) { owned = std::make_unique<PreWalk>(doc); index = owned.get(); }

    Dfa dfa(nfa, kSkipSymbols);
    PredEval pred(doc, index);
    auto matches = Walker(dfa, doc, index,
                          [&](int s, TSNode n) { return filter_guards(dfa, pred, s, n); }).collect(doc.root());
    return { std::move(matches), pred.unevaluable() };
}

}  // namespace gg
