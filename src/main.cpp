// gg — GDML grep.
//   gg '<query>' <file.gdml|->          run one query
//   gg -e '<query>' [-e '<query>'...] <file>   run several queries on one parse
//   gg placement-tree [volume] <file>   geometry placement hierarchy from <world> (or <volume>)
//   gg dead-defs     <file>             names defined but never referenced
//   gg dangling      <file>             names referenced but never defined
//   gg find-usages <name> <file>        sites referencing <name>
// Flags: -c count only, -o <field> emit a field (name/ref/type/attr), -q quiet
// (exit code only). With multiple queries the file is parsed (and indexed) once
// and each query runs against the shared tree, each block headed "==> q <==".
// Query mode follows grep exit codes (0 = matches, 1 = none, 2 = error); verbs
// return 0 on success, 2 on error.
#include "utils/output.hpp"
#include "utils/nodeattr.hpp"
#include "document.hpp"
#include "prewalk.hpp"
#include "query/nfa.hpp"
#include "query/ast.hpp"
#include "matchengine.hpp"
#include "verbs.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

int runVerb(const std::vector<const char *> &pos, bool pretty) {
    const char *verb = pos[0];
    bool findUsages = std::strcmp(verb, "find-usages") == 0;
    bool placementTree = std::strcmp(verb, "placement-tree") == 0;
    // find-usages needs <name> <file>; placement-tree takes an optional <volume>
    // before <file>; the rest take just <file>.
    bool ok = findUsages ? pos.size() == 3
            : placementTree ? pos.size() == 2 || pos.size() == 3
            : pos.size() == 2;
    if (!ok) {
        std::fprintf(stderr, "usage: gg %s %s<file.gdml|->\n", verb,
                     findUsages ? "<name> " : placementTree ? "[volume] " : "");
        return 2;
    }
    gg::Document doc(pos.back());
    gg::PreWalk index(doc);
    gg::Verbs verbs(doc, index, pretty);
    if (findUsages) verbs.findUsages(pos[1]);
    else if (placementTree) verbs.placementTree(pos.size() == 3 ? pos[1] : "");
    else if (std::strcmp(verb, "dead-defs") == 0) verbs.deadDefs();
    else verbs.dangling();
    return 0;
}

struct Options {
    bool quiet = false;       // -q: no output, exit code only
    bool count = false;       // -c: print match count only
    bool pretty = false;      // --pretty: ANSI-colorize stdout
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
        gg::Nfa nfa(q);
        for (const std::string &t : nfa.unknownTypes())
            std::fprintf(stderr, "gg: warning: unknown node type '%s' in '%s'\n", t.c_str(), query.c_str());
        if (nfa.needsDeref() && !shared) shared = std::make_unique<gg::PreWalk>(doc);

        gg::MatchResult result = gg::run_query(doc, nfa, shared.get());
        const std::vector<TSNode> &hits = result.matches;
        for (const auto &u : result.unevaluable)
            std::fprintf(stderr, "gg: warning: could not evaluate %s at %s:%u\n",
                         u.first.c_str(), doc.get_name().c_str(), doc.line(u.second));
        anyMatch = anyMatch || !hits.empty();

        if (opt.quiet) continue;
        if (opt.count) {
            if (multi) std::printf("%s:%zu\n", query.c_str(), hits.size());
            else std::printf("%zu\n", hits.size());
            continue;
        }
        if (multi) {
            if (opt.pretty) std::printf("%s==> %s <==%s\n", gg::ansi::dim, query.c_str(), gg::ansi::reset);
            else std::printf("==> %s <==\n", query.c_str());
        }
        for (TSNode n : hits) {
            if (opt.field.empty()) { gg::emitLine(doc, n, opt.pretty); continue; }
            if (auto v = gg::fieldOf(doc, n, opt.field)) std::printf("%s\n", v->c_str());
        }
    }
    return anyMatch ? 0 : 1;
}

bool isVerb(const char *s) {
    return !std::strcmp(s, "placement-tree") || !std::strcmp(s, "dead-defs") ||
           !std::strcmp(s, "dangling") || !std::strcmp(s, "find-usages");
}

void usage(std::FILE *out) {
    std::fprintf(out, "usage: gg [-c] [-q] [-o <field>] [--pretty] '<query>' <file.gdml|->\n"
                      "       gg [flags] -e '<query>' [-e '<query>'...] <file>\n"
                      "       gg [--pretty] placement-tree [volume] <file>\n"
                      "       gg [--pretty] <dead-defs|dangling> <file>\n"
                      "       gg [--pretty] find-usages <name> <file>\n"
                      "\n"
                      "flags:\n"
                      "  -e <query>   add a query (repeatable); the file is parsed once\n"
                      "  -c           print match count only\n"
                      "  -o <field>   emit a field (name/ref/type/attr) instead of the line\n"
                      "  -q           quiet: no output, exit status only\n"
                      "  --pretty     ANSI-colorized output (tree connectors for placement-tree)\n"
                      "  -h, --help   show this help\n");
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    std::vector<std::string> queries;
    std::vector<const char *> positional;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (std::strcmp(argv[i], "-e") == 0 && i + 1 < argc) queries.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) opt.field = argv[++i];
        else if (std::strcmp(argv[i], "-c") == 0) opt.count = true;
        else if (std::strcmp(argv[i], "-q") == 0) opt.quiet = true;
        else if (std::strcmp(argv[i], "--pretty") == 0) opt.pretty = true;
        else positional.push_back(argv[i]);
    }
    if (positional.empty()) { usage(stderr); return 2; }
    try {
        if (isVerb(positional[0])) return runVerb(positional, opt.pretty);
        if (!queries.empty()) {
            if (positional.size() != 1) { usage(stderr); return 2; }  // exactly the file
            return runQueries(queries, positional[0], opt);
        }
        if (positional.size() != 2) { usage(stderr); return 2; }       // <query> <file>
        return runQueries({positional[0]}, positional[1], opt);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "gg: %s\n", e.what());
        return 2;
    }
}
