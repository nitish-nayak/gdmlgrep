#pragma once
// Low-level tree-sitter glue: the generated grammar's entry point plus RAII
// wrappers so the C objects free themselves. Everything else builds on this.
#include <tree_sitter/api.h>

#include <memory>

// Defined in the vendored, generated grammar (vendor/grammar/parser.c).
extern "C" const TSLanguage *tree_sitter_gdml(void);

namespace gg {

struct ParserDeleter { void operator()(TSParser *p) const { ts_parser_delete(p); } };
struct TreeDeleter   { void operator()(TSTree   *t) const { ts_tree_delete(t);   } };

using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr   = std::unique_ptr<TSTree,   TreeDeleter>;

}  // namespace gg
