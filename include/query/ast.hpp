#pragma once
// ast.hpp — the query language: the parsed query tree (AST) and the
// recursive-descent parser that produces it.
//
// A query is a property-path expression :
// node-type steps joined by connectors, with
// grouping, alternation, quantifiers, and predicate tests.
// The grammar:
//
//   query   := ['/' | '//'] alt          // leading '/' anchors at the root; default is floating
//   alt     := seq ('|' seq)*            // alternation (lowest precedence)
//   seq     := quant (CONNECTOR quant)*  // CONNECTOR = / // => ==>
//   quant   := atom ('%' | '?' | '+')*   // quantifiers (Kleene star is '%')
//   atom    := '(' alt ')' | step
//   step    := (IDENT | '*') pred*       // '*' matches any node type
//   pred    := '[' ('!' pred | IDENT OP value | subpath) ']'
//   OP      := '=' | '=~' | '!=' | '<' | '<=' | '>' | '>='
//
// Every node and predicate kind is a std::variant alternative, so the tree is a
// sum type with no separate discriminant. Parsing is scannerless — there is no
// lexer pass — because a few tokens are context-dependent: '/' is a connector
// between steps but a regex delimiter inside [...], and a predicate value runs to
// the closing ']'.
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

// --------------------------
// Tracks the parse position over the source and provides the lexing primitives.
// The const methods (eof/at/peek) only inspect; the others advance the position.
// fail() raises a parse error carrying the 1-based column and never returns.
// --------------------------
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

// --------------------------
// One (symbol, enum) entry in an operator table.
// --------------------------
template <class E>
struct Token { std::string_view symbol; E val; };

// Consume the first table symbol present at the cursor and return its enum value.
// The tables list longer symbols first, so this resolves maximal-munch (==> wins
// over =>). Returns nullopt when no symbol matches.
template <class E, std::size_t N>
std::optional<E> match_token(const std::array<Token<E>, N> &table, Cursor &cursor) {
    for (const auto &t : table) if (cursor.match(t.symbol)) return t.val;
    return std::nullopt;
}

// The operator tables, longest symbol first (see match_token).
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

// --------------------------
// Lets std::visit take a set of per-alternative lambdas (used to dispatch over
// the AST variants here and in nfa/matchengine).
// --------------------------
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// forward declare
struct Node;
struct Predicate;
using NodePtr = std::unique_ptr<Node>;
using PredicatePtr = std::unique_ptr<Predicate>;

// --------------------------
// The three predicate kinds.
// --------------------------
struct Compare {
    std::string field; Comparator op; std::string value;  // field OP value
    std::string toString() const;
};
struct Exists {
    NodePtr sub; // [ subpath ]
    std::string toString() const;
};
struct Not {
    PredicatePtr neg; // [ !pred ]
    std::string toString() const;
};
// A test written inside [...] on a node. The active alternative is its kind.
using PredicateVariant = std::variant<Compare, Exists, Not>;
struct Predicate {
    PredicateVariant value;
    std::string toString() const;
};

// --------------------------
// The four node kinds.
// --------------------------
struct Step {
    std::string type; bool wildcard = false;  // a node type, or '*'
    std::string toString() const;
};
struct Seq {
    Connector connector; NodePtr lhs, rhs; // lhs CONNECTOR rhs
    std::string toString() const;
};
struct Alt {
    std::vector<NodePtr> branches; // a | b | ...
    std::string toString() const;
};
struct Repeat {
    Quantifier quant; NodePtr inner; // inner with a quantifier
    std::string toString() const;
};
// A node in the query tree: one path operator (the active alternative) plus any
// predicates attached to it. Predicates are independent of the kind — they may
// constrain a step or the node a group resolves to, e.g. `(a | b)[x>5]`.
using NodeVariant = std::variant<Step, Seq, Alt, Repeat>;
struct Node {
    NodeVariant value;
    std::vector<Predicate> preds;          // ANDed
    std::string toString() const;
};

// --------------------------
// Constructors for the AST: fetch_node wraps a node value in an owning pointer,
// and each named factory builds one kind from its payload.
// --------------------------
inline NodePtr fetch_node(NodeVariant value) {
    auto n = std::make_unique<Node>();
    n->value = std::move(value);
    return n;
}
inline NodePtr step(std::string type) {
    return fetch_node(Step{std::move(type), false});
}
inline NodePtr wildcard() {
    return fetch_node(Step{"", true});
}
inline NodePtr seq(Connector connector, NodePtr lhs, NodePtr rhs) {
    return fetch_node(Seq{connector, std::move(lhs), std::move(rhs)});
}
inline NodePtr alt(std::vector<NodePtr> branches) {
    return fetch_node(Alt{std::move(branches)});
}
inline NodePtr repeat(Quantifier quant, NodePtr inner) {
    return fetch_node(Repeat{quant, std::move(inner)});
}

