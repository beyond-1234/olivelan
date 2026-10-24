/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2026 Olive Team

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

#ifndef THEMECAPABILITIES_H
#define THEMECAPABILITIES_H

#include <QString>
#include <QStringList>

namespace olive {

/**
 * @brief Declarative behavior/geometry options supplied by a theme manifest.
 *
 * These values are intentionally data-only. A theme cannot execute code; it
 * can only select from the workspace layouts and widget defaults implemented
 * by the application. Missing values keep the legacy Olive behavior, so old
 * external themes and the Metro/Dark/Light built-ins remain compatible.
 */
struct ThemeCapabilities {
  enum class WorkspaceLayout {
    kStandard,
    kThreeColumn
  };

  WorkspaceLayout workspace_layout{WorkspaceLayout::kStandard};

  bool show_tool_panel{true};
  bool show_node_panel{true};
  bool show_footage_viewer{true};
  bool empty_inspector_when_unselected{false};

  /** Preferred width in logical pixels, or 0 to use the layout default. */
  int project_panel_width{0};
  /** Preferred width in logical pixels, or 0 to use the layout default. */
  int inspector_panel_width{0};
  /** Timeline share of the workspace height (10-90), or 0 for the default. */
  int timeline_height_percent{0};

  /** "tree", "list", "icon", or empty to keep the current view. */
  QString project_view;
  /** Thumbnail size in logical pixels, or 0 for the default. */
  int project_icon_size{0};
  /** Thumbnail cell height / width ratio (0.5-1.5), or 0 for square cells. */
  double project_icon_height_ratio{0.0};

  /** Ordered UI font families; missing families fall back to the app default. */
  QStringList ui_font_families;

  /** Start from the active palette's colors before applying tokens.json. */
  bool inherit_palette_tokens{true};

  bool operator==(const ThemeCapabilities &other) const
  {
    return workspace_layout == other.workspace_layout
        && show_tool_panel == other.show_tool_panel
        && show_node_panel == other.show_node_panel
        && show_footage_viewer == other.show_footage_viewer
        && empty_inspector_when_unselected == other.empty_inspector_when_unselected
        && project_panel_width == other.project_panel_width
        && inspector_panel_width == other.inspector_panel_width
        && timeline_height_percent == other.timeline_height_percent
        && project_view == other.project_view
        && project_icon_size == other.project_icon_size
        && project_icon_height_ratio == other.project_icon_height_ratio
        && ui_font_families == other.ui_font_families
        && inherit_palette_tokens == other.inherit_palette_tokens;
  }

  bool operator!=(const ThemeCapabilities &other) const
  {
    return !(*this == other);
  }
};

} // namespace olive

#endif // THEMECAPABILITIES_H
