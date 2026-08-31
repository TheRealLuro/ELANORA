# Muse 2 device I/O and streaming.

if(ELANORA_WITH_BRAINFLOW)
  elanora_add_lib(elanora_lsl
    SOURCES
      src/muse_device.cpp
    DEPS
      elanora_common
      elanora::brainflow
  )

  elanora_add_tests(test_lsl
    SOURCES
      tests/test_muse_device.cpp
    DEPS
      elanora_lsl
  )
  elanora_copy_runtime_dlls(test_lsl)

  # Hardware-free diagnostic: prints the channel map BrainFlow reports for the
  # Muse 2, and guards the constants the rest of the project is built around.
  add_executable(muse_probe app/muse_probe_main.cpp)
  target_link_libraries(muse_probe PRIVATE elanora_common elanora::brainflow)
  elanora_copy_runtime_dlls(muse_probe)
endif()
