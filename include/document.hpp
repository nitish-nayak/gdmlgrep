#pragma once
// A parsed GDML document: owns the source text and the tree-sitter tree, and
// answers node-level questions (type, source text, line) against them. The
// source string outlives the tree, so node text is returned as a view into it.
#include "ts.hpp"

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
        : source_(path == "-" ? readStream(std::cin) : readFile(path)),
          name_(path == "-" ? "<stdin>" : path) {
        parse();
    }

    TSNode root() const { return ts_tree_root_node(tree_.get()); }
    const std::string &name()   const { return name_; }
    const std::string &source() const { return source_; }

    // The source text a node spans (a view into source_, no copy).
    std::string_view text(TSNode n) const {
        uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
        return std::string_view(source_).substr(a, b - a);
    }

    // 1-based line of a node's start, for grep-style output.
    uint32_t line(TSNode n) const { return ts_node_start_point(n).row + 1; }

private:
    std::string source_;
    std::string name_;
    ParserPtr parser_;
    TreePtr   tree_;

    void parse() {
        parser_.reset(ts_parser_new());
        if (!ts_parser_set_language(parser_.get(), tree_sitter_gdml()))
            throw std::runtime_error(
                "tree-sitter grammar ABI incompatible with the linked runtime");
        tree_.reset(ts_parser_parse_string(
            parser_.get(), nullptr, source_.c_str(),
            static_cast<uint32_t>(source_.size())));
        if (!tree_) throw std::runtime_error("failed to parse " + name_);
    }

    static std::string readFile(const std::string &path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + path);
        return readStream(f);
    }
    static std::string readStream(std::istream &in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

}  // namespace gg
