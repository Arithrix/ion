add_executable(ion-tests
  test_main.cpp
  test_spsc.cpp
  test_syx.cpp
  test_fm.cpp
  test_module.cpp
  test_version.cpp
)
target_link_libraries(ion-tests PRIVATE ion_engine ion_warnings doctest)
add_test(NAME ion-tests COMMAND ion-tests)

# Dev tool: pretends to be an OXI One on the ALSA sequencer (see TESTING notes).
add_executable(fake-oxi tools/fake_oxi.cpp)
target_link_libraries(fake-oxi PRIVATE PkgConfig::ALSA)
