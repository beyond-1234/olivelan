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

#ifndef CAPCUTSTYLECONFIG_H
#define CAPCUTSTYLECONFIG_H

namespace olive {
namespace capcut {

constexpr const char *kThemeName = "olive-capcut";
constexpr const char *kThemeDisplayName = "CapCut Studio";

// Design-token keys shared by the theme file and custom-painted widgets.
constexpr const char *kTokenTrackVideo = "color.track.video";
constexpr const char *kTokenTrackAudio = "color.track.audio";
constexpr const char *kTokenTrackSubtitle = "color.track.subtitle";
constexpr const char *kTokenTrackSticker = "color.track.sticker";
constexpr const char *kTokenTrackEffect = "color.track.effect";
constexpr const char *kTokenPlayhead = "color.timeline.playhead";
constexpr const char *kTokenKeyframe = "color.keyframe";

}
}

#endif // CAPCUTSTYLECONFIG_H
