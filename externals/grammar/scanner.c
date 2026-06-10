#define TS_GDML
#include "scanner.h"
#include "tree_sitter/array.h"

#include <string.h>

typedef Array(char) String;

typedef Array(String) Vector;

static inline bool string_eq(String *a, String *b) {
    if (a->size != b->size) {
        return false;
    }
    return memcmp(a->contents, b->contents, a->size) == 0;
}

// Length-aware compare of a scanned name against a literal; mirrors string_eq
// but takes a C string, so we don't NUL-terminate or build a String.
static inline bool string_eq_lit(const String *a, const char *lit) {
    size_t len = strlen(lit);
    return a->size == len && memcmp(a->contents, lit, len) == 0;
}

// GDML tags emitted as typed nodes; MUST stay in sync with GDML_TAGS in grammar.js.
static const char *const GDML_TAGS[] = {
    // document structure
    "gdml", "define", "materials", "solids", "structure", "setup", "userinfo",
    // define
    "constant", "variable", "quantity", "expression", "matrix", "position", "rotation", "scale",
    // materials
    "isotope", "element", "material", "atom", "composite", "fraction",
    "D", "Dref", "T", "Tref", "P", "Pref", "MEE", "MEEref", "RL", "RLref", "AL", "ALref",
    // solids
    "box", "cone", "cutTube", "elcone", "ellipsoid", "eltube", "hype", "orb", "para", "paraboloid",
    "polycone", "genericPolycone", "polyhedra", "genericPolyhedra", "sphere", "torus", "trap", "trd",
    "tet", "arb8", "xtru", "tessellated", "twistedbox", "twistedtrap", "twistedtrd", "twistedtubs",
    "tube", "union", "subtraction", "intersection", "multiUnion", "scaledSolid", "reflectedSolid",
    "opticalsurface", "property",
    // solid sub-elements
    "zplane", "rzpoint", "first", "second", "firstposition", "firstpositionref",
    "firstrotation", "firstrotationref", "multiUnionNode", "solid", "section",
    "twoDimVertex", "triangular", "quadrangular",
    // structure
    "volume", "assembly", "physvol", "volumeref", "solidref", "materialref",
    "positionref", "rotationref", "scaleref", "physvolref",
    "divisionvol", "replicavol", "paramvol", "parameters", "parameterised_position_size",
    "replicate_along_axis", "direction", "directionref", "width", "offset",
    "bordersurface", "skinsurface", "loop", "auxiliary", "file", "world",
    // parameterisation dimensions (Dimensions substitution group)
    "box_dimensions", "trd_dimensions", "trap_dimensions", "tube_dimensions", "cone_dimensions",
    "sphere_dimensions", "orb_dimensions", "torus_dimensions", "ellipsoid_dimensions",
    "para_dimensions", "polycone_dimensions", "polyhedra_dimensions", "hype_dimensions",
};
static const size_t GDML_TAG_COUNT = sizeof(GDML_TAGS) / sizeof(GDML_TAGS[0]);

static inline bool is_gdml_tag(const String *name) {
    for (size_t i = 0; i < GDML_TAG_COUNT; ++i) {
        if (string_eq_lit(name, GDML_TAGS[i])) return true;
    }
    return false;
}

// Zero-length stack entry holds the place of a GDML tag (real names are never empty).
static inline bool is_sentinel(const String *s) { return s->size == 0; }

static void push_sentinel(Vector *tags) {
    String sentinel = array_new();
    array_push(tags, sentinel);
}

static String scan_tag_name(TSLexer *lexer) {
    String tag_name = array_new();
    if (is_valid_name_start_char(lexer->lookahead)) {
        array_push(&tag_name, (char)lexer->lookahead);
        advance(lexer);
    }
    while (is_valid_name_char(lexer->lookahead)) {
        array_push(&tag_name, (char)lexer->lookahead);
        advance(lexer);
    }
    return tag_name;
}

