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

## Getting Started

gg ships as a single Actually-Portable-Executable (APE) which runs
natively on Linux, macOS, and Windows, on both x86-64 and ARM.
No installation needed, no dependencies.
```sh
curl -L -o gg https://github.com/nitish-nayak/gdmlgrep/releases/download/v0.1.0/gg
chmod +x gg
./gg --help
```
This is enabled by the amazing [cosmocc](https://github.com/jart/cosmopolitan) compiler (See [Build](#build)).

## Build

If you'd prefer a local build, this is also relatively straightforward using standard `make` or `cmake`.
- Default `make` however, builds the cosmocc APE.
- `cosmocc` builds a slightly beefier executable (`2.7 MB` -> `6.9 MB`) since it ships its own `libc` runtime as well as embedding `x86-64` and `aarch64` directly, but can be used everywhere (`ARM`, `x86_64`) with the same binary!
- Use `make NATIVE=1` instead for a standard `gcc` build.

```sh
git submodule update --init

# Build through either of the below options
# CMake — native build + tests
cmake -B build && cmake --build build

# Makefile — portable APE (needs cosmocc/cosmoc++ on PATH)
make
# …or a native binary using standard GCC:
make NATIVE=1
```

## Usage

```sh
./gg [flags] '<query>' <file.gdml>
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


Run a query against a GDML file (or `-` for stdin).

The following section showcases some examples against a real geometry - the LHCb VELO from Keith Sloan's GDML
sample set (3019 lines, 59 volumes, 319 placements, 48 boolean solids).

```sh
VELO=https://raw.githubusercontent.com/KeithSloan/GDML/Main/SampleFiles/CERN/lhcbvelo.gdml
curl -fsSL "$VELO" | ./gg '<query>' -
```

Count solids by type in a single parse. Each `-e` adds a query, `-c` prints
counts instead of matching lines, and `A | B` matches either type:

```sh
curl -fsSL "$VELO" | ./gg -c -e box -e tube -e trap -e polycone -e 'union | subtraction' -
```

```
box:53
tube:31
trap:18
polycone:10
union | subtraction:48
```

List every boolean solid:

```sh
curl -fsSL "$VELO" | ./gg 'union | subtraction' -
```

Find booleans that contain a nested boolean. The `[ ]` keeps only those whose
operand (`first` or `second`) references another boolean, and `=>` follows that
reference to the solid it names:

```sh
curl -fsSL "$VELO" | ./gg '(union | subtraction)[(first | second) => (union | subtraction)]' -
```

```
lhcbvelo.gdml:721: <union name="PuStationUnion">
lhcbvelo.gdml:735: <union name="PUdetectorRUnion">
...                                          (19 results)
```

To list the inner operands instead, follow the references from the other side:

```sh
curl -fsSL "$VELO" | ./gg -c '(first | second) => (union | subtraction)' -      # 19
curl -fsSL "$VELO" | ./gg -o name '(first | second) => (union | subtraction)' -
```

Find volumes whose solid is a boolean. `volume / solidref` steps from each
volume into its `solidref` child, and `=>` follows that reference to the solid:

```sh
curl -fsSL "$VELO" | ./gg -c 'volume / solidref => (union | subtraction)' -     # 29
```

Or, since only a `solidref` can point at a boolean (a `materialref` cannot), let
`*` stand for any child and skip naming it:

```sh
curl -fsSL "$VELO" | ./gg -c 'volume[* => (union | subtraction)]' -             # 29
```

Count the volumes reachable below a given volume, following placements
transitively (`+` is one or more `physvol => volume` hops):

```sh
curl -fsSL "$VELO" | ./gg -c 'volume[name=VelolvVelo] / (physvol => volume)+' -  # 57
```

List leaf volumes, those that place nothing inside. `[!physvol]` keeps only
volumes with no `physvol` child:

```sh
curl -fsSL "$VELO" | ./gg -c 'volume[!physvol]' -                               # 46
```

Find placements at any depth with `//`. Physvols are nested inside volumes, not
direct children of `structure`, so `/` finds none and `//` reaches every level:

```sh
curl -fsSL "$VELO" | ./gg -c 'structure / physvol' -                            # 0
curl -fsSL "$VELO" | ./gg -c 'structure // physvol' -                           # 319
```

List the materials reached by placed geometry with `==>`. A physvol references a
volume, not a material, so a single `=>` finds none; `==>` follows the reference
chain physvol → volume → material:

```sh
curl -fsSL "$VELO" | ./gg -c 'physvol => material' -                            # 0
curl -fsSL "$VELO" | ./gg -o name 'physvol ==> material' - | sort -u
```

```
sAluminium
sCarbon
sCopper
...
```

### Verbs

Some questions need the full gdml reference graph or non-flat output, so they're
subcommands rather than queries:

Pretty-print the placement tree under a named volume. `placement-tree` expands
the references into the physical mother/daughter hierarchy, and `--pretty` draws
the connectors:

```sh
curl -fsSL "$VELO" | ./gg --pretty placement-tree VeloVacTanklvVTankDownStream -
```

```
VeloVacTanklvVTankDownStream
├── VeloVacTanklvV5Bx2
├── VeloVacTanklvV5Bx2 (repeated)
├── VeloVacTanklvV5Bx2 (repeated)
├── VeloVacTanklvV5Bx2 (repeated)
├── VeloVacTanklvV5Bx1
├── VeloVacTanklvV5Bx1 (repeated)
└── VeloVacTanklvVTank4B
```

Audit the GDML:

```sh
curl -fsSL "$VELO" | ./gg dangling -      # referenced but never defined (none here)
curl -fsSL "$VELO" | ./gg dead-defs -     # defined but never referenced (79 lines)
```

Trace where a name is referenced. `find-usages` lists every `*ref` that points
at the given name:

```sh
curl -fsSL "$VELO" | ./gg find-usages sSilicon -
```

```
lhcbvelo.gdml:1185: <materialref ref="sSilicon"/>
lhcbvelo.gdml:1198: <materialref ref="sSilicon"/>
...
```

Filter elements by their attributes. `[attr > value]` compares a number, and
`[attr =~/…/]` matches the attribute text against a regular expression:

```sh
curl -fsSL "$VELO" | ./gg -c 'box[x>100]' -                      # 11
curl -fsSL "$VELO" | ./gg -c 'tube[rmax>40]' -                   # 17
curl -fsSL "$VELO" | ./gg -o name 'material[name=~/[Ss]ili/]' -  # sSilicon
```

### Composing with other tools

`gg` writes plain lines, so it pipes into the usual tools. Fetch once, then rank
volumes by the size of the subtree below each:

```sh
curl -fsSL "$VELO" -o lhcbvelo.gdml
for v in $(./gg -o name volume lhcbvelo.gdml); do
  echo "$(./gg -c "volume[name=$v] / (physvol => volume)+" lhcbvelo.gdml) $v"
done | sort -rn | head
```

```
58 World
57 VelolvVelo
12 VeloSupportslvVeloSupport
7 VeloRFFoillvRFUpStreamSection
7 VeloRFFoillvRFPUSect2
7 VeloRFFoillvRFPUSect1
```

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
