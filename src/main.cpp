// gg — GDML grep.
//   gg '<query>' <file.gdml|->          run one query
//   gg -e '<query>' [-e '<query>'...] <file>   run several queries on one parse
//   gg placement-tree <file>            geometry placement hierarchy from <world>
//   gg dead-defs     <file>             names defined but never referenced
//   gg dangling      <file>             names referenced but never defined
//   gg find-usages <name> <file>        sites referencing <name>
// Flags: -c count only, -o <field> emit a field (name/ref/type/attr), -q quiet
// (exit code only). With multiple queries the file is parsed (and indexed) once
// and each query runs against the shared tree, each block headed "==> q <==".
// Query mode follows grep exit codes (0 = matches, 1 = none, 2 = error); verbs
// return 0 on success, 2 on error.
#include "document.hpp"
#include "matchengine.hpp"
#include "nfa.hpp"
#include "prewalk.hpp"
#include "query.hpp"
#include "verbs.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

int runVerb(const char *verb, char **argv, int argc) {
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

struct Options {
    bool quiet = false;       // -q: no output, exit code only
    bool count = false;       // -c: print match count only
    std::string field;        // -o <field>: emit this field instead of the line
};

// Parse `file` once, then run each query against the shared tree. A PreWalk
// index is built at most once, lazily, and shared across deref queries.
int runQueries(const std::vector<std::string> &queries, const char *file, const Options &opt) {
    gg::Document doc(file);
    std::unique_ptr<gg::PreWalk> shared;
    bool multi = queries.size() > 1;
    bool anyMatch = false;
    for (const std::string &query : queries) {
        gg::Query q = gg::QueryParser::parseString(query);
        gg::Nfa nfa(q, tree_sitter_gdml());
        for (const std::string &t : nfa.unknownTypes())
            std::fprintf(stderr, "gg: warning: unknown node type '%s' in '%s'\n", t.c_str(), query.c_str());
        if (nfa.needsDeref() && !shared) shared = std::make_unique<gg::PreWalk>(doc);

        gg::MatchEngine engine(doc, nfa, shared.get());
        std::vector<TSNode> hits = engine.run();
        for (const auto &u : engine.unevaluable())
            std::fprintf(stderr, "gg: warning: could not evaluate %s at %s:%u\n",
                         u.first.c_str(), doc.name().c_str(), doc.line(u.second));
        anyMatch = anyMatch || !hits.empty();

        if (opt.quiet) continue;
        if (opt.count) {
            if (multi) std::printf("%s:%zu\n", query.c_str(), hits.size());
            else std::printf("%zu\n", hits.size());
            continue;
        }
        if (multi) std::printf("==> %s <==\n", query.c_str());
        for (TSNode n : hits) {
            if (opt.field.empty()) { gg::emitLine(doc, n); continue; }
            if (auto v = gg::fieldOf(doc, n, opt.field)) std::printf("%s\n", v->c_str());
        }
    }
    return anyMatch ? 0 : 1;
}

bool isVerb(const char *s) {
    return !std::strcmp(s, "placement-tree") || !std::strcmp(s, "dead-defs") ||
           !std::strcmp(s, "dangling") || !std::strcmp(s, "find-usages");
}

void usage() {
    std::fprintf(stderr, "usage: gg [-c] [-q] [-o <field>] '<query>' <file.gdml|->\n"
                         "       gg [-c] [-q] [-o <field>] -e '<query>' [-e '<query>'...] <file>\n"
                         "       gg <placement-tree|dead-defs|dangling> <file>\n"
                         "       gg find-usages <name> <file>\n");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    try {
        if (isVerb(argv[1])) return runVerb(argv[1], argv, argc);

        Options opt;
        std::vector<std::string> queries;
        std::vector<const char *> positional;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-e") == 0 && i + 1 < argc) queries.push_back(argv[++i]);
            else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) opt.field = argv[++i];
            else if (std::strcmp(argv[i], "-c") == 0) opt.count = true;
            else if (std::strcmp(argv[i], "-q") == 0) opt.quiet = true;
            else positional.push_back(argv[i]);
        }
        if (!queries.empty()) {
            if (positional.size() != 1) { usage(); return 2; }  // exactly the file
            return runQueries(queries, positional[0], opt);
        }
        if (positional.size() != 2) { usage(); return 2; }       // <query> <file>
        return runQueries({positional[0]}, positional[1], opt);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "gg: %s\n", e.what());
        return 2;
    }
}
