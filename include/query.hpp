#pragma once
// Query.hpp — the query language: AST + a recursive-descent parser.
//
// A query is a property-path expression (à la SPARQL property paths): steps are
// operands, axes are the edges between them, with grouping/alternation/repeat.
//
//   query   := ['/' | '//'] alt          // optional leading anchor; default floating
//   alt     := seq ('|' seq)*            // alternation (lowest precedence)
//   seq     := postfix (AXIS postfix)*   // AXIS = / // => ==>
//   postfix := atom ('%' | '?' | '+')*   // quantifiers (Kleene star is '%')
//   atom    := '(' alt ')' | step
//   step    := (IDENT | '*') pred*       // '*' = any-node wildcard
//   pred    := '[' ('!' pred | IDENT OP value | subpath) ']'
//   OP      := '=' | '=~' | '!=' | '<' | '<=' | '>' | '>='
//
// Lexing is parser-driven (scannerless) because a few tokens are context
// sensitive: '/' is the child axis between steps but a regex delimiter inside
// [...] after '=~', and a predicate value runs arbitrarily up to ']'.
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gg {

enum class Axis  { Child, Descendant, Deref, DerefClosure };  //  /   //   =>   ==>
enum class Quant { Star, Plus, Opt };                         //  %   +    ?
enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge, Regex };           //  =  != < <= > >=  =~

struct Node;
using NodePtr = std::unique_ptr<Node>;

// A predicate is one [...] test on a step. Multiple brackets on a step AND.
struct Predicate {
    enum class Kind { Compare, Exists, Not } kind;
    std::string field;                  // Compare: the field/attr name
    CmpOp       op{};                   // Compare: the operator
    std::string value;                  // Compare: literal text (or regex source)
    NodePtr     sub;                    // Exists:  the relative sub-path
    std::unique_ptr<Predicate> neg;     // Not:     the negated predicate
};

// One AST node. A tagged sum type: Step is a leaf; Seq/Alt/Repeat are interior.
struct Node {
    enum class Kind { Step, Seq, Alt, Repeat } kind;
    std::string type;                   // Step: node-type name ("" if wildcard)
    bool        wildcard = false;       // Step: matches any node type
    std::vector<Predicate> preds;       // Step: predicates (ANDed)
    Axis        axis{};                 // Seq:  edge connecting kids[0] -> kids[1]
    Quant       quant{};                // Repeat: the quantifier
    std::vector<NodePtr> kids;          // Seq/Alt: children; Repeat: [inner]
};

struct Query {
    NodePtr root;
    bool anchored = false;              // leading '/' (root anchor); else floating
};

struct ParseError : std::runtime_error {
    size_t pos;
    ParseError(const std::string &msg, size_t p)
        : std::runtime_error(msg + " (at column " + std::to_string(p + 1) + ")"),
          pos(p) {}
};

class QueryParser {
public:
    explicit QueryParser(std::string_view src) : src_(src), n_(src.size()) {}

    Query parse() {
        skipWs();
        Query q;
        if (match("//")) {              // explicit floating (same as default)
        } else if (peek() == '/') {     // leading '/' = root anchor
            ++pos_;
            q.anchored = true;
        }
        q.root = parseAlt();
        skipWs();
        if (pos_ != n_) throw ParseError("unexpected trailing input", pos_);
        return q;
    }

    static Query parseString(std::string_view src) { return QueryParser(src).parse(); }

private:
    std::string_view src_;
    size_t n_;
    size_t pos_ = 0;

    // --- character primitives ---
    char peek() const { return pos_ < n_ ? src_[pos_] : '\0'; }
    bool match(std::string_view lit) {
        if (src_.substr(pos_, lit.size()) == lit) { pos_ += lit.size(); return true; }
        return false;
    }
    void skipWs() { while (pos_ < n_ && std::isspace((unsigned char)src_[pos_])) ++pos_; }
    void expect(char c) {
        if (peek() != c) throw ParseError(std::string("expected '") + c + "'", pos_);
        ++pos_;
    }

    // --- productions ---
    NodePtr parseAlt() {
        NodePtr left = parseSeq();
        skipWs();
        if (peek() != '|') return left;
        auto alt = std::make_unique<Node>();
        alt->kind = Node::Kind::Alt;
        alt->kids.push_back(std::move(left));
        while (skipWs(), peek() == '|') { ++pos_; alt->kids.push_back(parseSeq()); }
        return alt;
    }

    NodePtr parseSeq() {
        NodePtr left = parsePostfix();
        for (Axis ax; tryAxis(ax);) {
            auto seq = std::make_unique<Node>();
            seq->kind = Node::Kind::Seq;
            seq->axis = ax;
            seq->kids.push_back(std::move(left));
            seq->kids.push_back(parsePostfix());
            left = std::move(seq);
        }
        return left;
    }

    NodePtr parsePostfix() {
        NodePtr a = parseAtom();
        for (;;) {
            skipWs();
            char c = peek();
            Quant q;
            if (c == '%') q = Quant::Star;
            else if (c == '+') q = Quant::Plus;
            else if (c == '?') q = Quant::Opt;
            else break;
            ++pos_;
            auto rep = std::make_unique<Node>();
            rep->kind = Node::Kind::Repeat;
            rep->quant = q;
            rep->kids.push_back(std::move(a));
            a = std::move(rep);
        }
        return a;
    }

