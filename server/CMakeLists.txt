# The web collector's host.
#
# Depends on collector for build_schedule() and the protocol constants: the
# phone is handed a plan rather than owning a second copy of the experiment
# design, which would drift from the tested one.
#
# No ImGui and no BrainFlow. This process talks HTTP and writes CSV, and a
# machine with neither a screen nor a Bluetooth radio can still run it.

if(ELANORA_BUILD_APPS)
  elanora_add_lib(elanora_server
    SOURCES
      src/schedule_json.cpp
      src/round_store.cpp
    DEPS
      elanora_collector
  )

  add_executable(elanora_serve_app app/elanora_serve_main.cpp)
  set_target_properties(elanora_serve_app PROPERTIES OUTPUT_NAME elanora_serve)
  target_link_libraries(elanora_serve_app PRIVATE elanora_server httplib::httplib)
  elanora_set_warnings(elanora_serve_app)

  elanora_add_tests(test_server
    SOURCES
      tests/test_schedule_json.cpp
      tests/test_round_store.cpp
    DEPS
      elanora_server
  )
endif()
