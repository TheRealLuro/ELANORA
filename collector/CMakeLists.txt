# Stimulus design, round scheduling and the trial state machine.
#
# elanora_collector is pure logic: no ImGui, no audio device, no BrainFlow, so
# the experiment's rules are testable without hardware or a window.

elanora_add_lib(elanora_collector
  SOURCES
    src/session.cpp
  DEPS
    elanora_common
)

elanora_add_tests(test_collector
  SOURCES
    tests/test_session.cpp
  DEPS
    elanora_collector
)

# The tab UI is a separate target so the logic library above stays free of
# ImGui and can keep being tested headless.
if(ELANORA_BUILD_APPS AND ELANORA_WITH_BRAINFLOW)
  elanora_add_lib(elanora_collector_ui
    SOURCES
      src/view.cpp
    DEPS
      elanora_collector
      elanora_ui
      elanora_lsl
  )
endif()
