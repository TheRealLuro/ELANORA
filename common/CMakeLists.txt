# Shared vocabulary, CSV layer and UI shell used by all four applications.

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
