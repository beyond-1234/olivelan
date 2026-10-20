# Olive - Non-Linear Video Editor
# Copyright (C) 2023 Olive Studios LLC
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

project(olive-editor VERSION 0.2.0 LANGUAGES C CXX)

option(BUILD_DOXYGEN "Build Doxygen documentation" OFF)
option(BUILD_TESTS "Build unit tests" OFF)
option(USE_WERROR "Error on compile warning" OFF)

# Backend switches: AUTO (default, detect-and-enable-if-present) or OFF (force
# disable). No "ON" state — for test distribution a missing dependency should
# silently disable the backend rather than abort the configure step.
set(OLIVE_ENABLE_CUDA_NVDEC "AUTO" CACHE STRING "Enable FFmpeg CUDA/NVDEC decoding (AUTO/OFF)")
set(OLIVE_ENABLE_VAAPI_DMABUF "AUTO" CACHE STRING "Enable Linux VA-API DMA-BUF decoding (AUTO/OFF)")
set(OLIVE_ENABLE_D3D11_INTEROP "AUTO" CACHE STRING "Enable Windows D3D11 interop decoding (AUTO/OFF)")
set_property(CACHE OLIVE_ENABLE_CUDA_NVDEC PROPERTY STRINGS AUTO OFF)
set_property(CACHE OLIVE_ENABLE_VAAPI_DMABUF PROPERTY STRINGS AUTO OFF)
set_property(CACHE OLIVE_ENABLE_D3D11_INTEROP PROPERTY STRINGS AUTO OFF)

# Normalize "ON" (legacy) to the AUTO path so a stale -D...=ON still works.
if(OLIVE_ENABLE_CUDA_NVDEC STREQUAL "ON")
  set(OLIVE_ENABLE_CUDA_NVDEC "AUTO")
endif()
if(OLIVE_ENABLE_VAAPI_DMABUF STREQUAL "ON")
  set(OLIVE_ENABLE_VAAPI_DMABUF "AUTO")
endif()
if(OLIVE_ENABLE_D3D11_INTEROP STREQUAL "ON")
  set(OLIVE_ENABLE_D3D11_INTEROP "AUTO")
endif()

if(OLIVE_ENABLE_CUDA_NVDEC STREQUAL "OFF")
  # Disabled
else()
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND OR CMAKE_CUDA_COMPILER)
    set(OLIVE_ENABLE_CUDA_NVDEC ON)
  else()
    set(OLIVE_ENABLE_CUDA_NVDEC OFF)
    message("   CUDA toolkit not found; CUDA/NVDEC backend will be disabled.")
  endif()
endif()

if(OLIVE_ENABLE_CUDA_NVDEC)
  if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 89)
  endif()
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  list(APPEND OLIVE_LIBRARIES CUDA::cudart)
endif()

# Platform-native decode backends. These are auto-enabled per platform: the
# dependency ships with the OS SDK, so no manual -D flag is needed.
#   - VA-API DMA-BUF : Linux only (needs libva + libva-drm headers)
#   - D3D11 interop  : Windows only (d3d11.h ships with Windows SDK)
#   - VideoToolbox   : macOS only (already gated by __APPLE__ in the source)
if(UNIX AND NOT APPLE)
  if(OLIVE_ENABLE_VAAPI_DMABUF STREQUAL "OFF")
    set(OLIVE_ENABLE_VAAPI_DMABUF OFF)
  else()
    find_package(PkgConfig)
    pkg_check_modules(LIBVA libva libva-drm)

    # Patch 0117: libva alone is not enough. vaapi_dmabuf_pipeline.cpp also includes
    # <EGL/egl.h> and links eglCreateImageKHR/glEGLImageTargetTexture2DOES; without the
    # EGL development package the backend used to be enabled and then failed to compile.
    find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
    find_library(EGL_LIBRARY NAMES EGL)

    if(LIBVA_FOUND AND EGL_INCLUDE_DIR AND EGL_LIBRARY)
      set(OLIVE_ENABLE_VAAPI_DMABUF ON)
      list(APPEND OLIVE_INCLUDE_DIRS ${LIBVA_INCLUDE_DIRS} ${EGL_INCLUDE_DIR})
      list(APPEND OLIVE_LIBRARIES ${LIBVA_LIBRARIES} ${EGL_LIBRARY})
    else()
      set(OLIVE_ENABLE_VAAPI_DMABUF OFF)
      if(NOT LIBVA_FOUND)
        message("   libva/libva-drm not found; VA-API DMA-BUF backend will be disabled.")
      elseif(NOT EGL_INCLUDE_DIR)
        message("   EGL headers (EGL/egl.h) not found; VA-API DMA-BUF backend will be disabled.")
      else()
        message("   EGL library not found; VA-API DMA-BUF backend will be disabled.")
      endif()
    endif()
  endif()
