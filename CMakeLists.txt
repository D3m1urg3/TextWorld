cmake_minimum_required(VERSION 3.20)
project(TextWorld C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- sqlite3: vendored amalgamation, compiled as C ---
add_library(sqlite3 STATIC vendor/sqlite3.c)
target_include_directories(sqlite3 PUBLIC vendor)
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION)

# --- twcore: engine sources shared by game and tests ---
add_library(twcore STATIC src/db.cpp src/world.cpp src/parser.cpp src/mutations.cpp src/systems.cpp src/render.cpp src/loop.cpp)
target_include_directories(twcore PUBLIC src)
target_link_libraries(twcore PRIVATE sqlite3)

# --- textworld: game executable ---
add_executable(textworld src/main.cpp)
target_link_libraries(textworld PRIVATE twcore)

# --- tests: test executable ---
add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE twcore)
