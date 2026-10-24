/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2024 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef METROSTYLECONFIG_H
#define METROSTYLECONFIG_H

#include <QColor>

namespace olive {
namespace metro {

/**
 * @brief Design tokens for the Metro / Fluent (Microsoft tile) style.
 *
 * This is the single source of truth for the Metro look, shared by:
 * - the theme's stylesheet (olive-metro/style.css) and palette (olive-metro/palette.ini), whose
 *   literal values mirror these tokens, and
 * - C++ code that draws custom widgets (timeline clips, node graph, project tiles) with QPainter,
 *   which QSS cannot reach.
 *
 * Only the *values* live here; the actual surfaces are painted by their owning widgets.
 */

/// @brief Identifier and user-visible name of the Metro theme (registered in StyleManager).
inline constexpr const char *kThemeName = "olive-metro";
inline constexpr const char *kThemeDisplayName = "Olive Metro";

/// @brief Geometry / stroke tokens. Every Metro surface is a hard-edged rectangle.
inline constexpr int kCornerRadius = 0;  ///< Strictly square corners.
inline constexpr int kBorderWidth = 1;   ///< 1px hairline border.

/**
 * @brief Colour tokens.
 *
 * NOTE: QColor is not a literal type (none of its constructors are constexpr), so unlike the
 * integer tokens above these are C++17 `inline const` variables rather than `constexpr`.
 */
inline const QColor kAccentColor{0x00, 0xC4, 0xCC};  ///< Modern teal accent.
inline const QColor kTileBg{0x23, 0x25, 0x2B};       ///< Tile / card fill (#23252B).
inline const QColor kBorderColor{0x2C, 0x2F, 0x37};  ///< Divider / hairline border (#2C2F37).
inline const QColor kCanvasBg{0x16, 0x17, 0x1A};     ///< Deep canvas background (#16171A).
inline const QColor kTrackVideo{0x30, 0x78, 0x5C};   ///< Video clip block.
inline const QColor kTrackAudio{0x36, 0x5E, 0x99};   ///< Audio clip block.

}
}

#endif // METROSTYLECONFIG_H
