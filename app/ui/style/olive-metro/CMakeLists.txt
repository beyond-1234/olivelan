# Olive - Non-Linear Video Editor
# Copyright (C) 2026 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set(METRO_ICON_GEN "${CMAKE_CURRENT_SOURCE_DIR}/../../../../scripts/generate_metro_icons.py")
set(DARK_PNG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../olive-dark/png")
set(METRO_PNG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/png")

# The generated PNGs are committed, so a normal build only packages them. The
# generator writes into the source tree; keep that behind an explicit opt-in
# instead of mutating the checkout on every configure.
option(OLIVE_REGENERATE_METRO_ICONS
       "Regenerate the Metro PNG assets at configure time (writes the source tree)" OFF)

if(OLIVE_REGENERATE_METRO_ICONS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  message(STATUS "Regenerating Metro icons with ${Python3_EXECUTABLE}...")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${METRO_ICON_GEN}" "${DARK_PNG_DIR}" "${METRO_PNG_DIR}"
    RESULT_VARIABLE ICON_GEN_RESULT
    OUTPUT_VARIABLE ICON_GEN_OUTPUT
    ERROR_VARIABLE ICON_GEN_ERROR
  )

  if(NOT ICON_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Metro icon generation failed (exit ${ICON_GEN_RESULT}):\n"
      "${ICON_GEN_OUTPUT}\n${ICON_GEN_ERROR}")
  endif()
endif()

# CONFIGURE_DEPENDS: adding a new icon (the generator can do that when the dark
# theme grows) must re-run configure instead of silently leaving it out of the qrc.
file(GLOB STYLE_RESOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/png
     ${CMAKE_CURRENT_SOURCE_DIR}/png/*.png)

if(NOT STYLE_RESOURCES)
  message(FATAL_ERROR
    "No Metro PNG assets in ${METRO_PNG_DIR}. "
    "Run scripts/generate_metro_icons.py or configure with -DOLIVE_REGENERATE_METRO_ICONS=ON.")
endif()

set(QRC_BODY "")
foreach(ICON_FILE ${STYLE_RESOURCES})
  configure_file("png/${ICON_FILE}" "png/${ICON_FILE}" COPYONLY)
  string(APPEND QRC_BODY "<file>png/${ICON_FILE}</file>\n")
endforeach()

foreach(EXTRA_FILE manifest.json palette.ini style.css)
  string(APPEND QRC_BODY "<file>${EXTRA_FILE}</file>\n")
  configure_file("${EXTRA_FILE}" "${EXTRA_FILE}" COPYONLY)
endforeach()

configure_file(res.qrc.in res.qrc @ONLY)

set(OLIVE_RESOURCES
  ${OLIVE_RESOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/res.qrc
  PARENT_SCOPE
)