    // An atom is a step or a parenthesized group, each optionally carrying
    // predicates — a predicate on a group constrains the node the group
    // resolves to (e.g. `(position | positionref => position)[x>500]`).
    NodePtr parseAtom() {
        NodePtr a = parseBase();
        for (;;) { skipWs(); if (peek() != '[') break; a->preds.push_back(parsePredicate()); }
        return a;
    }

    NodePtr parseBase() {
        skipWs();
        if (peek() == '(') { ++pos_; NodePtr inner = parseAlt(); skipWs(); expect(')'); return inner; }
        auto step = std::make_unique<Node>();
        step->kind = Node::Kind::Step;
        if (peek() == '*') { ++pos_; step->wildcard = true; }
        else step->type = parseIdent("expected a node type or '*'");
        return step;
    }

    Predicate parsePredicate() {
        expect('[');
        Predicate p = parsePredBody();
        skipWs(); expect(']');
        return p;
    }

    Predicate parsePredBody() {
        skipWs();
        if (peek() == '!') {
            ++pos_;
            Predicate p;
            p.kind = Predicate::Kind::Not;
            p.neg = std::make_unique<Predicate>(parsePredBody());
            return p;
        }
        // Try "field OP value"; if no operator follows the identifier, rewind
        // and parse the bracket as a sub-path existence test.
        size_t save = pos_;
        std::string ident = tryIdent();
        if (!ident.empty()) {
            skipWs();
            if (CmpOp op; tryCmpOp(op)) {
                Predicate p;
                p.kind = Predicate::Kind::Compare;
                p.field = ident;
                p.op = op;
                p.value = (op == CmpOp::Regex) ? parseRegex() : parseValue();
                return p;
            }
        }
        pos_ = save;
        Predicate p;
        p.kind = Predicate::Kind::Exists;
        p.sub = parseAlt();
        return p;
    }

    // --- token helpers ---
    bool tryAxis(Axis &out) {
        skipWs();
        if (match("==>")) { out = Axis::DerefClosure; return true; }
        if (match("=>"))  { out = Axis::Deref;        return true; }
        if (match("//"))  { out = Axis::Descendant;   return true; }
        if (peek() == '/') { ++pos_; out = Axis::Child; return true; }
        return false;
    }

    bool tryCmpOp(CmpOp &out) {
        if (match("=~")) { out = CmpOp::Regex; return true; }
        if (match("!=")) { out = CmpOp::Ne;    return true; }
        if (match(">=")) { out = CmpOp::Ge;    return true; }
        if (match("<=")) { out = CmpOp::Le;    return true; }
        if (match("="))  { out = CmpOp::Eq;    return true; }
        if (match(">"))  { out = CmpOp::Gt;    return true; }
        if (match("<"))  { out = CmpOp::Lt;    return true; }
        return false;
    }

    std::string tryIdent() {
        skipWs();
        size_t start = pos_;
        if (pos_ < n_ && (std::isalpha((unsigned char)src_[pos_]) || src_[pos_] == '_')) {
            ++pos_;
            while (pos_ < n_ && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_')) ++pos_;
            return std::string(src_.substr(start, pos_ - start));
        }
        return {};
    }

    std::string parseIdent(const char *err) {
        std::string s = tryIdent();
        if (s.empty()) throw ParseError(err, pos_);
        return s;
    }

    // A comparison value runs up to ']' (so it can hold '*', '/', units, etc.).
    std::string parseValue() {
        skipWs();
        size_t start = pos_;
        while (pos_ < n_ && src_[pos_] != ']' && src_[pos_] != '[') ++pos_;
        size_t end = pos_;
        while (end > start && std::isspace((unsigned char)src_[end - 1])) --end;  // rtrim
        return std::string(src_.substr(start, end - start));
    }

    // A '/'-delimited regex literal; backslash escapes are preserved verbatim.
    std::string parseRegex() {
        skipWs();
        expect('/');
        std::string re;
        while (pos_ < n_ && src_[pos_] != '/') {
            if (src_[pos_] == '\\' && pos_ + 1 < n_) re += src_[pos_++];
            re += src_[pos_++];
        }
        expect('/');
        return re;
    }
};

// Render an AST back to a normalized query string (for diagnostics/tests).
inline std::string toString(const Node &n);

inline std::string toString(const Predicate &p) {
    switch (p.kind) {
        case Predicate::Kind::Compare: {
            const char *op =
                p.op == CmpOp::Eq ? "=" : p.op == CmpOp::Ne ? "!=" :
                p.op == CmpOp::Lt ? "<" : p.op == CmpOp::Le ? "<=" :
                p.op == CmpOp::Gt ? ">" : p.op == CmpOp::Ge ? ">=" : "=~";
            if (p.op == CmpOp::Regex) return "[" + p.field + "=~/" + p.value + "/]";
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
            const char *ax =
                n.axis == Axis::Child ? " / " : n.axis == Axis::Descendant ? " // " :
                n.axis == Axis::Deref ? " => " : " ==> ";
            base = toString(*n.kids[0]) + ax + toString(*n.kids[1]);
            if (!n.preds.empty()) base = "(" + base + ")";  // bind preds to the whole seq
            break;
        }
        case Node::Kind::Alt: {
            base = "(";
            for (size_t i = 0; i < n.kids.size(); ++i) base += (i ? " | " : "") + toString(*n.kids[i]);
            base += ")";
            break;
        }
        case Node::Kind::Repeat: {
            const char *q = n.quant == Quant::Star ? "%" : n.quant == Quant::Plus ? "+" : "?";
            base = "(" + toString(*n.kids[0]) + ")" + q;
            break;
        }
    }
    for (const auto &p : n.preds) base += toString(p);
    return base;
}

}  // namespace gg
