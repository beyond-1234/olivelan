/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

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

#ifndef AUDIOROUTE_H
#define AUDIOROUTE_H

#include <QString>
#include <QVector>

namespace olive {

/**
 * @brief A PortAudio device as far as routing policy is concerned.
 *
 * This deliberately contains no PortAudio types so the selection policy can be unit-tested
 * without opening an audio device. `id` is the PaDeviceIndex in production and an arbitrary
 * identifier in tests.
 */
struct AudioRouteDevice
{
  int id = -1;
  QString name;
  QString host_api;
  int max_output_channels = 0;
  int max_input_channels = 0;
  bool is_portaudio_default = false;
};

enum class AudioRouteDirection
{
  Output,
  Input
};

inline bool AudioRouteSupports(const AudioRouteDevice &device, AudioRouteDirection direction)
{
  return direction == AudioRouteDirection::Output
      ? device.max_output_channels > 0
      : device.max_input_channels > 0;
}

inline bool AudioRouteIsAutomatic(const QString &configured)
{
  return configured.isEmpty()
      || !configured.compare(QStringLiteral("default"), Qt::CaseInsensitive)
      || !configured.compare(QStringLiteral("auto"), Qt::CaseInsensitive);
}

inline int AudioRouteFindByName(const QVector<AudioRouteDevice> &devices,
                                const QString &name,
                                AudioRouteDirection direction)
{
  for (const AudioRouteDevice &device : devices) {
    if (AudioRouteSupports(device, direction) && !device.name.compare(name, Qt::CaseInsensitive)) {
      return device.id;
    }
  }

  return -1;
}

inline int AudioRouteFindByHostApi(const QVector<AudioRouteDevice> &devices,
                                   const QString &fragment,
                                   AudioRouteDirection direction)
{
  for (const AudioRouteDevice &device : devices) {
    if (AudioRouteSupports(device, direction)
        && device.host_api.contains(fragment, Qt::CaseInsensitive)) {
      return device.id;
    }
  }

  return -1;
}

/**
 * @brief Select a Linux output/input route.
 *
 * Explicit user choices win. For the implicit `default`/`auto` setting we prefer the host audio
 * server bridges over raw hardware:
 *
 *   PipeWire ALSA plugin -> Pulse/PipeWire compatibility -> JACK -> ALSA default -> PortAudio default
 *
 * The bridge devices are stable names exposed by ALSA plugins. Selecting one lets the host server
 * keep following its own default sink/source, which is important for PipeWire/PulseAudio hotplug.
 * Raw hardware remains the last resort so a machine without a server still gets audio.
 *
 * A configured device that is no longer present falls back through the same auto chain rather
 * than silently selecting the first HDMI output.
 */
inline int AudioRouteChoose(const QVector<AudioRouteDevice> &devices,
                            AudioRouteDirection direction,
                            const QString &configured)
{
  if (devices.isEmpty()) {
    return -1;
  }

  if (!AudioRouteIsAutomatic(configured)) {
    const int exact = AudioRouteFindByName(devices, configured, direction);
    if (exact >= 0) {
      return exact;
    }
  }

  for (const char *bridge : {"pipewire", "pulse"}) {
    const int id = AudioRouteFindByName(devices, QString::fromLatin1(bridge), direction);
    if (id >= 0) {
      return id;
    }
  }

  const int jack = AudioRouteFindByHostApi(devices, QStringLiteral("JACK"), direction);
  if (jack >= 0) {
    return jack;
  }

  const int alsa_default = AudioRouteFindByName(devices, QStringLiteral("default"), direction);
  if (alsa_default >= 0) {
    return alsa_default;
  }

  for (const AudioRouteDevice &device : devices) {
    if (AudioRouteSupports(device, direction) && device.is_portaudio_default) {
      return device.id;
    }
  }

  for (const AudioRouteDevice &device : devices) {
    if (AudioRouteSupports(device, direction)) {
      return device.id;
    }
  }

  return -1;
}

}

#endif // AUDIOROUTE_H
