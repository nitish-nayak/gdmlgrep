// gg — GDML grep. Run a structural/graph query against a GDML file (or stdin)
// and print matching elements grep-style (file:line: first-line-of-source).
//   gg '<query>' <file.gdml|->
// Exit codes follow grep: 0 = matches found, 1 = none, 2 = error.
#include "document.hpp"
#include "matchengine.hpp"
#include "nfa.hpp"
#include "query.hpp"

#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: gg '<query>' <file.gdml|->\n");
        return 2;
    }
    try {
        gg::Query q = gg::QueryParser::parseString(argv[1]);
        gg::Document doc(argv[2]);
        gg::Nfa nfa(q, tree_sitter_gdml());
        for (const std::string &t : nfa.unknownTypes())
            std::fprintf(stderr, "gg: warning: unknown node type '%s'\n", t.c_str());

        gg::MatchEngine engine(doc, nfa);
        std::vector<TSNode> hits = engine.run();
        for (TSNode n : hits) {
            std::string_view text = doc.text(n);
            text = text.substr(0, text.find('\n'));
            std::printf("%s:%u: %.*s\n", doc.name().c_str(), doc.line(n),
                        static_cast<int>(text.size()), text.data());
        }
        return hits.empty() ? 1 : 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "gg: %s\n", e.what());
        return 2;
    }
}
