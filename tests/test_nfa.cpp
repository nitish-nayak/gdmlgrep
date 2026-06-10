// Dump test for Nfa: parse a query, build the Glushkov automaton, print it.
//   test_nfa '<query>'
#include "query/nfa.hpp"
#include "query/ast.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_nfa '<query>'\n");
        return 2;
    }
    try {
        gg::Query q = gg::QueryParser::parseString(argv[1]);
        gg::Nfa nfa(q, tree_sitter_gdml());
        nfa.dump();
        for (const std::string &t : nfa.unknownTypes())
            std::fprintf(stderr, "warning: unknown node type '%s'\n", t.c_str());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "test_nfa: %s\n", e.what());
        return 2;
    }
    return 0;
}
