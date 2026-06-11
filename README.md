# gdmlgrep (`gg`)

A grep/search tool for [GDML](https://gdml.web.cern.ch/GDML/) geometry files,
built on a custom-build tree-sitter [grammar](https://github.com/nitish-nayak/tree-sitter-gdml) for GDML.

It walks the parsed syntax tree from the GDML input and builds a reference graph that you can query structurally, i.e
by element type, attributes, parent/child paths, and references.
The query syntax is quite expressive (See [Usage](#usage)), with its own AST implementation.
As a result, one can probe GDML files quite powerfully, similar to `jq`, `jsongrep` (for JSON).
This is done through a structural knowledge of how geometries are described within the GDML, provided by our tree-sitter grammar.

In addition, the search algorithm was heavily inspired by [jsongrep](https://github.com/micahkepe/jsongrep), which is blazing fast!
See Micah's blog [post](https://micahkepe.com/blog/jsongrep/) for more details.

## Build

```sh
git submodule update --init    # fetch the tree-sitter runtime + GDML grammar
cmake -B build
cmake --build build
```

## Usage

```sh
gg [flags] '<query>' <file.gdml>
```

Run a query against a GDML file (or `-` for stdin).
- Uses bundled `simple.gdml` (`tree-sitter/tree-sitter-gdml/gdml/simple.gdml`)
```sh
# every volume
$ gg volume simple.gdml
simple.gdml:92: <volume name="v1">
simple.gdml:96: <volume name="v2">
simple.gdml:100: <volume name="World">

# count physvols at any depth
$ gg -c '// physvol' simple.gdml
2

# emit one field instead of the whole line
$ gg -o name volume simple.gdml
v1
v2
World

# follow references: the volumes each physvol places
$ gg 'physvol => volume' simple.gdml
simple.gdml:92: <volume name="v1">
simple.gdml:96: <volume name="v2">

# numeric attribute predicate
$ gg 'box[x>1000]' simple.gdml
simple.gdml:62: <box name="WorldBox" x="10000.0" y="10000.0" z="10000.0"/>

# regex on an attribute
$ gg 'material[name=~/^A/]' simple.gdml
simple.gdml:41: <material name="Al" Z="13.0">
simple.gdml:50: <material name="Air">

# negated existence: volumes with no physvol child
$ gg 'volume[!physvol]' simple.gdml
simple.gdml:92: <volume name="v1">
simple.gdml:96: <volume name="v2">

# alternation, count only
$ gg -c 'box | tube' simple.gdml
6

# group + `%` (zero or more): a volume and everything it places, transitively
$ gg 'volume / (physvol => volume)%' simple.gdml
simple.gdml:92: <volume name="v1">
simple.gdml:96: <volume name="v2">
simple.gdml:100: <volume name="World">

# `+` (one or more) instead — excludes the starting volume (one hop minimum)
$ gg -c '(physvol => volume)+' simple.gdml
2
```

### Flags

| Flag | Effect |
| --- | --- |
| `-e <query>` | add a query (repeatable); the file is parsed once, each result block headed `==> q <==` |
| `-c` | print the match count only |
| `-o <field>` | emit a field (`name` / `ref` / `type` / any attribute) instead of the line |
| `-q` | quiet: no output, exit status only |
| `--pretty` | ANSI-colorized output |
| `-h`, `--help` | show help |

## Query Syntax

A query is a path of steps joined by connectors, with optional
predicates and quantifiers. A step is an element type (`volume`, `box`, ...)
or `*` (any element).

### Connectors

| Syntax | Meaning | Example |
| --- | --- | --- |
| `A / B` | `B` is a direct child of `A` | `volume / solidref` |
| `A // B` | `B` is descendant of `A` at any depth | `structure // physvol` |
| `A => B` | follow `A`'s reference (`volumeref`, `solidref`, …) to the `B` it names | `physvol => volume` |
| `A ==> B` | follow references at any depth | `world ==> volume` |

`//` and `==>` are basically Kleene-star equivalents of `/` and `=>` respectively

### Predicates

Written in `[ ]` after a step; multiple predicates are ANDed.

| Syntax | Meaning | Example |
| --- | --- | --- |
| `[field = value]` | attribute equals (textual or numeric) | `box[x=500]` |
| `[field != value]` | not equal | `material[name!=Air]` |
| `[field < v]` `<=` `>` `>=` | numeric comparison | `box[x>1000]` |
| `[field =~/regex/]` | attribute matches a regex | `material[name=~/^A/]` |
| `[subpath]` | the step has a matching child/path (existence) | `volume[solidref]` |
| `[!pred]` | negation | `volume[!physvol]` |

A predicate that can't be evaluated (e.g. a numeric compare against an
expression that doesn't reduce to a number) is excluded from the results and
reported on stderr.

### Quantifiers and grouping

| Syntax | Meaning |
| --- | --- |
| `step%` | zero or more |
| `step+` | one or more |
| `step?` | optional |
| `( … )` | group, e.g. `(physvol => volume)%` |
| `a \| b` | alternation, e.g. `box \| tube` |

## Verbs

Some questions need the full gdml reference graph or non-flat output, so they're
subcommands rather than queries:

```sh
# geometry placement hierarchy from <world> (optionally from a named volume)
# equivalent to `tree` but for the placement structure; --pretty draws connectors
$ gg --pretty placement-tree simple.gdml
World
├── v2
└── v1

# sites that reference a name
$ gg find-usages Al simple.gdml
simple.gdml:93: <materialref ref="Al"/>
simple.gdml:97: <materialref ref="Al"/>

# names defined but never referenced
$ gg dead-defs simple.gdml
simple.gdml:10: <position name="shiftbyx" x="20.0"/>
...

# names referenced but never defined
$ gg dangling simple.gdml
```
