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
