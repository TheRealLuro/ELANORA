# data -- feature math, statistics, and the evidence gate.
#
# Deliberately free of BrainFlow, ImGui, and audio: everything here is pure
# numeric code over std::vector, so every feature is verifiable on synthesised
# signals in CI on a machine with no headset.

elanora_add_lib(elanora_data
  SOURCES
    src/dsp.cpp
    src/eeg_features.cpp
    src/heart_features.cpp
    src/breath_features.cpp
    src/qc.cpp
    src/table.cpp
    src/dataset_builder.cpp
    src/stats.cpp
    src/evidence.cpp
  DEPS
    elanora_common
)

elanora_add_tests(test_data
  SOURCES
    tests/test_dsp.cpp
    tests/test_eeg_features.cpp
    tests/test_heart_features.cpp
    tests/test_breath_features.cpp
    tests/test_qc.cpp
    tests/test_dataset_builder.cpp
    tests/test_stats.cpp
    tests/test_evidence.cpp
  DEPS
    elanora_data
)

# The dataset browser and the evidence gate. Needs the UI shell, so it only
# exists in a build with apps enabled.
# The target is named _app because elanora_data is already the library; the
# executable it produces is still elanora_data.exe.
elanora_add_app(elanora_data_app
  SOURCES
    app/data_main.cpp
  DEPS
    elanora_data
    elanora_ui
)
if(TARGET elanora_data_app)
  set_target_properties(elanora_data_app PROPERTIES OUTPUT_NAME elanora_data)
endif()
