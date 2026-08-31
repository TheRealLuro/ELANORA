# Muse 2 device I/O and streaming.

# Hardware-free diagnostic: prints the channel map BrainFlow reports for the
# Muse 2. Doubles as the integration check that BrainFlow links at all.
if(ELANORA_WITH_BRAINFLOW AND ELANORA_BUILD_APPS)
  add_executable(muse_probe app/muse_probe_main.cpp)
  target_link_libraries(muse_probe PRIVATE elanora_common ${ELANORA_BRAINFLOW_TARGET})
endif()
