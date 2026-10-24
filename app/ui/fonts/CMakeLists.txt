# Olive - Non-Linear Video Editor
# Copyright (C) 2026 Olive Team
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set(FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/SourceHanSansSC-Regular.otf")

# The font is committed to the tree. This hook only exists for source snapshots
# that intentionally strip the 16 MB binary; it is opt-in so a normal configure
# never blocks on the network (and never writes the source tree unexpectedly).
option(OLIVE_FETCH_METRO_FONT
       "Download Source Han Sans SC at configure time when the bundled font is missing" OFF)

set(FONT_URL "https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf")
set(FONT_SHA256 "f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256")

if(NOT EXISTS "${FONT_FILE}" OR IS_DIRECTORY "${FONT_FILE}")
  if(OLIVE_FETCH_METRO_FONT)
    message(STATUS "Metro font missing; downloading Source Han Sans SC (checksum pinned)...")
    file(DOWNLOAD "${FONT_URL}" "${FONT_FILE}"
         EXPECTED_HASH "SHA256=${FONT_SHA256}"
         TLS_VERIFY ON
         STATUS FONT_DOWNLOAD_STATUS)

    list(GET FONT_DOWNLOAD_STATUS 0 FONT_DOWNLOAD_CODE)
    if(NOT FONT_DOWNLOAD_CODE EQUAL 0)
      list(GET FONT_DOWNLOAD_STATUS 1 FONT_DOWNLOAD_MESSAGE)
      file(REMOVE "${FONT_FILE}")
      message(FATAL_ERROR
        "Failed to download the Metro font: ${FONT_DOWNLOAD_MESSAGE}\n"
        "Provide app/ui/fonts/SourceHanSansSC-Regular.otf manually, or retry with "
        "-DOLIVE_FETCH_METRO_FONT=ON.")
    endif()
  else()
    message(WARNING
      "Metro font is missing: ${FONT_FILE}\n"
      "The embedded font resource will be omitted and the UI will use the platform font stack. "
      "Run scripts/download_metro_assets.sh or configure with -DOLIVE_FETCH_METRO_FONT=ON to fetch it.")
  endif()
endif()

if(EXISTS "${FONT_FILE}")
  set(OLIVE_RESOURCES
    ${OLIVE_RESOURCES}
    ui/fonts/fonts.qrc
    PARENT_SCOPE
  )
else()
  # Do not reference fonts.qrc: rcc fails the build when one of its <file> entries
  # does not exist. main.cpp already falls back to system CJK fonts when the
  # embedded font is absent (font_id == -1).
  message(WARNING "Omitting ui/fonts/fonts.qrc because the font file is unavailable.")
endif()
