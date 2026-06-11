#pragma once
// Query.hpp — the query language: AST + a recursive-descent parser.
//
// A query is a property-path expression (à la SPARQL property paths): steps are
// operands, connectors are the edges between them, with grouping/alternation/repeat.
//
//   query   := ['/' | '//'] alt          // optional leading anchor; default floating
//   alt     := seq ('|' seq)*            // alternation (lowest precedence)
//   seq     := postfix (CONNECTOR postfix)*   // CONNECTOR = / // => ==>
//   postfix := atom ('%' | '?' | '+')*   // quantifiers (Kleene star is '%')
//   atom    := '(' alt ')' | step
//   step    := (IDENT | '*') pred*       // '*' = any-node wildcard
//   pred    := '[' ('!' pred | IDENT OP value | subpath) ']'
//   OP      := '=' | '=~' | '!=' | '<' | '<=' | '>' | '>='
//
// Lexing is parser-driven (scannerless) because a few tokens are context
// sensitive: '/' is the child connector between steps but a regex delimiter inside
// [...] after '=~', and a predicate value runs arbitrarily up to ']'.
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gg {

enum class Connector  { Child, Descendant, Deref, DerefClosure };  //  /   //   =>   ==>
enum class Quantifier { Star, Plus, Opt };                         //  %   +    ?
enum class Comparator { Eq, Ne, Lt, Le, Gt, Ge, Regex };           //  =  != < <= > >=  =~

// A scannerless cursor over the query text: the position plus the lexing
// primitives. The const methods (eof/peek/at) are pure lookahead; the non-const
// ones (match/skip_whitespace/expect) advance. fail() reports a parse error with
// the 1-based column.
struct Cursor {
    std::string_view src;
    std::size_t pos = 0;

    bool eof() const { return pos >= src.size(); }
    char peek() const { return eof() ? '\0' : src[pos]; }
    bool at(std::string_view s) const { return src.substr(pos, s.size()) == s; }
    bool match(std::string_view s) { if (at(s)) { pos += s.size(); return true; } return false; }
    void skip_whitespace() { while (!eof() && std::isspace((unsigned char)src[pos])) ++pos; }
    void expect(char c) {
        if (!match(std::string_view(&c, 1))) fail(std::string("expected '") + c + "'");
    }
    [[noreturn]] void fail(std::string_view msg) const {
        throw std::runtime_error(std::string(msg) + " (at column " + std::to_string(pos + 1) + ")");
    }
};

// A token-string -> enum entry. Tables of these drive the operator productions;
// list longer symbols first so matching is maximal-munch (==> before =>, etc.).
template <class E>
struct Token { std::string_view symbol; E val; };

// Match the first table entry whose symbol is at the cursor (tried in order,
// so longer symbols must come first for maximal munch); nullopt if none.
template <class E, std::size_t N>
std::optional<E> match_token(const std::array<Token<E>, N> &table, Cursor &cursor) {
    for (const auto &t : table) if (cursor.match(t.symbol)) return t.val;
    return std::nullopt;
}

static constexpr std::array<Token<Comparator>, 7> kComparators = {{
    {"=~", Comparator::Regex}, {"!=", Comparator::Ne}, {">=", Comparator::Ge},
    {"<=", Comparator::Le}, {"=", Comparator::Eq}, {">", Comparator::Gt}, {"<", Comparator::Lt},
}};

static constexpr std::array<Token<Connector>, 4> kConnectors = {{
    {"==>", Connector::DerefClosure}, {"=>", Connector::Deref},
    {"//", Connector::Descendant}, {"/", Connector::Child},
}};

static constexpr std::array<Token<Quantifier>, 3> kQuantifiers = {{
    {"%", Quantifier::Star}, {"+", Quantifier::Plus}, {"?", Quantifier::Opt},
}};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Predicate;
using PredicatePtr = std::unique_ptr<Predicate>;

// A predicate is one [...] test on a step. Multiple brackets on a step AND.
struct Predicate {
    enum class Kind { Compare, Exists, Not } kind;
    Comparator op{};                    // Compare: the operator
    NodePtr sub;                        // Exists:  the relative sub-path
    std::string field;                  // Compare: the field/attr name
    std::string value;                  // Compare: literal text (or regex source)
    PredicatePtr neg;                   // Not:     the negated predicate
};

// One AST node. A tagged sum type: Step is a leaf; Seq/Alt/Repeat are interior.
struct Node {
    enum class Kind { Step, Seq, Alt, Repeat } kind;
    Connector connector{};              // Seq:  edge connecting children[0] -> children[1]
    Quantifier quant{};                 // Repeat: the quantifier
    std::vector<NodePtr> children;      // Seq/Alt: children; Repeat: [inner]
    std::vector<Predicate> preds;       // Step: predicates (ANDed)
    bool wildcard = false;              // Step: matches any node type
    std::string type;                   // Step: node-type name ("" if wildcard)
};

struct Query {
    NodePtr root;
    bool anchored = false;              // leading '/' (root anchor); else floating
};


class QueryParser {
public:
    explicit QueryParser(std::string_view src) : cursor{src} {}

    Query parse() {
        cursor.skip_whitespace();
        Query q;
        if (cursor.match("//")) {              // explicit floating (same as default)
        } else if (cursor.match("/")) {        // leading '/' = root anchor
            q.anchored = true;
        }
        q.root = parse_alternation();
        cursor.skip_whitespace();
        if (!cursor.eof()) cursor.fail("unexpected trailing input");
        return q;
    }

    static Query parseString(std::string_view src) { return QueryParser(src).parse(); }

private:
    Cursor cursor;

    // --- productions ---
    NodePtr parse_alternation() {
        NodePtr left = parse_sequence();
        cursor.skip_whitespace();
        if (cursor.peek() != '|') return left;
        auto alt = std::make_unique<Node>();
        alt->kind = Node::Kind::Alt;
        alt->children.push_back(std::move(left));
        while (cursor.skip_whitespace(), cursor.match("|")) alt->children.push_back(parse_sequence());
        return alt;
    }

    NodePtr parse_sequence() {
        NodePtr left = parse_quantity();
        for (;;) {
            cursor.skip_whitespace();
            auto connector = match_token(kConnectors, cursor);
            if (!connector) break;
            auto seq = std::make_unique<Node>();
            seq->kind = Node::Kind::Seq;
            seq->connector = *connector;
            seq->children.push_back(std::move(left));
            seq->children.push_back(parse_quantity());
            left = std::move(seq);
        }
        return left;
    }

    NodePtr parse_quantity() {
        NodePtr a = parse_atom();
        for (;;) {
            cursor.skip_whitespace();
            auto q = match_token(kQuantifiers, cursor);
            if (!q) break;
            auto rep = std::make_unique<Node>();
            rep->kind = Node::Kind::Repeat;
            rep->quant = *q;
            rep->children.push_back(std::move(a));
            a = std::move(rep);
        }
        return a;
    }

    // An atom is a parenthesized group or a step (a node type or '*'), optionally
    // followed by predicates. A predicate on a group constrains the node the group
    // resolves to (e.g. `(position | positionref => position)[x>500]`).
    NodePtr parse_atom() {
        cursor.skip_whitespace();
        NodePtr a;
        if (cursor.match("(")) {
            a = parse_alternation();
            cursor.skip_whitespace();
            cursor.expect(')');
        } else {
            a = std::make_unique<Node>();
            a->kind = Node::Kind::Step;
            if (cursor.match("*")) a->wildcard = true;
            else if (std::isalpha((unsigned char)cursor.peek()) || cursor.peek() == '_') a->type = parse_identifier();
            else cursor.fail("expected a node type or '*'");
        }
        for (;;) {
            cursor.skip_whitespace();
            if (!cursor.match("[")) break;
            a->preds.push_back(parse_predicate());
            cursor.skip_whitespace();
            cursor.expect(']');
        }
        return a;
    }

    Predicate parse_predicate() {
        cursor.skip_whitespace();
        if (cursor.match("!")) {
            Predicate p;
            p.kind = Predicate::Kind::Not;
            p.neg = std::make_unique<Predicate>(parse_predicate());
            return p;
        }
        // Try "field OP value"; if no operator follows the identifier, rewind
        // and parse the bracket as a sub-path existence test.
        std::size_t save = cursor.pos;
        if (std::isalpha((unsigned char)cursor.peek()) || cursor.peek() == '_') {
            std::string field = parse_identifier();
            cursor.skip_whitespace();
            if (auto op = match_token(kComparators, cursor)) {
                Predicate p;
                p.kind = Predicate::Kind::Compare;
                p.field = field;
                p.op = *op;
                p.value = (*op == Comparator::Regex) ? parse_regex() : parse_value();
                return p;
            }
        }
        cursor.pos = save;
        Predicate p;
        p.kind = Predicate::Kind::Exists;
        p.sub = parse_alternation();
        return p;
    }

    // --- token helpers ---

    // Consume an identifier. Precondition: the cursor is at an identifier start
    // (isalpha or '_'); callers guard before calling, so this never returns empty.
    std::string parse_identifier() {
        std::size_t start = cursor.pos;
        ++cursor.pos;  // first char, guaranteed by the precondition
        while (std::isalnum((unsigned char)cursor.peek()) || cursor.peek() == '_') ++cursor.pos;
        return std::string(cursor.src.substr(start, cursor.pos - start));
    }

    // A comparison value runs up to ']' (so it can hold '*', '/', units, etc.).
    std::string parse_value() {
        cursor.skip_whitespace();
        std::size_t start = cursor.pos;
        while (!cursor.eof() && cursor.peek() != ']' && cursor.peek() != '[') ++cursor.pos;
        std::size_t end = cursor.pos;
        while (end > start && std::isspace((unsigned char)cursor.src[end - 1])) --end;  // rtrim
        return std::string(cursor.src.substr(start, end - start));
    }

    // A '/'-delimited regex literal; backslash escapes are preserved verbatim.
    std::string parse_regex() {
        cursor.skip_whitespace();
        cursor.expect('/');
        std::string re;
        while (!cursor.eof() && cursor.peek() != '/') {
            bool esc = cursor.peek() == '\\';
            re += cursor.src[cursor.pos++];
            if (esc && !cursor.eof()) re += cursor.src[cursor.pos++];
        }
        cursor.expect('/');
        return re;
    }
};

// Render an AST back to a normalized query string (for diagnostics/tests).
inline std::string toString(const Node &n);

inline std::string toString(const Predicate &p) {
    switch (p.kind) {
        case Predicate::Kind::Compare: {
            const char *op =
                p.op == Comparator::Eq ? "=" : p.op == Comparator::Ne ? "!=" :
                p.op == Comparator::Lt ? "<" : p.op == Comparator::Le ? "<=" :
                p.op == Comparator::Gt ? ">" : p.op == Comparator::Ge ? ">=" : "=~";
            if (p.op == Comparator::Regex) return "[" + p.field + "=~/" + p.value + "/]";
            return "[" + p.field + op + p.value + "]";
        }
        case Predicate::Kind::Exists: return "[" + toString(*p.sub) + "]";
        case Predicate::Kind::Not:    return "[!" + toString(*p.neg).substr(1);  // reuse inner '[...]'
    }
    return "[?]";
}

inline std::string toString(const Node &n) {
    std::string base;
    switch (n.kind) {
        case Node::Kind::Step:
            base = n.wildcard ? "*" : n.type;
            break;
        case Node::Kind::Seq: {
            const char *connector =
                n.connector == Connector::Child ? " / " : n.connector == Connector::Descendant ? " // " :
                n.connector == Connector::Deref ? " => " : " ==> ";
            base = toString(*n.children[0]) + connector + toString(*n.children[1]);
            if (!n.preds.empty()) base = "(" + base + ")";  // bind preds to the whole seq
            break;
        }
        case Node::Kind::Alt: {
            base = "(";
            for (size_t i = 0; i < n.children.size(); ++i) base += (i ? " | " : "") + toString(*n.children[i]);
            base += ")";
            break;
        }
        case Node::Kind::Repeat: {
            const char *q = n.quant == Quantifier::Star ? "%" : n.quant == Quantifier::Plus ? "+" : "?";
            base = "(" + toString(*n.children[0]) + ")" + q;
            break;
        }
    }
    for (const auto &p : n.preds) base += toString(p);
    return base;
}

}  // namespace gg
