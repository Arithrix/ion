cmake_minimum_required(VERSION 3.25)

# The one place the version lives (HANDOFF §15). scripts/release.sh edits
# these two lines. ION_PRERELEASE is empty or e.g. "-dev.4".
project(ion VERSION 0.4.0 LANGUAGES C CXX)
set(ION_PRERELEASE "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(ION_BUILD_TESTS "Build the test suite" ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

# Release builds: -O2 with debug info (split out by release.sh), and the C++
# runtime linked statically so the binary runs on other distros (the Comet).
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -g -DNDEBUG")
add_link_options($<$<CONFIG:Release>:-static-libstdc++> $<$<CONFIG:Release>:-static-libgcc>)

add_subdirectory(third_party)

# ---- version header, regenerated on every build ---------------------------
set(ION_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(ion_version_header
  COMMAND ${CMAKE_COMMAND}
          -DION_VERSION=${PROJECT_VERSION}${ION_PRERELEASE}
          -DION_SOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DION_OUTPUT=${ION_GENERATED_DIR}/ion_version.h
          -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${ION_GENERATED_DIR}/ion_version.h
  COMMENT "Generating ion_version.h")

# ---- warnings for our own code only ---------------------------------------
add_library(ion_warnings INTERFACE)
target_compile_options(ion_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                       -Wno-sign-conversion)

# ---- engine library: everything except the UI ------------------------------
add_library(ion_engine STATIC
  src/core/params.cpp
  src/core/version.cpp
  src/dsp/biquad.cpp
  src/library/fm_library.cpp
  src/library/recording.cpp
  src/library/sample_loader.cpp
  src/library/syx.cpp
  src/engine/channel_eq.cpp
  src/engine/clock.cpp
  src/engine/engine_types.cpp
  src/engine/fm/dx7_voice.cpp
  src/engine/drum/drum_engine.cpp
  src/engine/fm/fm_engine.cpp
  src/engine/fx/effects.cpp
  src/engine/sampler/sampler_engine.cpp
  src/engine/module.cpp
  src/audio/pipewire_out.cpp
  src/audio/wav.cpp
  src/midi/alsa_seq_in.cpp
  src/midi/smf.cpp
  src/project/config.cpp
  src/project/offline.cpp
  src/project/paths.cpp
  src/project/session.cpp
)
add_dependencies(ion_engine ion_version_header)
target_include_directories(ion_engine PUBLIC src ${ION_GENERATED_DIR})
target_link_libraries(ion_engine
  PUBLIC Threads::Threads nlohmann_json
  PRIVATE ion_warnings synth_dexed sqlite3 samplerate tomlplusplus dr_libs PkgConfig::PIPEWIRE PkgConfig::ALSA)

# ---- executables -----------------------------------------------------------
add_library(ion_ui STATIC
  src/ui/algorithm_diagram.cpp
  src/ui/app.cpp
  src/ui/drum_screen.cpp
  src/ui/fm_browse.cpp
  src/ui/fx_screen.cpp
  src/ui/instrument_screen.cpp
  src/ui/library_screen.cpp
  src/ui/mixer_screen.cpp
  src/ui/piano.cpp
  src/ui/sample_browse.cpp
  src/ui/sample_view.cpp
  src/ui/sampler_screen.cpp
  src/ui/settings_screen.cpp
  src/ui/theme.cpp
  src/ui/tracks_screen.cpp
)
target_link_libraries(ion_ui PUBLIC ion_engine ftxui::component ftxui::dom ftxui::screen PRIVATE ion_warnings)

add_executable(ion src/app/main.cpp)
target_link_libraries(ion PRIVATE ion_ui ion_warnings)

add_executable(ion-bench src/app/bench.cpp)
target_link_libraries(ion-bench PRIVATE ion_engine ion_warnings)

add_executable(ion-render src/app/render.cpp)
target_link_libraries(ion-render PRIVATE ion_engine ion_warnings)

if(ION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
