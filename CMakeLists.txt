cmake_minimum_required(VERSION 3.24)
project(ELANORA VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every binary in one place so the BrainFlow runtime DLLs only need to be
# copied once rather than into five scattered per-target directories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor)
  # M_PI and friends
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()
include(CTest)
include(cmake/Dependencies.cmake)

# The BrainFlow C++ binding target name. Isolated in one variable because it is
# the single most likely thing to need adjusting on first configure -- if the
# build fails with "Target Brainflow not found", run
#   cmake --build build --target help
# to list the real name and set -DELANORA_BRAINFLOW_TARGET=<name>.
set(ELANORA_BRAINFLOW_TARGET "Brainflow" CACHE STRING "BrainFlow C++ target to link")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# elanora_add_lib(<name> SOURCES ... DEPS ...)
# A static library with its module's include/ directory exposed publicly.
function(elanora_add_lib NAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${NAME} STATIC ${ARG_SOURCES})
  target_include_directories(${NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  if(ARG_DEPS)
    target_link_libraries(${NAME} PUBLIC ${ARG_DEPS})
  endif()
endfunction()

# elanora_add_app(<name> SOURCES ... DEPS ...)
# A GUI executable. On Windows the BrainFlow shared libraries must sit beside
# the .exe at runtime, so they are copied post-build.
function(elanora_add_app NAME)
  if(NOT ELANORA_BUILD_APPS)
    message(STATUS "Skipping app ${NAME} (ELANORA_BUILD_APPS=OFF)")
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${NAME} ${ARG_SOURCES})
  target_link_libraries(${NAME} PRIVATE ${ARG_DEPS} imgui::imgui implot::implot)
  elanora_copy_runtime_dlls(${NAME})
endfunction()

# elanora_copy_runtime_dlls(<target>)
# Places BoardController.dll and friends beside the executable. Without this the
# program builds fine and then dies at launch with 0xC0000135 (DLL not found),
# which gives no hint about what is missing.
function(elanora_copy_runtime_dlls NAME)
  if(WIN32 AND ELANORA_WITH_BRAINFLOW AND ELANORA_BRAINFLOW_DLL_DIR)
    add_custom_command(TARGET ${NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${ELANORA_BRAINFLOW_DLL_DIR}" "$<TARGET_FILE_DIR:${NAME}>"
      COMMENT "Copying BrainFlow runtime libraries beside ${NAME}"
    )
  endif()
endfunction()

# elanora_add_tests(<name> SOURCES ... DEPS ...)
# A Catch2 test binary, auto-registered with CTest.
function(elanora_add_tests NAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${NAME} ${ARG_SOURCES})
  target_link_libraries(${NAME} PRIVATE ${ARG_DEPS} Catch2::Catch2WithMain)
  catch_discover_tests(${NAME})
endfunction()

# ---------------------------------------------------------------------------
# Modules. Order matters: each depends only on those above it.
#   common    <- everything
#   LSL       -> common
#   collector -> common, LSL
#   data      -> common
#   models    -> common, data   (the optimizer consumes OutcomeEvidence)
# ---------------------------------------------------------------------------
add_subdirectory(common)
add_subdirectory(LSL)
add_subdirectory(collector)
add_subdirectory(data)
add_subdirectory(models)

message(STATUS "")
message(STATUS "ELANORA configured")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  BrainFlow target: ${ELANORA_BRAINFLOW_TARGET}")
message(STATUS "  Binaries        : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