static bool scan_start_tag_name(Vector *tags, TSLexer *lexer, const bool *valid_symbols) {
    lexer->mark_end(lexer);
    String tag_name = scan_tag_name(lexer);
    if (tag_name.size == 0) {
        array_delete(&tag_name);
        return false;
    }
    // Zero-width marker (end left at start): leave the name for the grammar to lex.
    // We obtain the typed node of the GDML tag like <physvol> directly in grammar.js through $.physvol, etc.
    if (valid_symbols[GDML_OPEN] && is_gdml_tag(&tag_name)) {
        array_delete(&tag_name);
        push_sentinel(tags);
        lexer->result_symbol = GDML_OPEN;
        return true;
    }

    lexer->mark_end(lexer);
    lexer->result_symbol = START_TAG_NAME;
    array_push(tags, tag_name);
    return true;
}

static bool scan_end_tag_name(Vector *tags, TSLexer *lexer, const bool *valid_symbols) {
    lexer->mark_end(lexer);
    String tag_name = scan_tag_name(lexer);
    if (tag_name.size == 0) {
        array_delete(&tag_name);
        return false;
    }

    // Zero-width marker; pop the sentinel its open tag pushed.
    if (valid_symbols[GDML_CLOSE] && is_gdml_tag(&tag_name)) {
        array_delete(&tag_name);
        if (tags->size > 0 && is_sentinel(array_back(tags))) {
            array_delete(&array_pop(tags));
        }
        lexer->result_symbol = GDML_CLOSE;
        return true;
    }

    lexer->mark_end(lexer);
    if (tags->size > 0 && string_eq(array_back(tags), &tag_name)) {
        array_delete(&array_pop(tags));
        lexer->result_symbol = END_TAG_NAME;
    } else {
        lexer->result_symbol = ERRONEOUS_END_NAME;
    }
    array_delete(&tag_name);
    return lexer->result_symbol == END_TAG_NAME;
}

static bool scan_self_closing_tag_delimiter(Vector *tags, TSLexer *lexer) {
    advance(lexer);
    advance_if_eq(lexer, '>');
    // Pops the sentinel of a GDML empty-element, or the name of a generic one.
    if (tags->size > 0) {
        array_delete(&array_pop(tags));
        lexer->result_symbol = SELF_CLOSING_TAG_DELIMITER;
    }
    return true;
}

/// Check if the lexer is in error recovery mode
static inline bool in_error_recovery(const bool *valid_symbols) {
    return valid_symbols[PI_TARGET] && valid_symbols[PI_CONTENT] && valid_symbols[COMMENT] &&
           valid_symbols[CHAR_DATA] && valid_symbols[CDATA];
}

/// Check if the lexer is in a char data node
static inline bool in_char_data(TSLexer *lexer) {
    return !lexer->eof(lexer) && lexer->lookahead != '<' && lexer->lookahead != '&';
}

/// Scan for a CharData node
static bool scan_char_data(TSLexer *lexer) {
    bool advanced_once = false;

    while (in_char_data(lexer)) {
        if (lexer->lookahead == ']') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == ']') {
                advance(lexer);
                if (lexer->lookahead == '>') {
                    advance(lexer);
                    if (advanced_once) {
                        lexer->result_symbol = CHAR_DATA;
                        return false;
                    }
                }
            }
        }
        advanced_once = true;
        if (in_char_data(lexer)) {
            advance(lexer);
        }
    }

    if (advanced_once) {
        lexer->mark_end(lexer);
        lexer->result_symbol = CHAR_DATA;
        return true;
    }
    return false;
}

/// Scan for a CData node
static bool scan_cdata(TSLexer *lexer) {
    bool advanced_once = false;

    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == ']') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == ']') {
                advance(lexer);
                if (lexer->lookahead == '>' && advanced_once) {
                    lexer->result_symbol = CDATA;
                    return true;
                }
            }
        }
        advanced_once = true;
        advance(lexer);
    }

    return false;
}