inline Predicate compare(std::string field, Comparator op, std::string value) {
    return Predicate{Compare{std::move(field), op, std::move(value)}};
}
inline Predicate exists(NodePtr sub) {
    return Predicate{Exists{std::move(sub)}};
}
inline Predicate negated(Predicate inner) {
    return Predicate{Not{std::make_unique<Predicate>(std::move(inner))}};
}

// --------------------------
// A parsed query: its root node and whether it is anchored at the document root.
// --------------------------
struct Query {
    NodePtr root;
    bool anchored = false;
};
// Recursive-descent parser over the query text: one method per grammar
// production. Throws std::runtime_error (via Cursor::fail) on a syntax error.
class QueryParser {
public:
    explicit QueryParser(std::string_view src) : cursor{src} {}

    Query parse() {
        cursor.skip_whitespace();
        Query q;
        if (cursor.match("//")) {
          // leading '//' is allowed but means the floating default
        } else if (cursor.match("/")) {
          q.anchored = true;  // leading '/' anchors at the root
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

    // alt := seq ('|' seq)*
    NodePtr parse_alternation() {
        NodePtr left = parse_sequence();

        cursor.skip_whitespace();
        if (cursor.peek() != '|') return left;

        std::vector<NodePtr> branches;
        branches.push_back(std::move(left));
        while (cursor.skip_whitespace(), cursor.match("|"))
            branches.push_back(parse_sequence());
        return alt(std::move(branches));
    }

    // seq := quant (CONNECTOR quant)*, left-associative.
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

    // quant := atom ('%' | '?' | '+')*
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

    // atom := ('(' alt ')' | step) pred*. Predicates bind to the atom; on a group
    // they constrain the node it resolves to, e.g. `(positionref => position)[x>500]`.
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

    // The body inside [...]: '!' pred | IDENT OP value | subpath. An identifier
    // with no comparison operator after it is a subpath existence test, so the
    // cursor rewinds and the identifier is parsed as a step path instead.
    Predicate parse_predicate() {
        cursor.skip_whitespace();
        if (cursor.match("!")) return negated(parse_predicate());

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

    // An identifier. Precondition: the cursor is at an identifier start (alpha or
    // '_'); callers guard, so the result is never empty.
    std::string parse_identifier() {
        std::size_t start = cursor.pos;
        ++cursor.pos;  // first char, guaranteed by the precondition
        while (std::isalnum((unsigned char)cursor.peek()) || cursor.peek() == '_')
          ++cursor.pos;
        return std::string(cursor.src.substr(start, cursor.pos - start));
    }

    // A comparison value: everything up to the closing ']' (so it may contain
    // '*', '/', units, ...), with trailing whitespace trimmed.
    std::string parse_value() {
        cursor.skip_whitespace();

        std::size_t start = cursor.pos;
        while (!cursor.eof() && cursor.peek() != ']' && cursor.peek() != '[')
          ++cursor.pos;

        std::size_t end = cursor.pos;
        while (end > start && std::isspace((unsigned char)cursor.src[end-1]))
          --end;  // rtrim
        return std::string(cursor.src.substr(start, end - start));
    }

    // A '/'-delimited regex literal; backslash escapes (including \/) are copied
    // verbatim and handed to std::regex unchanged.
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

// --------------------------
// Render a node/predicate back to query syntax (for diagnostics and round-trip
// tests). Each alternative renders itself; Node and Predicate dispatch over their
// variant. The definitions are out-of-line because the recursive alternatives
// reach back through NodePtr/PredicatePtr, which need the wrapper types complete.
// --------------------------
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

inline std::string Not::toString() const { return "[!" + neg->toString().substr(1); }  // splice '!' into the inner [...]

inline std::string Node::toString() const {
    std::string s = std::visit([](const auto &a) { return a.toString(); }, value);
    if (!preds.empty() && std::holds_alternative<Seq>(value)) s = "(" + s + ")";  // parenthesize so preds bind to the whole seq
    for (const auto &p : preds) s += p.toString();
    return s;
}

inline std::string Predicate::toString() const {
    return std::visit([](const auto &a) { return a.toString(); }, value);
}

}  // namespace gg
