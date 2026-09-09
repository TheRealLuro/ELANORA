# Developer tools, not part of the shipped system.
#
# These used to sit in module app/ directories next to the real applications,
# where nothing distinguished a program you run to collect data from one you
# run while working on the code. Keeping them here makes that distinction
# visible in the tree rather than only in a comment.

if(ELANORA_BUILD_APPS)
  # Emits reference values from the C++ tone generator for the browser port to
  # be checked against. Run after any change to tone.cpp:
  #
  #   tone_reference > web/fixtures/tone_reference.json
  #
  # The JavaScript conformance test then fails if the two have drifted apart.
  add_executable(tone_reference tone_reference_main.cpp)
  target_link_libraries(tone_reference PRIVATE elanora_collector)
  elanora_set_warnings(tone_reference)

  # Opens a window with one panel and closes cleanly. Proves the UI shell,
  # fonts and theme are wired up without needing a headset.
  add_executable(ui_smoke ui_smoke_main.cpp)
  target_link_libraries(ui_smoke PRIVATE elanora_ui imgui::imgui implot::implot)
  elanora_set_warnings(ui_smoke)
endif()
