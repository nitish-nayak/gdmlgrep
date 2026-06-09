cmake_minimum_required(VERSION 3.16)
project(gdmlgrep LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Vendored tree-sitter runtime (lib.c amalgamates the rest of src/).
add_library(tree-sitter STATIC vendor/tree-sitter/src/lib.c)
target_include_directories(tree-sitter
  PUBLIC  vendor/tree-sitter/include
  PRIVATE vendor/tree-sitter/src)

# Vendored, generated GDML grammar.
add_library(gdml-grammar STATIC
  vendor/grammar/parser.c
  vendor/grammar/scanner.c)
target_include_directories(gdml-grammar PRIVATE vendor/grammar)

# libgg: the header-only engine.
add_library(gg-engine INTERFACE)
target_include_directories(gg-engine INTERFACE include)
target_link_libraries(gg-engine INTERFACE tree-sitter gdml-grammar)

# The gg CLI.
add_executable(gg src/main.cpp)
target_compile_options(gg PRIVATE -Wall -Wextra)
target_link_libraries(gg PRIVATE gg-engine)

# Tests (built by default; run directly).
enable_testing()
add_executable(test_prewalk tests/test_prewalk.cpp)
target_compile_options(test_prewalk PRIVATE -Wall -Wextra)
target_link_libraries(test_prewalk PRIVATE gg-engine)
add_test(NAME prewalk COMMAND test_prewalk ${CMAKE_CURRENT_SOURCE_DIR}/../tree-sitter-gdml/gdml/simple.gdml)

add_executable(test_nfa tests/test_nfa.cpp)
target_compile_options(test_nfa PRIVATE -Wall -Wextra)
target_link_libraries(test_nfa PRIVATE gg-engine)

add_executable(bench tests/bench.cpp)
target_compile_options(bench PRIVATE -Wall -Wextra)
target_link_libraries(bench PRIVATE gg-engine)

add_executable(test_match tests/test_match.cpp)
target_compile_options(test_match PRIVATE -Wall -Wextra)
target_link_libraries(test_match PRIVATE gg-engine)
add_test(NAME match_suite COMMAND test_match ${CMAKE_CURRENT_SOURCE_DIR}/../tree-sitter-gdml/gdml/simple.gdml)
add_test(NAME nfa COMMAND test_nfa "volume / (physvol => volume)%")

# End-to-end smoke: a deref query must find matches (exit 0).
add_test(NAME match COMMAND gg "physvol => volume"
         ${CMAKE_CURRENT_SOURCE_DIR}/../tree-sitter-gdml/gdml/simple.gdml)

# A numeric-guard query must find matches (exit 0).
add_test(NAME guard COMMAND gg "box[x>200]"
         ${CMAKE_CURRENT_SOURCE_DIR}/../tree-sitter-gdml/gdml/simple.gdml)
