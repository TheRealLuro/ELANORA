# Shared vocabulary, CSV layer and UI shell used by all four applications.

# elanora_common carries no third-party dependencies at all. Keeping it free of
# ImGui and BrainFlow means the feature and model libraries -- and their tests
# -- build and run without a graphics stack or a headset attached.
elanora_add_lib(elanora_common
  SOURCES
    src/types.cpp
    src/csv.cpp
)

elanora_add_tests(test_common
  SOURCES
    tests/test_smoke.cpp
    tests/test_types.cpp
    tests/test_csv.cpp
  DEPS
    elanora_common
)

# The GUI shell is separate so headless builds skip the whole graphics stack.
if(ELANORA_BUILD_APPS)
  elanora_add_lib(elanora_ui
    SOURCES
      src/ui_shell.cpp
      src/theme.cpp
      src/render.cpp
    DEPS
      elanora_common
      imgui::imgui
      implot::implot
  )
  # stb_image_write lives here; see cmake/Dependencies.cmake.
  target_include_directories(elanora_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

endif()
