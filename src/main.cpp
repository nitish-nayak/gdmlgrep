// gg — GDML grep.
//   gg '<query>' <file.gdml|->        run a structural/graph query
//   gg placement-tree <file>          geometry placement hierarchy from <world>
//   gg dead-defs     <file>           names defined but never referenced
//   gg dangling      <file>           names referenced but never defined
//   gg find-usages <name> <file>      sites referencing <name>
// Query mode prints matches grep-style and follows grep exit codes
// (0 = matches, 1 = none, 2 = error); verbs return 0 on success, 2 on error.
#include "document.hpp"
#include "matchengine.hpp"
#include "nfa.hpp"
#include "prewalk.hpp"
#include "query.hpp"
#include "verbs.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

int runVerb(const char *verb, char **argv, int argc) {
    // gg <verb> <file>   (find-usages takes an extra <name> before <file>)
    bool findUsages = std::strcmp(verb, "find-usages") == 0;
    int want = findUsages ? 4 : 3;
    if (argc != want) {
        std::fprintf(stderr, "usage: gg %s %s<file.gdml|->\n", verb, findUsages ? "<name> " : "");
        return 2;
    }
    gg::Document doc(argv[argc - 1]);
    gg::PreWalk index(doc);
    gg::Verbs verbs(doc, index);
    if (findUsages) verbs.findUsages(argv[2]);
    else if (std::strcmp(verb, "placement-tree") == 0) verbs.placementTree();
    else if (std::strcmp(verb, "dead-defs") == 0) verbs.deadDefs();
    else verbs.dangling();
    return 0;
}

int runQuery(const char *query, const char *file) {
    gg::Query q = gg::QueryParser::parseString(query);
    gg::Document doc(file);
    gg::Nfa nfa(q, tree_sitter_gdml());
    for (const std::string &t : nfa.unknownTypes())
        std::fprintf(stderr, "gg: warning: unknown node type '%s'\n", t.c_str());

    gg::MatchEngine engine(doc, nfa);
    std::vector<TSNode> hits = engine.run();
    for (const auto &u : engine.unevaluable())
        std::fprintf(stderr, "gg: warning: could not evaluate %s at %s:%u\n",
                     u.first.c_str(), doc.name().c_str(), doc.line(u.second));
    for (TSNode n : hits) gg::emitLine(doc, n);
    return hits.empty() ? 1 : 0;
}

bool isVerb(const char *s) {
    return !std::strcmp(s, "placement-tree") || !std::strcmp(s, "dead-defs") ||
           !std::strcmp(s, "dangling") || !std::strcmp(s, "find-usages");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gg '<query>' <file.gdml|->\n"
                             "       gg <placement-tree|dead-defs|dangling> <file>\n"
                             "       gg find-usages <name> <file>\n");
        return 2;
    }
    try {
        if (isVerb(argv[1])) return runVerb(argv[1], argv, argc);
        return runQuery(argv[1], argv[2]);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "gg: %s\n", e.what());
        return 2;
    }
}
