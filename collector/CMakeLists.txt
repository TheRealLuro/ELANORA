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
      elanora_device
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
      elanora_device
  )
endif()
