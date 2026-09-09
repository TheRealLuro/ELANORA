cmake_minimum_required(VERSION 3.24)
project(ELANORA VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every binary in one place so the BrainFlow runtime DLLs only need to be
# copied once rather than into five scattered per-target directories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Kept as a no-op shim so the add_* helpers below need no edit if per-target
# warning control is introduced later.
function(elanora_set_warnings NAME)
endfunction()

enable_testing()
include(CTest)
include(cmake/Dependencies.cmake)

# Our warning and conformance flags are set AFTER the dependencies are pulled
# in, on purpose. A directory-scope add_compile_options is inherited by every
# add_subdirectory issued after it, and FetchContent_MakeAvailable issues one
# per dependency from inside Dependencies.cmake. Setting these first therefore
# compiled the vendored sources with our flags, and two of them are not merely
# noisy there -- they break the build outright:
#
#   /permissive-       makes C4596 (illegal qualified name in a member
#                      declaration) fire in SimpleBLE's PeripheralBase.h
#   /Zc:preprocessor   enables C5102 (invalid command-line macro definition),
#                      which SimpleBLE trips by passing "/D_WIN32_WINNT=..."
#                      through target_compile_definitions
#
# SimpleBLE compiles with /WX, so both became hard errors and no
# simpleble-c.dll was produced -- which is what a Muse 2 needs to connect over
# BLE. Vendored code compiles with its own flags; ours apply from here down.
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

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
  elanora_set_warnings(${NAME})
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
  elanora_set_warnings(${NAME})
  target_link_libraries(${NAME} PRIVATE ${ARG_DEPS} imgui::imgui implot::implot)
  elanora_copy_runtime_dlls(${NAME})
endfunction()

# elanora_copy_runtime_dlls(<target>)
# Places BoardController.dll and friends beside the executable. Without this the
# program builds fine and then dies at launch with 0xC0000135 (DLL not found),
# which gives no hint about what is missing.
function(elanora_copy_runtime_dlls NAME)
  if(WIN32 AND ELANORA_WITH_BRAINFLOW AND ELANORA_BRAINFLOW_DLL_DIR)
    # simpleble-c is not linked by anything -- BrainFlow loads it with
    # LoadLibrary the first time a BLE board is opened. Nothing therefore pulls
    # it into the build, and the copy below would silently ship without it,
    # leaving the Muse 2 to fail at connect time with
    #   "failed to load lib simpleble-c.dll".
    if(TARGET simpleble-c)
      add_dependencies(${NAME} simpleble-c)
    endif()
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
  elanora_set_warnings(${NAME})
  target_link_libraries(${NAME} PRIVATE ${ARG_DEPS} Catch2::Catch2WithMain)
  catch_discover_tests(${NAME})
endfunction()

# ---------------------------------------------------------------------------
# Modules. Order matters: each depends only on those above it.
#   common    <- everything
#   device    -> common
#   collector -> common, device
#   data      -> common
#   models    -> common, data   (the optimizer consumes OutcomeEvidence)
# ---------------------------------------------------------------------------
add_subdirectory(common)
add_subdirectory(device)
add_subdirectory(collector)
add_subdirectory(data)
add_subdirectory(models)
add_subdirectory(server)
add_subdirectory(tools)

message(STATUS "")
message(STATUS "ELANORA configured")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  BrainFlow target: ${ELANORA_BRAINFLOW_TARGET}")
message(STATUS "  Binaries        : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
