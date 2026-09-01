# Stimulus design, round scheduling and the trial state machine.
#
# Pure logic with no ImGui, no audio device and no BrainFlow, so the
# experiment's rules are testable without hardware or a window.

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
