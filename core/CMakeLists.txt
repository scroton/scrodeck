# core/CMakeLists.txt — builds the model core and its test runner.
#   cmake -S core -B core/build && cmake --build core/build && core/build/deck-core-test
# The core is pure C++17 (no OS APIs), so any compiler works. On the build box a WinLibs MinGW-w64 g++ is
# installed (see core/test/build.ps1 for a no-CMake one-liner if cmake is not available).
cmake_minimum_required(VERSION 3.15)
project(claude_deck_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(deck_core STATIC
  model/layout.cpp
  model/hibernate.cpp
  model/prompts.cpp
  model/triggers.cpp
)
target_include_directories(deck_core PUBLIC model)

# the terminal engine (core/vt): a port of xterm.js 6's parser and buffer, no OS calls
add_library(deck_vt STATIC
  vt/terminal.cpp
  vt/buffer.cpp
  vt/parser.cpp
  vt/unicode.cpp
)
target_include_directories(deck_vt PUBLIC vt)

add_executable(deck-core-test test/run.cpp)
target_link_libraries(deck-core-test PRIVATE deck_core)

add_executable(deck-vt-test test/vt/vt_test.cpp)
target_link_libraries(deck-vt-test PRIVATE deck_vt)

add_executable(deck-proto-test test/proto/proto_test.cpp)

if(MINGW)
  # a dynamically linked MinGW exe exits silently when the runtime DLLs are not on PATH
  foreach(t deck-core-test deck-vt-test deck-proto-test)
    target_link_options(${t} PRIVATE -static -static-libgcc -static-libstdc++)
  endforeach()
endif()

enable_testing()
add_test(NAME model COMMAND deck-core-test)
add_test(NAME vt COMMAND deck-vt-test ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/vt)
add_test(NAME proto COMMAND deck-proto-test ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures)
