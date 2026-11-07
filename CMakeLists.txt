cmake_minimum_required(VERSION 3.25)

# The one place the version lives (HANDOFF §15).
project(ion VERSION 0.0.1 LANGUAGES C CXX)

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

add_subdirectory(third_party)

# ---- version header, regenerated on every build ---------------------------
set(ION_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(ion_version_header
  COMMAND ${CMAKE_COMMAND}
          -DION_VERSION=${PROJECT_VERSION}
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
  src/core/version.cpp
  src/library/syx.cpp
  src/engine/fm/fm_engine.cpp
  src/engine/module.cpp
  src/audio/pipewire_out.cpp
  src/midi/alsa_seq_in.cpp
)
add_dependencies(ion_engine ion_version_header)
target_include_directories(ion_engine PUBLIC src ${ION_GENERATED_DIR})
target_link_libraries(ion_engine
  PUBLIC Threads::Threads
  PRIVATE ion_warnings synth_dexed PkgConfig::PIPEWIRE PkgConfig::ALSA)

# ---- executables -----------------------------------------------------------
add_executable(ion src/app/main.cpp src/ui/shell.cpp)
target_link_libraries(ion PRIVATE ion_engine ion_warnings ftxui::component ftxui::dom ftxui::screen)

add_executable(ion-bench src/app/bench.cpp)
target_link_libraries(ion-bench PRIVATE ion_engine ion_warnings)

if(ION_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
