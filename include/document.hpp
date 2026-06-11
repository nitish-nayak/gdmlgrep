#pragma once
// A parsed GDML document: owns the source text and the tree-sitter tree, and
// answers node-level questions (source text, line) against them. The source
// string outlives the tree, so node text is returned as a view into it.
#include "query/grammar.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gg {

class Document {
public:
    // Parse a file by path, or stdin when path == "-".
    explicit Document(const std::string &path)
        : source(read_source(path)), name(path == "-" ? "<stdin>" : path) {
        // The parser is only scaffolding for building the tree; the tree owns its
        // data afterward, so the parser stays local and is freed here.
        ParserPtr parser(ts_parser_new());
        if (!ts_parser_set_language(parser.get(), tree_sitter_gdml()))
            throw std::runtime_error("tree-sitter grammar ABI incompatible with the linked runtime");
        tree.reset(ts_parser_parse_string(parser.get(), nullptr,
                                           source.c_str(), static_cast<uint32_t>(source.size())));
        if (!tree) throw std::runtime_error("failed to parse " + name);
    }

    TSNode root() const { return ts_tree_root_node(tree.get()); }
    const std::string &get_name() const { return name; }

    // The source text a node spans (a view into `source`, no copy).
    std::string_view text(TSNode n, bool strip_quotes = false) const {
        uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
        auto t = std::string_view(source).substr(a, b - a);
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && strip_quotes)
            return t.substr(1, t.size() - 2);
        return t;
    }

    // 1-based line of a node's start, for grep-style output.
    uint32_t line(TSNode n) const { return ts_node_start_point(n).row + 1; }

private:
    std::string source;
    std::string name;
    TreePtr     tree;

    // Slurp the whole file — or stdin, for path "-" — into a string.
    static std::string read_source(const std::string &path) {
        std::ostringstream ss;
        if (path == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream f(path, std::ios::binary);
            if (!f) throw std::runtime_error("cannot open " + path);
            ss << f.rdbuf();
        }
        return ss.str();
    }
};

}  // namespace gg
