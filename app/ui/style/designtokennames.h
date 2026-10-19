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

#ifndef DESIGNTOKENNAMES_H
#define DESIGNTOKENNAMES_H

namespace olive {
namespace tokens {

// Stable token keys shared by QSS and custom-painted widgets. They are not
// tied to a specific theme; every theme may override their values.
constexpr const char *kTrackVideo = "color.track.video";
constexpr const char *kTrackAudio = "color.track.audio";
constexpr const char *kTrackSubtitle = "color.track.subtitle";
constexpr const char *kTrackSticker = "color.track.sticker";
constexpr const char *kTrackEffect = "color.track.effect";
constexpr const char *kPlayhead = "color.timeline.playhead";
constexpr const char *kKeyframe = "color.keyframe";

} // namespace tokens
} // namespace olive

#endif // DESIGNTOKENNAMES_H
