# Build the gg CLI from src/gg.cpp + the header-only engine + the vendored
# tree-sitter runtime and GDML grammar. (Native libgg + tests are in CMake.)
#
#   make            portable APE via cosmocc/cosmoc++ (on PATH)
#   make NATIVE=1   native binary via gcc/g++
#   make clean
#
# Both modes share build/, so run `make clean` when switching between them.
ifeq ($(NATIVE),1)
  CC  := gcc
  CXX := g++
  CSTD := -std=gnu11          # GNU mode: glibc's <endian.h> exposes le16toh etc.
  ENDIAN :=
else
  CC  := cosmocc
  CXX := cosmoc++
  CSTD := -std=c11
  ENDIAN := -DHAVE_ENDIAN_H   # cosmocc lacks __linux__; route endian.h to cosmo's <endian.h>
endif

BUILD := build

# Runtime and grammar each ship a tree_sitter/ header dir, but their files are
# disjoint and never cross-include, so one shared -I set is safe.
CPPFLAGS += $(ENDIAN) \
            -Iinclude \
            -Itree-sitter/tree-sitter/lib/include \
            -Itree-sitter/tree-sitter/lib/src \
            -Itree-sitter/tree-sitter-gdml/gdml/src
CFLAGS   += $(CSTD) -O2
CXXFLAGS += -std=c++17 -O2 -Wall -Wextra

C_SRCS   := tree-sitter/tree-sitter/lib/src/lib.c \
            tree-sitter/tree-sitter-gdml/gdml/src/parser.c \
            tree-sitter/tree-sitter-gdml/gdml/src/scanner.c
CXX_SRCS := src/gg.cpp
OBJS     := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o) $(CXX_SRCS:.cpp=.o))

gg: $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) -r $(BUILD) gg gg.com.dbg gg.aarch64.elf

.PHONY: clean
