// test_match — assertion-based correctness test for the match engine. Runs a
// suite of queries against simple.gdml and checks the match count (the engine's
// behaviour was validated by hand against this file). Replaces the earlier
// NFA-vs-DFA differential check now that the DFA is the only engine.
//   test_match <simple.gdml>
#include "document.hpp"
#include "matchengine.hpp"
#include "query/nfa.hpp"
#include "query/ast.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_match <simple.gdml>\n");
        return 2;
    }
    // {query, expected match count} on simple.gdml (3 volumes; World holds 2
    // physvols placing v2 and v1; world ref=World; boxes/tubes/positions as in file).
    const std::vector<std::pair<std::string, std::size_t>> cases = {
        {"physvol", 2},
        {"volume", 3},
        {"volume / physvol", 2},
        {"physvol => volume", 2},
        {"world => volume", 1},
        {"volume / (physvol => volume)%", 3},
        {"structure // physvol", 2},
        {"box[name=b500]", 1},
        {"box[x>200]", 2},
        {"tube[z>900]", 1},
        {"position[name=~/px.*/]", 3},
        {"volume[physvol]", 1},
        {"volume[!physvol]", 2},
        {"box | tube", 6},
        {"cone[deltaphi>3]", 1},     // deltaphi=TWOPI=2*pi=6.28 (easy-T3)
        {"sphere[deltatheta>4]", 0}, // deltatheta=PI=3.14, not > 4
    };
    int failures = 0;
    try {
        gg::Document doc(argv[1]);
        for (const auto &[query, expected] : cases) {
            gg::Query q = gg::QueryParser::parseString(query);
            gg::Nfa nfa(q);
            gg::MatchEngine engine(doc, nfa);
            std::size_t got = engine.run().size();
            bool ok = got == expected;
            std::printf("%-32s expected=%zu got=%zu  %s\n", query.c_str(), expected, got,
                        ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "test_match: %s\n", e.what());
        return 2;
    }
    return failures == 0 ? 0 : 1;
}
