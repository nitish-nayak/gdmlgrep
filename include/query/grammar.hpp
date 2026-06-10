#pragma once
// Low-level tree-sitter glue: the generated grammar's entry point plus RAII
// wrappers so the C objects free themselves. Everything else builds on this.
#include <tree_sitter/api.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// Defined in the vendored, generated grammar (externals/grammar/parser.c).
extern "C" const TSLanguage *tree_sitter_gdml(void);

namespace gg {

struct ParserDeleter { void operator()(TSParser *p) const { ts_parser_delete(p); } };
struct TreeDeleter   { void operator()(TSTree   *t) const { ts_tree_delete(t);   } };

using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr   = std::unique_ptr<TSTree,   TreeDeleter>;

// Raw name -> symbol id; 0 when the grammar has no such node type. Use this when
// absence is tolerable (e.g. the matcher's skip set of maybe-absent XML names).
inline TSSymbol sym(const char *name) {
    return ts_language_symbol_for_name(tree_sitter_gdml(), name,
                                       static_cast<uint32_t>(std::strlen(name)), true);
}

// A fixed grammar symbol resolved by name. The catalog below is built at startup,
// so this turns a typo or a renamed grammar rule into an immediate abort instead
// of a symbol-0 that silently never matches any node — the quiet failure mode
// this guards against. The check is unconditional (not assert) because the binary
// ships -DNDEBUG (Release), which would strip an assert. (Only construct Symbol
// for names that MUST exist; for best-effort lookups use sym().)
class Symbol {
public:
    explicit Symbol(const char *name) : s(sym(name)) {
        if (s == 0) {
            std::fprintf(stderr, "gg: internal error: '%s' is not a node type in the GDML grammar\n", name);
            std::abort();
        }
    }
    TSSymbol id() const { return s; }
    bool valid() const { return s != 0; }
private:
    TSSymbol s;
};

inline bool operator==(TSSymbol tss, const Symbol &o) { return tss == o.id(); }
inline bool operator!=(TSSymbol tss, const Symbol &o) { return tss != o.id(); }

static const Symbol kPHYSVOL("physvol");
static const Symbol kVOLUMEREF("volumeref");
static const Symbol kDIVISIONVOL("divisionvol");
static const Symbol kREPLICAVOL("replicavol");
static const Symbol kPARAMVOL("paramvol");
static const Symbol kWORLD("world");
static const Symbol kIDENTIFIER("identifier");
static const Symbol kATTRIBUTE("Attribute");
static const Symbol kNUMBER("number");
static const Symbol kBINARY("binary_expression");
static const Symbol kUNARY("unary_expression");
static const Symbol kPAREN("parenthesized_expression");
static const Symbol kCALL("call_expression");
static const Symbol kCONSTANT("constant");
static const Symbol kQUANTITY("quantity");
static const Symbol kVALUE("value_attribute");
static const Symbol kSTRING("string_attribute");

}  // namespace gg
