# Olive - Non-Linear Video Editor
# Copyright (C) 2026 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# Patch 0108: Metro theme now uses its own dedicated Microsoft Fluent 1.5px flat icons.
file(GLOB STYLE_RESOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/png
     ${CMAKE_CURRENT_SOURCE_DIR}/png/*.png)

set(QRC_BODY "")
foreach(ICON_FILE ${STYLE_RESOURCES})
  configure_file("png/${ICON_FILE}" "png/${ICON_FILE}" COPYONLY)
  string(APPEND QRC_BODY "<file>png/${ICON_FILE}</file>\n")
endforeach()

foreach(EXTRA_FILE palette.ini style.css)
  string(APPEND QRC_BODY "<file>${EXTRA_FILE}</file>\n")
  configure_file("${EXTRA_FILE}" "${EXTRA_FILE}" COPYONLY)
endforeach()

configure_file(res.qrc.in res.qrc @ONLY)

set(OLIVE_RESOURCES
  ${OLIVE_RESOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/res.qrc
  PARENT_SCOPE
)
