// bench — isolate walk time from parse time for the DFA match engine.
//   bench '<query>' <file.gdml> [iterations]
// Reports raw tree-sitter parse time, then the per-run walk time (parse
// excluded), so the two can be compared directly.
#include "document.hpp"
#include "matchengine.hpp"
#include "query/nfa.hpp"
#include "query/ast.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

using clk = std::chrono::steady_clock;
static double ms(clk::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bench '<query>' <file.gdml> [iterations]\n");
        return 2;
    }
    int iters = argc > 3 ? std::atoi(argv[3]) : 100;
    try {
        std::ifstream f(argv[2], std::ios::binary);
        std::stringstream buf; buf << f.rdbuf();
        std::string src = buf.str();
        TSParser *p = ts_parser_new();
        ts_parser_set_language(p, tree_sitter_gdml());
        auto t0 = clk::now();
        TSTree *tree = ts_parser_parse_string(p, nullptr, src.c_str(), (uint32_t)src.size());
        auto t1 = clk::now();
        ts_tree_delete(tree);
        ts_parser_delete(p);

        gg::Query q = gg::QueryParser::parseString(argv[1]);
        gg::Document doc(argv[2]);
        gg::Nfa nfa(q);
        gg::MatchEngine engine(doc, nfa);

        std::size_t n = engine.run().size();  // warm (builds the DFA)
        auto a = clk::now();
        for (int i = 0; i < iters; ++i) engine.run();
        auto b = clk::now();

        std::printf("file    : %s (%zu bytes)\n", argv[2], src.size());
        std::printf("query   : %s\n", argv[1]);
        std::printf("matches : %zu\n", n);
        std::printf("parse   : %.2f ms  (tree-sitter, excluded from walk)\n", ms(t1 - t0));
        std::printf("walk    : %.3f ms / run  (parse excluded)\n", ms(b - a) / iters);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "bench: %s\n", e.what());
        return 2;
    }
}
