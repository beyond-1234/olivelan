# Olive - Non-Linear Video Editor
# Copyright (C) 2026 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set(FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/SourceHanSansSC-Regular.otf")

# Build-time automatic hook: download font if missing
if(NOT EXISTS "${FONT_FILE}")
  message(STATUS "Metro font not found locally; executing build-time download hook...")
  execute_process(
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/download_font.sh" "${FONT_FILE}"
    RESULT_VARIABLE FONT_DOWNLOAD_RES
  )
endif()

set(OLIVE_RESOURCES
  ${OLIVE_RESOURCES}
  ui/fonts/fonts.qrc
  PARENT_SCOPE
)