endif()

if(WIN32)
  if(OLIVE_ENABLE_D3D11_INTEROP STREQUAL "OFF")
    set(OLIVE_ENABLE_D3D11_INTEROP OFF)
  else()
    set(OLIVE_ENABLE_D3D11_INTEROP ON)
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Generates a compile_commands.json in the build dir, link that to the repo
# root to enrich your IDE with clangd language server protocol functionalities
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizers
add_library(olive-sanitizers INTERFACE)
include(cmake/Sanitizers.cmake)
enable_sanitizers(olive-sanitizers)
list(APPEND OLIVE_LIBRARIES olive-sanitizers)

# Set compiler options
if(MSVC)
  set(OLIVE_COMPILE_OPTIONS
    /wd4267
    /wd4244
    /experimental:external
    /external:anglebrackets
    /external:W0
    "$<$<CONFIG:RELEASE>:/O2>"
    "$<$<COMPILE_LANGUAGE:CXX>:/MP>"
  )
  if (USE_WERROR)
    list(APPEND OLIVE_COMPILE_OPTIONS "/WX")
  endif()
else()
  set(OLIVE_COMPILE_OPTIONS
    "$<$<CONFIG:RELEASE>:-O2>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wuninitialized>"
    "$<$<COMPILE_LANGUAGE:CXX>:-pedantic-errors>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wextra>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wshadow>"
  )
  if (USE_WERROR)
    list(APPEND OLIVE_COMPILE_OPTIONS "-Werror")

    # GCC 13 emits -Wmaybe-uninitialized from Qt's inline QRect code (reached via
    # app/widget/curvewidget/curveview.cpp) during optimisation. It is a third-party
    # false positive, and optimisation-time warnings are *not* covered by the
    # system-header suppression that -isystem installs for Qt. Keep it visible as a
    # warning, but do not let it fail the build.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND OLIVE_COMPILE_OPTIONS "-Wno-error=maybe-uninitialized")
    endif()
  endif()
endif()

set(OLIVE_DEFINITIONS -DQT_DEPRECATED_WARNINGS)
if(OLIVE_ENABLE_CUDA_NVDEC STREQUAL "ON")
  list(APPEND OLIVE_DEFINITIONS OLIVE_ENABLE_CUDA_NVDEC)
endif()
if(OLIVE_ENABLE_VAAPI_DMABUF STREQUAL "ON")
  list(APPEND OLIVE_DEFINITIONS OLIVE_ENABLE_VAAPI_DMABUF)
endif()
if(OLIVE_ENABLE_D3D11_INTEROP STREQUAL "ON")
  list(APPEND OLIVE_DEFINITIONS OLIVE_ENABLE_D3D11_INTEROP)
