# Muse 2 device I/O and streaming.

if(ELANORA_WITH_BRAINFLOW)
  elanora_add_lib(elanora_lsl
    SOURCES
      src/muse_device.cpp
      src/stream_recorder.cpp
      src/signal_quality.cpp
    DEPS
      elanora_common
      elanora::brainflow
  )

  elanora_add_tests(test_lsl
    SOURCES
      tests/test_muse_device.cpp
      tests/test_stream_recorder.cpp
      tests/test_signal_quality.cpp
    DEPS
      elanora_lsl
  )
  elanora_copy_runtime_dlls(test_lsl)

  elanora_add_app(muse_monitor
    SOURCES app/muse_monitor_main.cpp
    DEPS    elanora_lsl elanora_ui elanora_collector_ui
  )

  # Hardware-free diagnostic: prints the channel map BrainFlow reports for the
  # Muse 2, and guards the constants the rest of the project is built around.
  add_executable(muse_probe app/muse_probe_main.cpp)
  elanora_set_warnings(muse_probe)
  target_link_libraries(muse_probe PRIVATE elanora_common elanora::brainflow)
  elanora_copy_runtime_dlls(muse_probe)
endif()
