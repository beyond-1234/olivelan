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

/**
 * Patch 0141: Linux audio routing is a scheduling policy, not a PortAudio call. Test it without
 * opening a device so CI and macOS builds cover the same cases as the Linux package.
 *
 * The important property is that an implicit `default` never pins the first raw hardware device
 * while a PipeWire/Pulse bridge is available. An explicit user choice must still win.
 */

#include "testutil.h"

#include <QVector>

#include "audio/audioroute.h"

namespace olive {

namespace {

AudioRouteDevice MakeDevice(int id, const QString &name, const QString &host_api,
                            int outputs, int inputs, bool is_default = false)
{
  AudioRouteDevice device;
  device.id = id;
  device.name = name;
  device.host_api = host_api;
  device.max_output_channels = outputs;
  device.max_input_channels = inputs;
  device.is_portaudio_default = is_default;
  return device;
}

QVector<AudioRouteDevice> TypicalDesktop()
{
  return {
    MakeDevice(0, QStringLiteral("HDA NVidia: HDMI 0"), QStringLiteral("ALSA"), 8, 0),
    MakeDevice(1, QStringLiteral("pipewire"), QStringLiteral("ALSA"), 64, 64),
    MakeDevice(2, QStringLiteral("pulse"), QStringLiteral("ALSA"), 32, 32),
    MakeDevice(3, QStringLiteral("default"), QStringLiteral("ALSA"), 64, 64, true)
  };
}

}

OLIVE_ADD_TEST(LinuxAudioRouteAutoPrefersPipeWire)
{
  const QVector<AudioRouteDevice> devices = TypicalDesktop();

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString()), 1);
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output,
                                      QStringLiteral("default")), 1);
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Input,
                                      QStringLiteral("auto")), 1);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteAutoPrefersPulseWithoutPipeWire)
{
  const QVector<AudioRouteDevice> devices = {
    MakeDevice(0, QStringLiteral("HDA NVidia: HDMI 0"), QStringLiteral("ALSA"), 8, 0),
    MakeDevice(5, QStringLiteral("pulse"), QStringLiteral("ALSA"), 32, 32, true)
  };

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString()), 5);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteAutoPrefersJackBeforeRawDefault)
{
  const QVector<AudioRouteDevice> devices = {
    MakeDevice(0, QStringLiteral("default"), QStringLiteral("ALSA"), 2, 2, true),
    MakeDevice(4, QStringLiteral("system"), QStringLiteral("JACK Audio Connection Kit"), 2, 2)
  };

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString()), 4);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteExplicitChoiceWins)
{
  const QVector<AudioRouteDevice> devices = TypicalDesktop();

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output,
                                      QStringLiteral("HDA NVidia: HDMI 0")), 0);
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output,
                                      QStringLiteral("PULSE")), 2);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteMissingExplicitFallsBackToBridge)
{
  const QVector<AudioRouteDevice> devices = TypicalDesktop();

  // The saved USB interface is gone. Do not leave the route on a stale hardware index.
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output,
                                      QStringLiteral("USB Audio Interface")), 1);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteInputSkipsOutputOnlyDevices)
{
  const QVector<AudioRouteDevice> devices = {
    MakeDevice(0, QStringLiteral("HDMI"), QStringLiteral("ALSA"), 8, 0),
    MakeDevice(1, QStringLiteral("pulse"), QStringLiteral("ALSA"), 32, 2),
  };

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Input, QString()), 1);
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output,
                                      QStringLiteral("HDMI")), 0);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteEmptyDeviceList)
{
  const QVector<AudioRouteDevice> devices;

  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString()), -1);
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Input,
                                      QStringLiteral("default")), -1);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(LinuxAudioRouteBlacklistSkipsFailedBridge)
{
  const QVector<AudioRouteDevice> devices = TypicalDesktop();

  // Normally selects pipewire (id 1)
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString()), 1);

  // If pipewire failed and is blacklisted, it must select pulse (id 2) instead of oscillating
  auto pipewire_failed = [](int id) { return id == 1; };
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString(), pipewire_failed), 2);

  // If both pipewire and pulse failed, it must select ALSA default (id 3)
  auto bridges_failed = [](int id) { return id == 1 || id == 2; };
  OLIVE_ASSERT_EQUAL(AudioRouteChoose(devices, AudioRouteDirection::Output, QString(), bridges_failed), 3);

  OLIVE_TEST_END;
}

} // namespace olive
