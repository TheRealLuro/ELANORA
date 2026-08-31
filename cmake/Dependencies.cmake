# ELANORA third-party dependencies.
#
# Everything is fetched at configure time so the repo stays free of vendored
# source. The one exception is miniaudio, a single header, which is downloaded
# to collector/third_party/ by this file.
#
# Dependency graph these targets serve:
#   common    -> (none)
#   LSL       -> BrainFlow
#   collector -> miniaudio
#   data      -> BrainFlow (DataFilter DSP), Eigen
#   models    -> Eigen
#   all apps  -> glfw, imgui, implot, OpenGL

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Heavy dependencies are opt-out so the toolchain can be verified quickly:
#
#   cmake -B build -S . -DELANORA_BUILD_APPS=OFF -DELANORA_WITH_BRAINFLOW=OFF
#
# configures and builds in seconds with only Catch2, which is enough to prove
# the compiler works. Turn them back on as the milestones that need them land.
option(ELANORA_BUILD_APPS    "Fetch GLFW/ImGui/ImPlot and build the GUI apps" ON)
option(ELANORA_WITH_BRAINFLOW "Fetch and build BrainFlow (device I/O + DSP)"  ON)
option(ELANORA_WITH_EIGEN     "Fetch Eigen (model and statistics math)"       ON)

# ---------------------------------------------------------------------------
# Catch2 v3 -- test framework
# ---------------------------------------------------------------------------
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ---------------------------------------------------------------------------
# Eigen 3.4 -- linear algebra for Ridge / GP / statistics
#
# Eigen's own CMake pulls in a large test and doc tree we do not want, so the
# sub-build is switched off and the headers are exposed directly.
# ---------------------------------------------------------------------------
if(ELANORA_WITH_EIGEN)
  set(EIGEN_BUILD_DOC        OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_TESTING    OFF CACHE BOOL "" FORCE)
  set(EIGEN_BUILD_PKGCONFIG  OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(Eigen3
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(Eigen3)
endif()

# ---------------------------------------------------------------------------
# GLFW -- windowing for the four ImGui apps
# ---------------------------------------------------------------------------
if(ELANORA_BUILD_APPS)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(glfw)

  find_package(OpenGL REQUIRED)

  # ---------------------------------------------------------------------------
  # Dear ImGui (docking branch) -- ships no CMake target, so one is defined here.
  # ---------------------------------------------------------------------------
  # Pinned to a tag, not the moving 'docking' branch: an unpinned branch means
  # a dependency can change under you between two clean checkouts of the same
  # commit. v1.90.9 is the version ImPlot v0.16 below was released against.
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9-docking
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
  add_library(imgui::imgui ALIAS imgui)

  # ---------------------------------------------------------------------------
  # ImPlot -- live signal plots. Also ships no CMake target.
  # ---------------------------------------------------------------------------
  # v0.16 pairs with ImGui v1.90.x. Bumping one without the other is the most
  # likely source of a sudden wall of compile errors in this file.
  FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(implot)

  add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
  )
  target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
  target_link_libraries(implot PUBLIC imgui)
  add_library(implot::implot ALIAS implot)
endif()

# ---------------------------------------------------------------------------
# BrainFlow -- Muse 2 device I/O and DSP (band power, filters).
#
# Built from source so the C++ headers and libs match this toolchain exactly.
# BrainFlow's CMake produces BoardController and DataHandler shared libs; the
# app targets need those DLLs beside the executable at runtime, which the
# elanora_add_app() helper in the top-level CMakeLists handles.
# ---------------------------------------------------------------------------
if(ELANORA_WITH_BRAINFLOW)
  set(BRAINFLOW_VERSION "5.16.0" CACHE STRING "BrainFlow git tag to build against")
  FetchContent_Declare(brainflow
    GIT_REPOSITORY https://github.com/brainflow-dev/brainflow.git
    GIT_TAG        ${BRAINFLOW_VERSION}
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(brainflow)

  # BrainFlow's own targets do not export their include directories, and the
  # C++ API headers are spread across five directories rather than one. Wrap
  # that up once here so every module can just link elanora::brainflow.
  add_library(elanora_brainflow INTERFACE)
  target_link_libraries(elanora_brainflow INTERFACE ${ELANORA_BRAINFLOW_TARGET})
  target_include_directories(elanora_brainflow SYSTEM INTERFACE
    ${brainflow_SOURCE_DIR}/cpp_package/src/inc      # board_shim.h, data_filter.h
    ${brainflow_SOURCE_DIR}/src/utils/inc            # brainflow_array/constants/exception
    ${brainflow_SOURCE_DIR}/src/board_controller/inc # board_controller.h, input_params
    ${brainflow_SOURCE_DIR}/src/data_handler/inc     # data_handler.h
    ${brainflow_SOURCE_DIR}/src/ml/inc               # brainflow_model_params.h
    ${brainflow_SOURCE_DIR}/third_party/json         # json.hpp
  )
  add_library(elanora::brainflow ALIAS elanora_brainflow)

  # BrainFlow writes its shared libraries into its own source tree rather than
  # the build tree, so $<TARGET_RUNTIME_DLLS> does not find them. Record the
  # location for elanora_copy_runtime_dlls() in the top-level CMakeLists.
  set(ELANORA_BRAINFLOW_DLL_DIR "${brainflow_SOURCE_DIR}/compiled/$<CONFIG>"
      CACHE INTERNAL "Directory holding BoardController.dll and friends")
endif()

# ---------------------------------------------------------------------------
# miniaudio -- single-header audio playback for the stimulus generator.
# Downloaded rather than committed; see .gitignore.
# ---------------------------------------------------------------------------
if(ELANORA_BUILD_APPS)
  set(MINIAUDIO_HEADER "${CMAKE_SOURCE_DIR}/collector/third_party/miniaudio.h")
  if(NOT EXISTS "${MINIAUDIO_HEADER}")
    message(STATUS "Downloading miniaudio.h ...")
    file(DOWNLOAD
      "https://raw.githubusercontent.com/mackron/miniaudio/0.11.21/miniaudio.h"
      "${MINIAUDIO_HEADER}"
      TLS_VERIFY ON
      STATUS _ma_status
    )
    list(GET _ma_status 0 _ma_code)
    if(NOT _ma_code EQUAL 0)
      message(FATAL_ERROR "Failed to download miniaudio.h: ${_ma_status}")
    endif()
  endif()

  add_library(miniaudio INTERFACE)
  target_include_directories(miniaudio INTERFACE "${CMAKE_SOURCE_DIR}/collector/third_party")
  add_library(miniaudio::miniaudio ALIAS miniaudio)
endif()
