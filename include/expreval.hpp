#pragma once
// expreval.hpp — easy-T3 expression evaluation. Reduces a GDML value expression
// (number / identifier / unary / binary / parenthesized / call, as already
// parsed by the grammar) to a double, resolving <constant>/<quantity>
// references and `pi`, plus the common <cmath> functions.
//
// eval() returns nullopt when the expression cannot be reduced to a number: an
// undefined identifier, a unit token (V, cm, ...), a <variable> (loop-mutable,
// deliberately out of scope), an unknown function, or a reference cycle. The
// caller treats nullopt as "unevaluable" and surfaces a diagnostic rather than
// silently failing the match. Unit resolution and <variable>/<loop> evaluation
// are intentionally not done here.
#include "query/grammar.hpp"
#include "document.hpp"
#include "utils/nodeattr.hpp"

#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gg {

constexpr double kPi = 3.14159265358979323846;  // CLHEP pi

class Evaluator {
public:
    explicit Evaluator(const Document &doc) : doc(doc) {}

    // Reduce expression node n to a number, or nullopt if it can't be (see header).
    std::optional<double> eval(TSNode n) {
        // Index the document's constants on first use — most queries never get here.
        if (!built) { built = true; eval_constants(doc.root()); }
        return eval_node(n);
    }

private:
    const Document &doc;
    std::map<std::string, TSNode> const_expr;                   // <constant>/<quantity> name -> value expr
    std::map<std::string, std::optional<double>> cache;
    std::set<std::string> in_progress;                          // cycle guard
    bool built = false;

    // Record every <constant>/<quantity> as name -> its value expression, so
    // eval_ident can resolve references. <variable> is deliberately skipped: it is
    // loop-mutable, so references to it must stay unevaluable.
    void eval_constants(TSNode n) {
        TSSymbol s = ts_node_symbol(n);

        if (s == kCONSTANT || s == kQUANTITY) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            TSNode expr = valueExprNode(doc, n, "value");
            if (!ts_node_is_null(name) && !ts_node_is_null(expr))
                const_expr.emplace(std::string(doc.text(name, true)), expr);
        }

        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i)
            eval_constants(ts_node_named_child(n, i));
    }

    // Evaluate one expression node by kind, recursing into operands; nullopt
    // propagates up the moment any sub-expression is unevaluable.
    std::optional<double> eval_node(TSNode n) {
        if (ts_node_is_null(n)) return std::nullopt;
        TSSymbol s = ts_node_symbol(n);

        if (s == kNUMBER)
            return std::strtod(std::string(doc.text(n)).c_str(), nullptr);
        if (s == kIDENTIFIER)
            return eval_ident(std::string(doc.text(n)));
        if (s == kPAREN)
            return ts_node_named_child_count(n) > 0 ? eval_node(ts_node_named_child(n, 0)) : std::nullopt;
        if (s == kUNARY) {
            auto v = eval_node(ts_node_named_child(n, 0));
            if (!v) return std::nullopt;

            TSNode op = ts_node_child_by_field_name(n, "op", 2);
            return (!ts_node_is_null(op) && doc.text(op) == "-") ? -*v : *v;
        }
        if (s == kBINARY) {
            if (ts_node_named_child_count(n) < 2) return std::nullopt;

            auto a = eval_node(ts_node_named_child(n, 0));
            auto b = eval_node(ts_node_named_child(n, 1));
            if (!a || !b) return std::nullopt;

            TSNode op = ts_node_child_by_field_name(n, "op", 2);
            std::string o = ts_node_is_null(op) ? "" : std::string(doc.text(op));
            if (o == "+") return *a + *b;
            if (o == "-") return *a - *b;
            if (o == "*") return *a * *b;
            if (o == "/") return *b != 0 ? std::optional<double>(*a / *b) : std::nullopt;  // 0 divisor -> unevaluable
            if (o == "^") return std::pow(*a, *b);
            return std::nullopt;
        }
        if (s == kCALL) return eval_call(n);
        return std::nullopt;
    }

    // Resolve an identifier to a number: the builtin `pi`, or a <constant>/<quantity>
    // value (memoized, failures included). in_progress breaks reference cycles — a
    // constant that refers, directly or transitively, back to itself.
    std::optional<double> eval_ident(const std::string &name) {
        if (name == "pi") return kPi;

        auto ci = cache.find(name);
        if (ci != cache.end()) return ci->second;

        if (in_progress.count(name)) return std::nullopt;

        auto ei = const_expr.find(name);
        if (ei == const_expr.end()) return cache[name] = std::nullopt;  // unknown / unit / variable

        in_progress.insert(name);
        std::optional<double> v = eval_node(ei->second);
        in_progress.erase(name);
        return cache[name] = v;
    }

    // Evaluate a function call: child 0 is the function name, the rest its
    // already-reduced arguments. Only the <cmath> builtins below are supported.
    std::optional<double> eval_call(TSNode n) {
        uint32_t c = ts_node_named_child_count(n);
        if (c == 0) return std::nullopt;

        std::string fn = std::string(doc.text(ts_node_named_child(n, 0)));
        std::vector<double> a;
        for (uint32_t i = 1; i < c; ++i) {
            auto v = eval_node(ts_node_named_child(n, i));
            if (!v) return std::nullopt;
            a.push_back(*v);
        }

        if (a.size() == 1) {
            double x = a[0];
            if (fn == "sin") return std::sin(x);
            if (fn == "cos") return std::cos(x);
            if (fn == "tan") return std::tan(x);
            if (fn == "asin") return std::asin(x);
            if (fn == "acos") return std::acos(x);
            if (fn == "atan") return std::atan(x);
            if (fn == "exp") return std::exp(x);
            if (fn == "log") return std::log(x);
            if (fn == "log10") return std::log10(x);
            if (fn == "sqrt") return std::sqrt(x);
            if (fn == "abs" || fn == "fabs") return std::fabs(x);
            if (fn == "floor") return std::floor(x);
            if (fn == "ceil") return std::ceil(x);
        } else if (a.size() == 2) {
            if (fn == "pow") return std::pow(a[0], a[1]);
            if (fn == "atan2") return std::atan2(a[0], a[1]);
            if (fn == "min") return a[0] < a[1] ? a[0] : a[1];
            if (fn == "max") return a[0] > a[1] ? a[0] : a[1];
            if (fn == "fmod") return std::fmod(a[0], a[1]);
        }
        return std::nullopt;
    }
};

}  // namespace gg
