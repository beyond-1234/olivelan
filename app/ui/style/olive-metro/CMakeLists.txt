# Olive - Non-Linear Video Editor
# Copyright (C) 2024 Olive Team
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

# NOTE (patch 0071a): until the Microsoft Fluent icon set is imported (patch 0071b), the Metro
# theme reuses the olive-dark PNG icon pack. It is copied into this theme's build directory so
# the resource layout (":/style/olive-metro/png/<name>.<size>.png") matches every other theme.
file(GLOB STYLE_RESOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/../olive-dark/png
     ${CMAKE_CURRENT_SOURCE_DIR}/../olive-dark/png/*.png)

set(QRC_BODY "")
foreach(ICON_FILE ${STYLE_RESOURCES})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../olive-dark/png/${ICON_FILE}" "png/${ICON_FILE}" COPYONLY)
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