bool tree_sitter_gdml_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Vector *tags = (Vector *)payload;

    if (in_error_recovery(valid_symbols)) {
        return false;
    }

    if (valid_symbols[PI_TARGET]) {
        return scan_pi_target(lexer, valid_symbols);
    }

    if (valid_symbols[PI_CONTENT]) {
        return scan_pi_content(lexer);
    }

    if (valid_symbols[CHAR_DATA] && scan_char_data(lexer)) {
        return true;
    }

    if (valid_symbols[CDATA] && scan_cdata(lexer)) {
        return true;
    }

    switch (lexer->lookahead) {
        case '<':
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '!') {
                advance(lexer);
                return scan_comment(lexer);
            }
            break;
        case '/':
            if (valid_symbols[SELF_CLOSING_TAG_DELIMITER]) {
                return scan_self_closing_tag_delimiter(tags, lexer);
            }
            break;
        case '\0':
            break;
        default:
            // GDML_OPEN/GDML_CLOSE are only ever valid at a start/end tag respectively.
            if (valid_symbols[START_TAG_NAME] || valid_symbols[GDML_OPEN]) {
                return scan_start_tag_name(tags, lexer, valid_symbols);
            }
            if (valid_symbols[END_TAG_NAME] || valid_symbols[GDML_CLOSE]) {
                return scan_end_tag_name(tags, lexer, valid_symbols);
            }
    }

    return false;
}

void *tree_sitter_gdml_external_scanner_create() {
    Vector *tags = (Vector *)ts_calloc(1, sizeof(Vector));
    if (tags == NULL) abort();
    array_init(tags);
    return tags;
}

void tree_sitter_gdml_external_scanner_destroy(void *payload) {
    Vector *tags = (Vector *)payload;
    for (uint32_t i = 0; i < tags->size; ++i) {
        array_delete(array_get(tags, i));
    }
    array_delete(tags);
    ts_free(tags);
}

unsigned tree_sitter_gdml_external_scanner_serialize(void *payload, char *buffer) {
    Vector *tags = (Vector *)payload;
    uint32_t tag_count = tags->size > UINT16_MAX ? UINT16_MAX : tags->size;
    uint32_t serialized_tag_count = 0, size = sizeof tag_count;

    memcpy(&buffer[size], &tag_count, size);
    size += sizeof tag_count;

    for (; serialized_tag_count < tag_count; ++serialized_tag_count) {
        String *tag = array_get(tags, serialized_tag_count);
        uint32_t name_length = tag->size;
        if (name_length > UINT8_MAX) {
            name_length = UINT8_MAX;
        }
        if (size + 2 + name_length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
            break;
        }
        buffer[size++] = (char)name_length;
        if (name_length > 0) {
            memcpy(&buffer[size], tag->contents, name_length);
        }
        array_delete(tag);
        size += name_length;
    }

    memcpy(&buffer[0], &serialized_tag_count, sizeof serialized_tag_count);
    return size;
}

void tree_sitter_gdml_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Vector *tags = (Vector *)payload;

    for (unsigned i = 0; i < tags->size; ++i) {
        array_delete(array_get(tags, i));
    }
    array_delete(tags);

    if (length == 0) return;

    uint32_t size = 0, tag_count = 0, serialized_tag_count = 0;
    memcpy(&serialized_tag_count, &buffer[size], sizeof serialized_tag_count);
    size += sizeof serialized_tag_count;
    memcpy(&tag_count, &buffer[size], sizeof tag_count);
    size += sizeof tag_count;

    if (tag_count == 0) return;

    array_reserve(tags, tag_count);

    uint32_t iter = 0;
    for (; iter < serialized_tag_count; ++iter) {
        String tag = array_new();
        tag.size = (uint8_t)buffer[size++];
        if (tag.size > 0) {
            array_reserve(&tag, tag.size + 1);
            memcpy(tag.contents, &buffer[size], tag.size);
            size += tag.size;
        }
        array_push(tags, tag);
    }
    // add zero tags if we didn't read enough, this is because the
    // buffer had no more room but we held more tags.
    for (; iter < tag_count; ++iter) {
        String tag = array_new();
        array_push(tags, tag);
    }
}
