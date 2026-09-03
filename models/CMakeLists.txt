# models -- regression, validation, and the inverse optimizer.
#
# Depends on data/ deliberately: the optimizer takes OutcomeEvidence and must
# refuse goals on outcomes the evidence gate found nothing for. The gate lives
# in data/ because it is data analysis, so the arrow points this way.

elanora_add_lib(elanora_models
  SOURCES
    src/ridge.cpp
    src/gp.cpp
    src/validation.cpp
    src/model_registry.cpp
    src/optimizer.cpp
  DEPS
    elanora_common
    elanora_data
    Eigen3::Eigen
)

elanora_add_tests(test_models
  SOURCES
    tests/test_models_core.cpp
    tests/test_registry_optimizer.cpp
    tests/test_pipeline.cpp
  DEPS
    elanora_models
)

# Train, predict forward, optimise inverse.
# Named _app for the same reason as elanora_data_app; the executable is still
# elanora_models.exe.
elanora_add_app(elanora_models_app
  SOURCES
    app/models_main.cpp
  DEPS
    elanora_models
    elanora_ui
)
if(TARGET elanora_models_app)
  set_target_properties(elanora_models_app PROPERTIES OUTPUT_NAME elanora_models)
endif()