endif()
if (WIN32)
  list(APPEND OLIVE_DEFINITIONS -DUNICODE -D_UNICODE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Link OpenGL
if(UNIX AND NOT APPLE AND NOT DEFINED OpenGL_GL_PREFERENCE)
  set(OpenGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
list(APPEND OLIVE_LIBRARIES OpenGL::GL)

# OpenColorIO is optional and is OFF by default: the default build uses Olive's
# built-in colour engine (see app/render/colorprocessor.cpp and
# app/render/renderer.cpp) and therefore pulls in no VFX colour dependency.
# Configure with -DOLIVE_ENABLE_OCIO=ON to restore the full OpenColorIO backend,
# which is also the reference the colour-equivalence gate compares against.
#
# HONESTY NOTE (patch 0043): the built-in engine has NOT yet been measured against
# the OpenColorIO golden at the A-class gate (mean dE00 < 1.0, PSNR > 45 dB) - that
# requires a GPU session. This default therefore selects an engine whose
# equivalence to the OCIO backend is still unquantified. It is a deliberate flip of
# the default pending that measurement, not a claim that the two backends match.
option(OLIVE_ENABLE_OCIO "Enable the full OpenColorIO colour backend" OFF)

if(OLIVE_ENABLE_OCIO)
  find_package(OpenColorIO 2.1.1 REQUIRED)
  list(APPEND OLIVE_LIBRARIES ${OCIO_LIBRARIES})
  list(APPEND OLIVE_INCLUDE_DIRS ${OCIO_INCLUDE_DIRS})
  list(APPEND OLIVE_DEFINITIONS OLIVE_ENABLE_OCIO)
endif()

# Link Olive
list(APPEND OLIVE_LIBRARIES olivecore)
list(APPEND OLIVE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/ext/core/include)

# Link vendored LZ4 (used by the frame cache container)
list(APPEND OLIVE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4)

# Link Qt 6 (explicitly require Qt 6, minimum version 6.2 for LTS/C++17 compatibility, supporting distro-provided 6.4+)
set(QT_LIBRARIES
  Core
  Gui
  Widgets
  OpenGL
  OpenGLWidgets
  LinguistTools
  Concurrent
)
if (UNIX AND NOT APPLE)
  list(APPEND QT_LIBRARIES DBus)
endif()

find_package(Qt6 6.2 REQUIRED
  COMPONENTS
    ${QT_LIBRARIES}
  OPTIONAL_COMPONENTS
    Network
)

if (NOT Qt6Network_FOUND)
  message("   Qt6::Network module not found, crash reporting will be disabled.")
endif()

# Zip package import for .olivetheme is optional: it uses Qt's private
# QZipReader, which is only available when the Qt private development headers
# are installed. Directory and plain .qss imports work without it.
if (TARGET Qt6::CorePrivate)
  list(APPEND OLIVE_LIBRARIES Qt6::CorePrivate)
  list(APPEND OLIVE_DEFINITIONS OLIVE_HAVE_QZIPREADER)
  message(STATUS "Qt6 CorePrivate found - .olivetheme zip import enabled")
else()
  message(STATUS "Qt6 CorePrivate not found - .olivetheme zip import disabled")
endif()

list(APPEND OLIVE_LIBRARIES
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  Qt6::Concurrent
)

list(APPEND OLIVE_LIBRARIES
  KDAB::kddockwidgets
)

# Link FFmpeg
#
# 6.1 is the *real* floor, not the upstream-declared 3.0: this tree reads
# AVCodecParameters::coded_side_data, which was added in FFmpeg 6.1 (6.0 and earlier only had the
# deprecated AVStream::side_data).
find_package(FFMPEG 6.1 REQUIRED
  COMPONENTS
  avutil
  avcodec
  avformat
  avfilter
  swscale
  swresample
)
list(APPEND OLIVE_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
list(APPEND OLIVE_LIBRARIES
  FFMPEG::avutil
  FFMPEG::avcodec
  FFMPEG::avformat
  FFMPEG::avfilter
  FFMPEG::swscale
  FFMPEG::swresample
)

# Belt-and-braces floor check, independent of the version *string* format.
#
# FFMPEG_VERSION is read out of libavutil/ffversion.h and its shape varies by how FFmpeg was
# produced (release tarball "6.1", distro package "6.1.1-3ubuntu5", git snapshot
# "N-123456-gabcdef"), so the comparison above is a user-facing hint rather than a guarantee.
# Assert on the actual symbol the code depends on instead.
# NOTE: AVCodecParameters is declared in libavcodec/codec_par.h, *not* libavcodec/avcodec.h.
include(CheckStructHasMember)
set(CMAKE_REQUIRED_INCLUDES ${FFMPEG_INCLUDE_DIRS})
check_struct_has_member("AVCodecParameters" coded_side_data "libavcodec/codec_par.h"
                        OLIVE_HAVE_AVCODECPARAMETERS_CODED_SIDE_DATA)
unset(CMAKE_REQUIRED_INCLUDES)
if (NOT OLIVE_HAVE_AVCODECPARAMETERS_CODED_SIDE_DATA)
  message(FATAL_ERROR
    "FFmpeg >= 6.1 is required: AVCodecParameters::coded_side_data was not found "
    "(searched libavcodec/codec_par.h). This tree cannot be built against FFmpeg 6.0 or older.")
endif ()

# Link PortAudio
find_package(PortAudio REQUIRED)
set(CMAKE_REQUIRED_INCLUDES ${PORTAUDIO_INCLUDE_DIRS})
include(CheckIncludeFileCXX)
check_include_file_cxx( "pa_jack.h" PA_HAS_JACK)
if (PA_HAS_JACK)
  list(APPEND OLIVE_DEFINITIONS PA_HAS_JACK)
endif()

list(APPEND OLIVE_INCLUDE_DIRS ${PORTAUDIO_INCLUDE_DIRS})
list(APPEND OLIVE_LIBRARIES ${PORTAUDIO_LIBRARIES})

# Optional: Link OpenTimelineIO
find_package(OpenTimelineIO)
if (OpenTimelineIO_FOUND)
  list(APPEND OLIVE_DEFINITIONS USE_OTIO)
  list(APPEND OLIVE_INCLUDE_DIRS ${OTIO_INCLUDE_DIRS})
  list(APPEND OLIVE_LIBRARIES ${OTIO_LIBRARIES})
else()
  message("   OpenTimelineIO interchange will be disabled.")
endif()

# Optional: Link Google Crashpad
find_package(GoogleCrashpad)
if (GoogleCrashpad_FOUND)
  list(APPEND OLIVE_DEFINITIONS USE_CRASHPAD)
  list(APPEND OLIVE_INCLUDE_DIRS ${CRASHPAD_INCLUDE_DIRS})
  list(APPEND OLIVE_LIBRARIES ${CRASHPAD_LIBRARIES})
else()
  message("   Automatic crash reporting will be disabled.")

  if (APPLE)
    # Enables use of special functions for slider dragging, only linked if Crashpad isn't found
    # because Crashpad links it itself and will cause duplicate references if we also link it
    list(APPEND OLIVE_LIBRARIES "-framework ApplicationServices")
  endif()
endif()

if (WIN32)
  list(APPEND OLIVE_DEFINITIONS "-DUNICODE -D_UNICODE")
elseif (APPLE)
  list(APPEND OLIVE_LIBRARIES "-framework CoreAudio" "-framework CoreFoundation" "-framework IOKit" "-framework IOSurface" "-framework CoreVideo" "-framework OpenGL")
elseif(UNIX)
  list(APPEND OLIVE_LIBRARIES Qt6::DBus)
endif()

# Generate Git hash
set(PROJECT_LONG_VERSION ${PROJECT_VERSION})
if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  find_package(Git)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      OUTPUT_VARIABLE GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(PROJECT_LONG_VERSION ${PROJECT_VERSION}-${GIT_HASH})
  endif()
endif()

# Optional: Find Doxygen if requested
if(BUILD_DOXYGEN)
  find_package(Doxygen)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)
list(APPEND OLIVE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/ext)

add_subdirectory(ext)
add_subdirectory(app)

if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
