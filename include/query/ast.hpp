#pragma once
// ast.hpp — the query language: AST + a recursive-descent parser.
//
// A query is a property-path expression (à la SPARQL property paths): steps are
// operands, connectors are the edges between them, with grouping/alternation/repeat.
//
//   query   := ['/' | '//'] alt          // optional leading anchor; default floating
//   alt     := seq ('|' seq)*            // alternation (lowest precedence)
//   seq     := quant (CONNECTOR quant)*  // CONNECTOR = / // => ==>
//   quant   := atom ('%' | '?' | '+')*   // quantifiers (Kleene star is '%')
//   atom    := '(' alt ')' | step
//   step    := (IDENT | '*') pred*       // '*' = any-node wildcard
//   pred    := '[' ('!' pred | IDENT OP value | subpath) ']'
//   OP      := '=' | '=~' | '!=' | '<' | '<=' | '>' | '>='
//
// Lexing is parser-driven (scannerless) because a few tokens are context
// sensitive: '/' is the child connector between steps but a regex delimiter inside
// [...] after '=~', and a predicate value runs arbitrarily up to ']'.
//
// The node/predicate kinds are std::variant alternatives — the active alternative
// IS the kind, so there's no hand-maintained discriminant. Each alternative knows
// how to render itself (toString); structural consumers (nfa/matchengine) dispatch
// with std::visit + the `overloaded` helper.
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
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
    bool at(std::string_view s) const {
      return src.substr(pos, s.size()) == s;
    }
    bool match(std::string_view s) {
      if (at(s)) {
        pos += s.size(); return true;
      } return false;
    }
    char peek() const { return eof() ? '\0' : src[pos]; }
    void skip_whitespace() {
      while (!eof() && std::isspace((unsigned char)src[pos]))
        ++pos;
    }
    void expect(char c) {
        if (!match(std::string_view(&c, 1)))
          fail(std::string("expected '") + c + "'");
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

// Visit helper: lets std::visit take an overload set of lambdas, one per
// alternative (used by the NFA builder and the match engine).
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Node;
struct Predicate;
using NodePtr = std::unique_ptr<Node>;
using PredicatePtr = std::unique_ptr<Predicate>;

// ---- predicate alternatives (the active variant member is the kind) ----
struct Compare { std::string field; Comparator op; std::string value;  std::string toString() const; };  // field OP value
struct Exists  { NodePtr sub;                                          std::string toString() const; };  // [subpath]
struct Not     { PredicatePtr neg;                                     std::string toString() const; };  // [!pred]

// A predicate is one [...] test on a step. Multiple brackets on a step AND.
using PredicateVariant = std::variant<Compare, Exists, Not>;
struct Predicate {
    PredicateVariant value;
    std::string toString() const;
};

// ---- node alternatives ----
struct Step   { std::string type; bool wildcard = false;  std::string toString() const; };  // a node type, or '*'
struct Seq    { Connector connector; NodePtr lhs, rhs;    std::string toString() const; };  // lhs CONNECTOR rhs
struct Alt    { std::vector<NodePtr> branches;            std::string toString() const; };  // a | b | ...
struct Repeat { Quantifier quant; NodePtr inner;          std::string toString() const; };  // inner% / inner+ / inner?

// A node carries its kind (the active alternative) plus the predicates that
// decorate it. preds is orthogonal to the kind — a predicate can constrain a
// step OR the node a group resolves to (e.g. `(a | b)[x>5]`).
using NodeVariant = std::variant<Step, Seq, Alt, Repeat>;
struct Node {
    NodeVariant value;
    std::vector<Predicate> preds;          // ANDed
    std::string toString() const;
};

// AST constructors. Each pairs a kind with the payload it carries, so the
// kind <-> Connector/Quantifier/Comparator relationships are legible at a glance.
inline NodePtr fetch_node(NodeVariant value) {
    auto n = std::make_unique<Node>();
    n->value = std::move(value);
    return n;
}
inline NodePtr step(std::string type) { return fetch_node(Step{std::move(type), false}); }
inline NodePtr wildcard()             { return fetch_node(Step{"", true}); }
inline NodePtr seq(Connector connector, NodePtr lhs, NodePtr rhs) { return fetch_node(Seq{connector, std::move(lhs), std::move(rhs)}); }
inline NodePtr alt(std::vector<NodePtr> branches)                 { return fetch_node(Alt{std::move(branches)}); }
inline NodePtr repeat(Quantifier quant, NodePtr inner)            { return fetch_node(Repeat{quant, std::move(inner)}); }
inline Predicate compare(std::string field, Comparator op, std::string value) {
    return Predicate{Compare{std::move(field), op, std::move(value)}};
}
inline Predicate exists(NodePtr sub) { return Predicate{Exists{std::move(sub)}}; }
inline Predicate negated(Predicate inner) {
    return Predicate{Not{std::make_unique<Predicate>(std::move(inner))}};
}

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
        if (cursor.match("//")) {
          // explicit floating (same as default)
        } else if (cursor.match("/")) {
          // leading '/' = root anchor
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
        std::vector<NodePtr> branches;
        branches.push_back(std::move(left));
        while (cursor.skip_whitespace(), cursor.match("|")) branches.push_back(parse_sequence());
        return alt(std::move(branches));
    }

    NodePtr parse_sequence() {
        NodePtr left = parse_quantity();
        for (;;) {
            cursor.skip_whitespace();
            auto connector = match_token(kConnectors, cursor);
            if (!connector) break;
            left = seq(*connector, std::move(left), parse_quantity());
        }
        return left;
    }

    NodePtr parse_quantity() {
        NodePtr a = parse_atom();
        for (;;) {
            cursor.skip_whitespace();
            auto q = match_token(kQuantifiers, cursor);
            if (!q) break;
            a = repeat(*q, std::move(a));
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
        } else if (cursor.match("*")) {
            a = wildcard();
        } else if (std::isalpha((unsigned char)cursor.peek()) || cursor.peek() == '_') {
            a = step(parse_identifier());
        } else {
            cursor.fail("expected a node type or '*'");
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
        if (cursor.match("!")) return negated(parse_predicate());
        // Try "field OP value"; if no operator follows the identifier, rewind
        // and parse the bracket as a sub-path existence test.
        std::size_t save = cursor.pos;
        if (std::isalpha((unsigned char)cursor.peek()) || cursor.peek() == '_') {
            std::string field = parse_identifier();
            cursor.skip_whitespace();
            if (auto op = match_token(kComparators, cursor))
                return compare(std::move(field), *op,
                               (*op == Comparator::Regex) ? parse_regex() : parse_value());
        }
        cursor.pos = save;
        return exists(parse_alternation());
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

// --- toString: each alternative renders itself; Node/Predicate dispatch with a
// --- generic visitor. Defined out-of-line because Seq/Alt/Repeat/Exists/Not
// --- recurse through NodePtr/PredicatePtr, which need the wrappers complete.
inline std::string Step::toString() const { return wildcard ? "*" : type; }

inline std::string Seq::toString() const {
    const char *c = "";
    switch (connector) {
        case Connector::Child:        c = " / ";   break;
        case Connector::Descendant:   c = " // ";  break;
        case Connector::Deref:        c = " => ";  break;
        case Connector::DerefClosure: c = " ==> "; break;
    }
    return lhs->toString() + c + rhs->toString();
}

inline std::string Alt::toString() const {
    std::string s = "(";
    for (size_t i = 0; i < branches.size(); ++i)
      s += (i ? " | " : "") + branches[i]->toString();
    return s + ")";
}

inline std::string Repeat::toString() const {
    const char *q = "";
    switch (quant) {
        case Quantifier::Star: q = "%"; break;
        case Quantifier::Plus: q = "+"; break;
        case Quantifier::Opt:  q = "?"; break;
    }
    return "(" + inner->toString() + ")" + q;
}

inline std::string Compare::toString() const {
    const char *o = "";
    switch (op) {
        case Comparator::Eq:    o = "=";  break;
        case Comparator::Ne:    o = "!="; break;
        case Comparator::Lt:    o = "<";  break;
        case Comparator::Le:    o = "<="; break;
        case Comparator::Gt:    o = ">";  break;
        case Comparator::Ge:    o = ">="; break;
        case Comparator::Regex: return "[" + field + "=~/" + value + "/]";  // value is a /…/ regex literal
    }
    return "[" + field + o + value + "]";
}

inline std::string Exists::toString() const { return "[" + sub->toString() + "]"; }

inline std::string Not::toString() const { return "[!" + neg->toString().substr(1); }  // reuse inner '[...]'

inline std::string Node::toString() const {
    std::string s = std::visit([](const auto &a) { return a.toString(); }, value);
    if (!preds.empty() && std::holds_alternative<Seq>(value)) s = "(" + s + ")";  // bind preds to the seq
    for (const auto &p : preds) s += p.toString();
    return s;
}

inline std::string Predicate::toString() const {
    return std::visit([](const auto &a) { return a.toString(); }, value);
}

}  // namespace gg
