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
#include "query/ts.hpp"
#include "document.hpp"
#include "utils.hpp"

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
    explicit Evaluator(const Document &doc) : doc_(doc) {}

    std::optional<double> eval(TSNode n) {
        if (!built_) { built_ = true; collectConstants(doc_.root()); }
        return evalNode(n);
    }

private:
    const Document &doc_;
    std::map<std::string, TSNode> constExpr_;                   // <constant>/<quantity> name -> value expr
    std::map<std::string, std::optional<double>> cache_;
    std::set<std::string> inProgress_;                          // cycle guard
    bool built_ = false;

    // <variable> is deliberately not collected -> references to it are unevaluable.
    void collectConstants(TSNode n) {
        TSSymbol s = ts_node_symbol(n);
        if (s == kCONSTANT || s == kQUANTITY) {
            TSNode name = ts_node_child_by_field_name(n, "name", 4);
            TSNode expr = valueExprNode(doc_, n, "value");
            if (!ts_node_is_null(name) && !ts_node_is_null(expr))
                constExpr_.emplace(std::string(doc_.text(name, true)), expr);
        }
        uint32_t c = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < c; ++i) collectConstants(ts_node_named_child(n, i));
    }

    std::optional<double> evalNode(TSNode n) {
        if (ts_node_is_null(n)) return std::nullopt;
        TSSymbol s = ts_node_symbol(n);
        if (s == kNUMBER) return std::strtod(std::string(doc_.text(n)).c_str(), nullptr);
        if (s == kIDENTIFIER) return resolveIdent(std::string(doc_.text(n)));
        if (s == kPAREN)
            return ts_node_named_child_count(n) > 0 ? evalNode(ts_node_named_child(n, 0)) : std::nullopt;
        if (s == kUNARY) {
            auto v = evalNode(ts_node_named_child(n, 0));
            if (!v) return std::nullopt;
            TSNode op = ts_node_child_by_field_name(n, "op", 2);
            return (!ts_node_is_null(op) && doc_.text(op) == "-") ? -*v : *v;
        }
        if (s == kBINARY) {
            if (ts_node_named_child_count(n) < 2) return std::nullopt;
            auto a = evalNode(ts_node_named_child(n, 0));
            auto b = evalNode(ts_node_named_child(n, 1));
            if (!a || !b) return std::nullopt;
            TSNode op = ts_node_child_by_field_name(n, "op", 2);
            std::string o = ts_node_is_null(op) ? "" : std::string(doc_.text(op));
            if (o == "+") return *a + *b;
            if (o == "-") return *a - *b;
            if (o == "*") return *a * *b;
            if (o == "/") return *b != 0 ? std::optional<double>(*a / *b) : std::nullopt;
            if (o == "^") return std::pow(*a, *b);
            return std::nullopt;
        }
        if (s == kCALL) return evalCall(n);
        return std::nullopt;
    }

    std::optional<double> resolveIdent(const std::string &name) {
        if (name == "pi") return kPi;
        auto ci = cache_.find(name);
        if (ci != cache_.end()) return ci->second;
        if (inProgress_.count(name)) return std::nullopt;  // reference cycle
        auto ei = constExpr_.find(name);
        if (ei == constExpr_.end()) return cache_[name] = std::nullopt;  // unknown / unit / variable
        inProgress_.insert(name);
        std::optional<double> v = evalNode(ei->second);
        inProgress_.erase(name);
        return cache_[name] = v;
    }

    std::optional<double> evalCall(TSNode n) {
        uint32_t c = ts_node_named_child_count(n);
        if (c == 0) return std::nullopt;
        std::string fn = std::string(doc_.text(ts_node_named_child(n, 0)));
        std::vector<double> a;
        for (uint32_t i = 1; i < c; ++i) {
            auto v = evalNode(ts_node_named_child(n, i));
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
