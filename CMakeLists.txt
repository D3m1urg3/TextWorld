cmake_minimum_required(VERSION 3.20)
project(TextWorld C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL REQUIRED)

# --- sqlite3: vendored amalgamation, compiled as C ---
add_library(sqlite3 STATIC vendor/sqlite3.c)
target_include_directories(sqlite3 PUBLIC vendor)
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION)

# --- linenoise: vendored line editor, compiled as C ---
# Upstream antirez/linenoise @ a473823d74b93eab2ba83480df16ed37617493f2
# (2026-05-01), BSD-2. Vendored like sqlite3: no package manager behind it, so
# the commit is recorded here.
add_library(linenoise STATIC vendor/linenoise.c)
target_include_directories(linenoise PUBLIC vendor)

# --- twcore: engine sources shared by game and tests ---
add_library(twcore STATIC src/log.cpp src/db.cpp src/world.cpp src/parser.cpp src/mutations.cpp src/npc.cpp src/systems.cpp src/combat.cpp src/render.cpp src/term.cpp src/band.cpp src/profile.cpp src/aihttp.cpp src/prose.cpp src/nlresolve.cpp src/architect.cpp src/pregen.cpp src/loop.cpp src/bard.cpp src/bardworker.cpp)
target_include_directories(twcore PUBLIC src)
target_include_directories(twcore PRIVATE vendor)
target_link_libraries(twcore PRIVATE sqlite3 linenoise CURL::libcurl)

# --- textworld: game executable ---
add_executable(textworld src/main.cpp)
target_link_libraries(textworld PRIVATE twcore linenoise)

# --- tests: test executable ---
# Links CURL directly so the gated architect live smoke (TEXTWORLD_AI_LIVE_TEST=1)
# can make its single bounded coherence-judge call. The default offline run never
# touches it.
add_executable(tests tests/tests.cpp)
# linenoise is named explicitly, not left to twcore. twcore links it PRIVATE, so
# CMake carries the OBJECTS through to this target but not the PUBLIC include
# directory — which is what the history and spinner tests need in order to
# #include "linenoise.h" at all.
target_link_libraries(tests PRIVATE twcore linenoise CURL::libcurl)
