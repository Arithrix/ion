add_executable(ion-tests
  test_main.cpp
  test_spsc.cpp
  test_syx.cpp
  test_dx7.cpp
  test_envelope.cpp
  test_drum.cpp
  test_library.cpp
  test_fm.cpp
  test_fm_handover.cpp
  test_fx.cpp
  test_midi_alsa.cpp
  test_module.cpp
  test_params_eq.cpp
  test_sampler.cpp
  test_session.cpp
  test_recording.cpp
  test_undo.cpp
  test_plaits.cpp
  test_modulation.cpp
  test_acid.cpp
  test_chip.cpp
  test_render.cpp
  test_ui.cpp
  test_version.cpp
)
target_link_libraries(ion-tests PRIVATE ion_ui ion_engine ion_warnings doctest PkgConfig::ALSA)
target_compile_definitions(ion-tests PRIVATE ION_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME ion-tests COMMAND ion-tests)

# Dev tool: pretends to be an OXI One on the ALSA sequencer (see TESTING notes).
add_executable(fake-oxi tools/fake_oxi.cpp)
target_link_libraries(fake-oxi PRIVATE PkgConfig::ALSA)

# Release tool: builds the demo project from a DX7 bank on this machine.
add_executable(make-demo tools/make_demo.cpp)
target_link_libraries(make-demo PRIVATE ion_engine)
