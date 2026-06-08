// Smoke test for PreWalk: build the reference index for a GDML file and
// print summary counts plus a couple of spot lookups.
//   test_prewalk <file.gdml> [name ...]
#include "document.hpp"
#include "prewalk.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_prewalk <file.gdml> [name ...]\n");
        return 2;
    }
    try {
        gg::Document doc(argv[1]);
        gg::PreWalk index(doc);

        size_t defNames = index.allDefs().size(), useNames = index.allUses().size();
        size_t defNodes = 0, useNodes = 0;
        for (const auto &[k, v] : index.allDefs()) defNodes += v.size();
        for (const auto &[k, v] : index.allUses()) useNodes += v.size();
        std::printf("defs: %zu names / %zu nodes\n", defNames, defNodes);
        std::printf("uses: %zu names / %zu nodes\n", useNames, useNodes);

        for (int i = 2; i < argc; ++i) {
            const char *name = argv[i];
            std::printf("  %-24s defs=%zu uses=%zu\n", name,
                        index.definitions(name).size(), index.uses(name).size());
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "test_prewalk: %s\n", e.what());
        return 2;
    }
    return 0;
}
