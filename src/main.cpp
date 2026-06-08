// gg — GDML grep. Scaffold smoke test: parse a file (or stdin) and dump the
// named-node tree. The query engine replaces this dump in later commits.
#include "document.hpp"

#include <cstdio>
#include <exception>

namespace {

void dump(const gg::Document &doc, TSNode n, int depth) {
    if (ts_node_is_null(n)) return;
    std::printf("%*s%s [%u]\n", depth * 2, "", ts_node_type(n), doc.line(n));
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i)
        dump(doc, ts_node_named_child(n, i), depth + 1);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gg <file.gdml|->\n");
        return 2;
    }
    try {
        gg::Document doc(argv[1]);
        dump(doc, doc.root(), 0);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "gg: %s\n", e.what());
        return 2;
    }
    return 0;
}
