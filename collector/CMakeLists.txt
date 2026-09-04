# Stimulus design, round scheduling, the trial state machine, and tone DSP.
#
# elanora_collector is pure: no ImGui, no audio device, no BrainFlow, so the
# experiment's rules and the tone generator are testable headless.

elanora_add_lib(elanora_collector
  SOURCES
    src/session.cpp
    src/tone.cpp
  DEPS
    elanora_common
)

elanora_add_tests(test_collector
  SOURCES
    tests/test_session.cpp
    tests/test_tone.cpp
  DEPS
    elanora_collector
)

# Audio output and disk recording. Separate from the pure library because one
# needs a sound device and the other needs BrainFlow's Sample type.
if(ELANORA_BUILD_APPS AND ELANORA_WITH_BRAINFLOW)
  elanora_add_lib(elanora_collector_io
    SOURCES
      src/audio_engine.cpp
      src/recorder.cpp
    DEPS
      elanora_collector
      elanora_lsl
      miniaudio::miniaudio
  )

  # Recording needs BrainFlow's Sample type, so these tests only exist in a
  # build that has it. The pure-logic tests above always run.
  elanora_add_tests(test_collector_io
    SOURCES
      tests/test_recorder.cpp
    DEPS
      elanora_collector_io
  )

  elanora_add_lib(elanora_collector_ui
    SOURCES
      src/view.cpp
    DEPS
      elanora_collector
      elanora_collector_io
      elanora_ui
      elanora_lsl
  )
endif()

# Emits reference values from the C++ tone generator for the browser port to be
# checked against. Not part of the shipped system -- a conformance tool.
#
#   tone_reference > web/fixtures/tone_reference.json
if(ELANORA_BUILD_APPS)
  add_executable(tone_reference app/tone_reference_main.cpp)
  target_link_libraries(tone_reference PRIVATE elanora_collector)
  elanora_set_warnings(tone_reference)
endif()
