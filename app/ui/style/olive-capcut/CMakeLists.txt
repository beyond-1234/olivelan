# Olive - Non-Linear Video Editor
# Copyright (C) 2026 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# CapCut-style built-in theme. It intentionally ships without a duplicate icon
# directory: icon::Create() falls back to the Metro glyph set when a theme has no
# PNGs of its own. The stylesheet, palette and C++ design tokens are enough to
# give the whole workspace a distinct look without doubling the binary's image
# resources.

set(STYLE_RESOURCES
  palette.ini
  style.css
  tokens.json
)

set(QRC_BODY "")
foreach(STYLE_FILE ${STYLE_RESOURCES})
  string(APPEND QRC_BODY "<file>${STYLE_FILE}</file>\n")
  configure_file("${STYLE_FILE}" "${STYLE_FILE}" COPYONLY)
endforeach()

configure_file(res.qrc.in res.qrc @ONLY)

set(OLIVE_RESOURCES
  ${OLIVE_RESOURCES}
  ${CMAKE_CURRENT_BINARY_DIR}/res.qrc
  PARENT_SCOPE
)
