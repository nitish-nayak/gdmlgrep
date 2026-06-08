# gdmlgrep (`gg`)

A grep/search tool for [GDML](https://gdml.web.cern.ch/GDML/) geometry files,
built on a tree-sitter grammar. It walks the parse tree (and the
reference graph) so you can query structurally — by element type, attributes,
parent/child paths, and references — instead of by raw text.

Status: **scaffold** — parses a file and dumps the node tree. The query engine
(path/predicate matching, the reference-graph axes, verbs) lands in later
commits.

## Build

```sh
cmake -B build
cmake --build build
```

Produces `build/gg`. No network needed: the tree-sitter runtime and the GDML
grammar are vendored under `vendor/`.

## Usage (scaffold)

```sh
./build/gg path/to/file.gdml     # dump the named-node tree
cat file.gdml | ./build/gg -     # read from stdin
```

## Layout

- `vendor/tree-sitter/` — tree-sitter C runtime (v0.25.6)
- `vendor/grammar/` — the generated GDML grammar (`parser.c`, `scanner.c`)
- `include/` — header-only engine: `Document`, and (later) the query parser,
  name index, and query engine
- `src/main.cpp` — the CLI
