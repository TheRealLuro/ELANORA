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

  # REQUIRED for the Muse 2. BrainFlow defaults BUILD_BLE to OFF, which silently
  # produces a BoardController that cannot open any Bluetooth Low Energy board:
  # the DLL loads, prepare_session() is reached, and only then does it fail with
  # "failed to load lib simpleble-c.dll" -- a message that reads like a missing
  # runtime rather than a build option that was never turned on.
  #
  # MUSE_2_BOARD talks native BLE with no dongle, so without this the headset
  # can never connect, however healthy the electrodes are.
  set(BUILD_BLE ON CACHE BOOL "" FORCE)
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

  # Upstream bug, BrainFlow 5.16.0: several vendored bglib sources use symbols
  # without including the header that declares them. They compiled historically
  # because older MSVC STL headers pulled these in transitively; 19.44 no
  # longer does.
  #
  # Injected at configure time rather than kept as patch files, so a fresh
  # FetchContent clone self-heals and the fix survives a version bump that
  # happens to fix it upstream (the guard makes it a no-op then).
  set(_bf_missing_includes
    "src/board_controller/muse/muse_bglib/muse_bglib_helper.cpp|chrono"
    "src/board_controller/muse/muse_bglib/uart.cpp|stdlib.h"
    "src/board_controller/neuromd/brainbit_bglib/uart.cpp|stdlib.h"
    "src/board_controller/openbci/ganglion_bglib/uart.cpp|stdlib.h"
  )
  foreach(_entry IN LISTS _bf_missing_includes)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _rel)
    list(GET _parts 1 _hdr)
    set(_abs "${brainflow_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_abs}")
      file(READ "${_abs}" _src)
      string(FIND "${_src}" "#include <${_hdr}>" _found)
      if(_found EQUAL -1)
        string(PREPEND _src "#include <${_hdr}>
")
        file(WRITE "${_abs}" "${_src}")
        message(STATUS "BrainFlow: injected #include <${_hdr}> into ${_rel}")
      endif()
    endif()
  endforeach()

  # BrainFlow ships backends for many headsets. BrainBit and OpenBCI Ganglion
  # are not used here and each pulls in a sizeable vendored bglib tree, so they
  # are dropped from the default build purely to save compile time. They do
  # compile once the include injection above has run.
  foreach(_bf_unused BrainBitLib GanglionLib)
    if(TARGET ${_bf_unused})
      set_target_properties(${_bf_unused} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
  endforeach()

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
  # ---------------------------------------------------------------------------
  # Fonts. ImGui's built-in face is ProggyClean, a 13px bitmap font, and it is
  # the single biggest reason an ImGui app reads as a debug overlay rather than
  # an application. Fira Sans / Fira Code are the pairing the design system
  # selected for data dashboards.
  #
  # Downloaded rather than committed so the repo stays free of binaries. If the
  # download fails the app falls back to system Segoe UI / Consolas, so a
  # missing network is a cosmetic downgrade rather than a build failure.
  # ---------------------------------------------------------------------------
  set(ELANORA_FONT_DIR "${CMAKE_SOURCE_DIR}/common/assets/fonts")
  file(MAKE_DIRECTORY "${ELANORA_FONT_DIR}")
  # raw.githubusercontent.com directly rather than the github.com/.../raw
  # redirect, which is one more hop that can fail silently.
  set(_font_base "https://raw.githubusercontent.com/mozilla/Fira/4.202/ttf")
  set(_fonts FiraSans-Regular.ttf FiraSans-Medium.ttf FiraSans-Light.ttf FiraMono-Regular.ttf)
  foreach(_name IN LISTS _fonts)
    set(_dest "${ELANORA_FONT_DIR}/${_name}")
    # Re-fetch anything implausibly small: file(DOWNLOAD) does NOT fail on an
    # HTTP 404, it reports success and writes the error body (or nothing), so
    # a zero-byte "font" lands on disk and ImGui then asserts on it.
    if(EXISTS "${_dest}")
      file(SIZE "${_dest}" _sz)
      if(_sz LESS 4096)
        message(STATUS "Discarding truncated font ${_name} (${_sz} bytes)")
        file(REMOVE "${_dest}")
      endif()
    endif()
    if(NOT EXISTS "${_dest}")
      message(STATUS "Downloading font ${_name} ...")
      file(DOWNLOAD "${_font_base}/${_name}" "${_dest}" TLS_VERIFY ON STATUS _st)
      list(GET _st 0 _code)
      set(_sz 0)
      if(EXISTS "${_dest}")
        file(SIZE "${_dest}" _sz)
      endif()
      if(NOT _code EQUAL 0 OR _sz LESS 4096)
        message(WARNING "Could not fetch ${_name}; falling back to system fonts.")
        file(REMOVE "${_dest}")
      endif()
    endif()
  endforeach()

  # stb_image_write: lets the apps save their own framebuffer to PNG. Capturing
  # via the OS screen grabber is unreliable (it captures whatever window happens
  # to be in front) and reads pixels that are none of this program's business.
  set(STBIW_HEADER "${CMAKE_SOURCE_DIR}/common/third_party/stb_image_write.h")
  if(NOT EXISTS "${STBIW_HEADER}")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/common/third_party")
    message(STATUS "Downloading stb_image_write.h ...")
    file(DOWNLOAD
      "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h"
      "${STBIW_HEADER}" TLS_VERIFY ON STATUS _stb_st)
    list(GET _stb_st 0 _stb_code)
    set(_stb_sz 0)
    if(EXISTS "${STBIW_HEADER}")
      file(SIZE "${STBIW_HEADER}" _stb_sz)
    endif()
    if(NOT _stb_code EQUAL 0 OR _stb_sz LESS 4096)
      message(WARNING "stb_image_write.h unavailable; --screenshot disabled.")
      file(REMOVE "${STBIW_HEADER}")
    endif()
  endif()
  if(EXISTS "${STBIW_HEADER}")
    add_compile_definitions(ELANORA_HAVE_STBIW)
  endif()

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

# ---------------------------------------------------------------------------
# cpp-httplib -- the web collector's host
# ---------------------------------------------------------------------------
# Header-only, so it costs a download and nothing else at link time.
#
# No TLS yet. Whether the phone needs HTTPS depends on whether Bluefy enforces
# secure context for Web Bluetooth, which is a two-minute probe; pulling in
# OpenSSL on MSVC before knowing would be a large cost for a maybe.
if(ELANORA_BUILD_APPS)
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(httplib)
endif()
