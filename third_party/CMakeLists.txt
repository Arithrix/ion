# Vendored code. Versions and licenses: third_party/README.md.
# No warnings flags here — upstream code is built as-is.

# ---- Synth_Dexed (GPL-3.0, MSFA parts Apache-2.0) --------------------------
file(GLOB SYNTH_DEXED_SOURCES CONFIGURE_DEPENDS synth_dexed/src/*.cpp)
add_library(synth_dexed STATIC ${SYNTH_DEXED_SOURCES})
# dexed_port/ goes first: it provides arm_math.h and the platform helpers
# Synth_Dexed normally gets from Teensy or Circle.
target_include_directories(synth_dexed SYSTEM
  PUBLIC ${CMAKE_SOURCE_DIR}/src/engine/fm/dexed_port synth_dexed/src)
target_compile_options(synth_dexed PRIVATE -w)

# ---- FTXUI (MIT) ------------------------------------------------------------
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
add_subdirectory(ftxui EXCLUDE_FROM_ALL)

# ---- doctest (MIT) ----------------------------------------------------------
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE doctest)

# ---- nlohmann/json (MIT) — project files -------------------------------------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE nlohmann)

# ---- toml++ (MIT) — config ----------------------------------------------------
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus SYSTEM INTERFACE tomlplusplus)

# ---- dr_wav (public domain / MIT-0) — WAV I/O ----------------------------------
add_library(dr_libs INTERFACE)
target_include_directories(dr_libs SYSTEM INTERFACE dr_libs)

# ---- SQLite (public domain) — the library index ---------------------------------
add_library(sqlite3 STATIC sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC sqlite)
# No extensions loaded at run time, no Tcl-isms; threads stay on (import runs on a worker).
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_THREADSAFE=2)
target_compile_options(sqlite3 PRIVATE -w)
