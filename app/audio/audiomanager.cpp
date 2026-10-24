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

#include "audiomanager.h"

#ifdef PA_HAS_JACK
#include <pa_jack.h>
#endif

#include <QApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#if defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "audio/audioroute.h"
#include "audio/portaudiosink.h"
#include "config/config.h"

namespace olive {

namespace {

constexpr int kTransportFadeMs = 5;

// Diagnostic counters for the audio callback.
std::atomic<quint64> g_output_callbacks{0};
std::atomic<quint64> g_output_underruns{0};
std::atomic<quint64> g_output_underrun_frames{0};
std::atomic<quint64> g_output_hw_underruns{0};

bool IsFadeFormat(SampleFormat fmt)
{
  return fmt == SampleFormat::S16 || fmt == SampleFormat::S32 || fmt == SampleFormat::F32;
}

float ReadSampleAsFloat(const char *data, SampleFormat fmt)
{
  switch (fmt) {
  case SampleFormat::S16:
  {
    int16_t v;
    memcpy(&v, data, sizeof(v));
    return static_cast<float>(v) / 32768.0f;
  }
  case SampleFormat::S32:
  {
    int32_t v;
    memcpy(&v, data, sizeof(v));
    return static_cast<float>(v) / 2147483648.0f;
  }
  case SampleFormat::F32:
  {
    float v;
    memcpy(&v, data, sizeof(v));
    return v;
  }
  case SampleFormat::INVALID:
  case SampleFormat::COUNT:
  case SampleFormat::U8:
  case SampleFormat::U8P:
  case SampleFormat::S16P:
  case SampleFormat::S32P:
  case SampleFormat::S64:
  case SampleFormat::S64P:
  case SampleFormat::F32P:
  case SampleFormat::F64:
  case SampleFormat::F64P:
    break;
  }

  return 0.0f;
}

void WriteSampleFromFloat(char *data, SampleFormat fmt, float v)
{
  v = std::clamp(v, -1.0f, 1.0f);

  switch (fmt) {
  case SampleFormat::S16:
  {
    int16_t s = static_cast<int16_t>(std::lround(v * 32767.0f));
    memcpy(data, &s, sizeof(s));
    break;
  }
  case SampleFormat::S32:
  {
    int32_t s = static_cast<int32_t>(std::llround(static_cast<double>(v) * 2147483647.0));
    memcpy(data, &s, sizeof(s));
    break;
  }
  case SampleFormat::F32:
  {
    memcpy(data, &v, sizeof(v));
    break;
  }
  case SampleFormat::INVALID:
  case SampleFormat::COUNT:
  case SampleFormat::U8:
  case SampleFormat::U8P:
  case SampleFormat::S16P:
  case SampleFormat::S32P:
  case SampleFormat::S64:
  case SampleFormat::S64P:
  case SampleFormat::F32P:
  case SampleFormat::F64:
  case SampleFormat::F64P:
    break;
  }
}

QString PortAudioDeviceName(PaDeviceIndex device)
{
  if (device == paNoDevice) {
    return QString();
  }

  const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
  return info ? QString::fromUtf8(info->name) : QString();
}

QString PortAudioHostApiName(PaDeviceIndex device)
{
  if (device == paNoDevice) {
    return QString();
  }

  const PaDeviceInfo *info = Pa_GetDeviceInfo(device);
  if (!info) {
    return QString();
  }

  const PaHostApiInfo *host = Pa_GetHostApiInfo(info->hostApi);
  return host ? QString::fromUtf8(host->name) : QString();
}

#if defined(__linux__)
QVector<AudioRouteDevice> BuildLinuxAudioCandidates(bool is_output)
{
  QVector<AudioRouteDevice> candidates;
  const PaDeviceIndex default_device = is_output ? Pa_GetDefaultOutputDevice()
                                                 : Pa_GetDefaultInputDevice();

  for (PaDeviceIndex i = 0, end = Pa_GetDeviceCount(); i < end; i++) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }

    AudioRouteDevice candidate;
    candidate.id = i;
    candidate.name = QString::fromUtf8(info->name);
    candidate.host_api = PortAudioHostApiName(i);
    candidate.max_output_channels = info->maxOutputChannels;
    candidate.max_input_channels = info->maxInputChannels;
    candidate.is_portaudio_default = (i == default_device);
    candidates.push_back(candidate);
  }

  return candidates;
}

PaDeviceIndex FindLinuxAudioDevice(bool is_output, const QString &configured,
                                   const std::function<bool(int)> &is_blacklisted = nullptr)
{
  const AudioRouteDirection direction = is_output ? AudioRouteDirection::Output
                                                  : AudioRouteDirection::Input;
  const QVector<AudioRouteDevice> candidates = BuildLinuxAudioCandidates(is_output);
  return static_cast<PaDeviceIndex>(
      AudioRouteChoose(candidates, direction, configured, is_blacklisted));
}
#endif

#if defined(__APPLE__)
#if defined(MAC_OS_VERSION_12_0)
constexpr AudioObjectPropertyElement kDefaultOutputPropertyElement =
    kAudioObjectPropertyElementMain;
#else
constexpr AudioObjectPropertyElement kDefaultOutputPropertyElement =
    kAudioObjectPropertyElementMaster;
#endif

bool QueryMacSystemDefaultOutputDevice(PaDeviceIndex* out_device, QString* out_name)
{
  if (!out_device) {
    return false;
  }

  *out_device = paNoDevice;
  if (out_name) {
    out_name->clear();
  }

  AudioObjectPropertyAddress address = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    kDefaultOutputPropertyElement
  };

  AudioDeviceID device_id = kAudioObjectUnknown;
  UInt32 size = sizeof(device_id);
  OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                               0, nullptr, &size, &device_id);
  if (status != noErr || device_id == kAudioObjectUnknown) {
    return false;
  }

  address.mSelector = kAudioDevicePropertyDeviceNameCFString;
  address.mScope = kAudioObjectPropertyScopeGlobal;
  address.mElement = kDefaultOutputPropertyElement;

  CFStringRef cf_name = nullptr;
  size = sizeof(cf_name);
  status = AudioObjectGetPropertyData(device_id, &address, 0, nullptr, &size, &cf_name);
  if (status != noErr || !cf_name) {
    return false;
  }

  QString name;
  const CFIndex max_size =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(cf_name), kCFStringEncodingUTF8) + 1;
  if (max_size > 0) {
    QByteArray utf8(static_cast<int>(max_size), '\0');
    if (CFStringGetCString(cf_name, utf8.data(), max_size, kCFStringEncodingUTF8)) {
      name = QString::fromUtf8(utf8.constData());
    }
  }
  CFRelease(cf_name);

  if (name.isEmpty()) {
    return false;
  }

  if (out_name) {
    *out_name = name;
  }

  const PaDeviceIndex cached_default = Pa_GetDefaultOutputDevice();
  PaDeviceIndex first_match = paNoDevice;
  for (PaDeviceIndex i = 0, end = Pa_GetDeviceCount(); i < end; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxOutputChannels <= 0) {
      continue;
    }

    if (name == QString::fromUtf8(info->name)) {
      if (i == cached_default) {
        *out_device = i;
        return true;
      }
      if (first_match == paNoDevice) {
        first_match = i;
      }
    }
  }

  *out_device = first_match;
  return true;
}
#endif

} // namespace

AudioManager* AudioManager::instance_ = nullptr;

void AudioManager::CreateInstance()
{
  instance_ = new AudioManager();
}

void AudioManager::DestroyInstance()
{
  delete instance_;
  instance_ = nullptr;
}

AudioManager *AudioManager::instance()
{
  return instance_;
}

void AudioManager::SetOutputNotifyInterval(int n)
{
  if (output_buffer_) {
    output_buffer_->set_notify_interval(n);
  }
}

int InputCallback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
  Q_UNUSED(output)
  Q_UNUSED(timeInfo)
  Q_UNUSED(statusFlags)

  FFmpegEncoder *f = static_cast<FFmpegEncoder*>(userData);

  AudioParams our_params = f->params().audio_params();
  our_params.set_format(f->params().audio_params().format().to_packed_equivalent());

  f->WriteAudioData(our_params, reinterpret_cast<const uint8_t**>(&input), frameCount);

  return paContinue;
}

bool AudioManager::PushToOutput(const AudioParams &params, const QByteArray &samples, QString *error,
                                bool fade_out_tail)
{
  if (!audio_sink_ || !audio_sink_->IsActive()) {
    SyncSystemOutputDevice();
  }

  if (output_device_ == paNoDevice) {
    if (error) *error = tr("No output device is set");
    return false;
  }

  if (!audio_sink_) {
    audio_sink_ = std::make_unique<PortAudioSink>();
  }

  if (output_params_ != params || !audio_sink_->IsActive()) {
    if (output_params_ != params || audio_sink_->current_device() != output_device_) {
      CloseOutputStream();

      output_params_ = params;

      auto audio_callback = [this](const void *input, void *output,
                                   unsigned long frame_count,
                                   uint32_t status_flags) -> int {
        Q_UNUSED(input)
        g_output_callbacks.fetch_add(1, std::memory_order_relaxed);

        if (status_flags & paOutputUnderflow) {
          g_output_hw_underruns.fetch_add(1, std::memory_order_relaxed);
        }

        qint64 max_read = frame_count * output_buffer_->bytes_per_frame();
        qint64 read_count = output_buffer_->read(reinterpret_cast<char*>(output), max_read);
        if (read_count < max_read) {
          memset(reinterpret_cast<uint8_t*>(output) + read_count, 0, max_read - read_count);
          g_output_underruns.fetch_add(1, std::memory_order_relaxed);
          g_output_underrun_frames.fetch_add(
              frame_count - (read_count / std::max(1, output_buffer_->bytes_per_frame())),
              std::memory_order_relaxed);
        }

        if (output && frame_count > 0) {
          UpdateOutputPeaks(output, frame_count);
        }

        return paContinue;
      };

      if (!audio_sink_->Open(output_params_, output_device_, audio_callback, error)) {
        return false;
      }

      output_device_ = audio_sink_->current_device();

      qInfo() << "[Audio] Opened output stream:" << output_params_.sample_rate() << "Hz,"
              << output_params_.channel_count() << "ch,"
              << QString::fromStdString(output_params_.format().to_string())
              << "| device:" << (output_device_ == paNoDevice ? "none" : Pa_GetDeviceInfo(output_device_)->name)
              << "| host API:" << PortAudioHostApiName(output_device_)
              << "| suggested latency:" << audio_sink_->suggested_latency() << "s";

      output_buffer_->set_bytes_per_frame(output_params_.samples_to_bytes(1));
    }
  }

  QByteArray processed;
  const QByteArray *payload = &samples;

  if (!samples.isEmpty() && (fade_in_next_ || fade_out_tail)) {
    processed = samples;

    if (fade_in_next_) {
      ApplyFadeIn(processed, TransportFadeFrames());
      fade_in_next_ = false;
    }

    if (fade_out_tail) {
      FadeOutTail(processed, TransportFadeFrames());
    }

    payload = &processed;
  }

  output_buffer_->write(*payload);

  if (!audio_sink_->IsActive()) {
    audio_sink_->Start();
  }

  return true;
}

void AudioManager::ClearBufferedOutput()
{
  if (!output_buffer_) {
    return;
  }

  if (output_buffer_->buffered_bytes() == 0) {
    const QByteArray fade = MakeFadeOut(TransportFadeFrames());
    if (!fade.isEmpty()) {
      output_buffer_->replace_buffer(fade);
    }

    output_buffer_->reset_last_frame();
    fade_in_next_ = true;
    return;
  }

  output_buffer_->modify_buffer([this](QByteArray &buffer) {
    const int frames = TransportFadeFrames();
    const int frame_bytes = output_buffer_->bytes_per_frame();

    if (buffer.isEmpty() || frames <= 0 || frame_bytes <= 0) {
      buffer.clear();
      return;
    }

    const int available = std::min(frames, static_cast<int>(buffer.size() / frame_bytes));
    if (available <= 0) {
      buffer.clear();
      return;
    }

    buffer.truncate(available * frame_bytes);
    FadeOutBuffer(buffer, available);
  });

  output_buffer_->reset_last_frame();
  fade_in_next_ = true;
}

int AudioManager::TransportFadeFrames() const
{
  if (output_params_.sample_rate() <= 0) {
    return 0;
  }

  return std::max(2, output_params_.sample_rate() * kTransportFadeMs / 1000);
}

QByteArray AudioManager::MakeFadeOut(int frames) const
{
  if (!output_buffer_ || frames < 2) {
    return QByteArray();
  }

  const QByteArray last = output_buffer_->last_frame();
  const int frame_bytes = output_buffer_->bytes_per_frame();
  const int channels = output_params_.channel_count();
  const SampleFormat fmt = output_params_.format();

  if (frame_bytes <= 0 || last.size() != frame_bytes || channels <= 0
      || !fmt.is_packed() || !IsFadeFormat(fmt)
      || frame_bytes != channels * fmt.byte_count()) {
    return QByteArray();
  }

  QByteArray fade(frames * frame_bytes, Qt::Uninitialized);
  char *out = fade.data();

  for (int i = 0; i < frames; ++i) {
    const float gain = 1.0f - static_cast<float>(i) / static_cast<float>(frames - 1);

    for (int ch = 0; ch < channels; ++ch) {
      const int offset = ch * fmt.byte_count();
      const float v = ReadSampleAsFloat(last.constData() + offset, fmt) * gain;
      WriteSampleFromFloat(out + i * frame_bytes + offset, fmt, v);
    }
  }

  return fade;
}

void AudioManager::ApplyFadeIn(QByteArray &samples, int frames) const
{
  if (!output_buffer_ || frames <= 0) {
    return;
  }

  const QByteArray last = output_buffer_->last_frame();
  const int frame_bytes = output_buffer_->bytes_per_frame();
  const int channels = output_params_.channel_count();
  const SampleFormat fmt = output_params_.format();

  if (frame_bytes <= 0 || channels <= 0 || !fmt.is_packed() || !IsFadeFormat(fmt)
      || frame_bytes != channels * fmt.byte_count()) {
    return;
  }

  frames = std::min(frames, static_cast<int>(samples.size() / frame_bytes));
  if (frames <= 0) {
    return;
  }

  const bool have_last = (last.size() == frame_bytes);
  char *out = samples.data();

  for (int i = 0; i < frames; ++i) {
    const float gain = static_cast<float>(i) / static_cast<float>(frames);

    for (int ch = 0; ch < channels; ++ch) {
      const int offset = ch * fmt.byte_count();
      const float old_v = have_last
          ? ReadSampleAsFloat(last.constData() + offset, fmt)
          : 0.0f;
      const float new_v = ReadSampleAsFloat(out + i * frame_bytes + offset, fmt);

      WriteSampleFromFloat(out + i * frame_bytes + offset, fmt,
                           old_v * (1.0f - gain) + new_v * gain);
    }
  }
}

void AudioManager::FadeOutBuffer(QByteArray &samples, int frames) const
{
  const int frame_bytes = output_buffer_ ? output_buffer_->bytes_per_frame() : 0;
  const int channels = output_params_.channel_count();
  const SampleFormat fmt = output_params_.format();

  if (frame_bytes <= 0 || channels <= 0 || !fmt.is_packed() || !IsFadeFormat(fmt)
      || frame_bytes != channels * fmt.byte_count() || samples.isEmpty()) {
    return;
  }

  frames = std::min(frames, static_cast<int>(samples.size() / frame_bytes));
  if (frames <= 0) {
    return;
  }

  char *data = samples.data();

  for (int i = 0; i < frames; ++i) {
    const float gain = (frames > 1)
        ? 1.0f - static_cast<float>(i) / static_cast<float>(frames - 1)
        : 0.0f;

    for (int ch = 0; ch < channels; ++ch) {
      const int offset = ch * fmt.byte_count();
      const float v = ReadSampleAsFloat(data + i * frame_bytes + offset, fmt) * gain;

      WriteSampleFromFloat(data + i * frame_bytes + offset, fmt, v);
    }
  }
}

void AudioManager::FadeOutTail(QByteArray &samples, int frames) const
{
  const int frame_bytes = output_buffer_ ? output_buffer_->bytes_per_frame() : 0;
  const int channels = output_params_.channel_count();
  const SampleFormat fmt = output_params_.format();

  if (frame_bytes <= 0 || channels <= 0 || !fmt.is_packed() || !IsFadeFormat(fmt)
      || frame_bytes != channels * fmt.byte_count() || samples.isEmpty()) {
    return;
  }

  const int available = static_cast<int>(samples.size() / frame_bytes);
  frames = std::min(frames, available);
  if (frames <= 0) {
    return;
  }

  const int start = available - frames;
  char *data = samples.data();

  for (int i = 0; i < frames; ++i) {
    const float gain = (frames > 1)
        ? 1.0f - static_cast<float>(i) / static_cast<float>(frames - 1)
        : 0.0f;

    for (int ch = 0; ch < channels; ++ch) {
      const int offset = ch * fmt.byte_count();
      const int sample_offset = (start + i) * frame_bytes + offset;
      const float v = ReadSampleAsFloat(data + sample_offset, fmt) * gain;

      WriteSampleFromFloat(data + sample_offset, fmt, v);
    }
  }
}

PaSampleFormat AudioManager::GetPortAudioSampleFormat(SampleFormat fmt)
{
  return PortAudioSink::ToPortAudioFormat(fmt);
}

void AudioManager::CloseOutputStream()
{
  if (audio_sink_) {
    audio_sink_->Close();
  }

  if (output_buffer_) {
    output_buffer_->clear();
    fade_in_next_ = true;
  }
}

void AudioManager::StopOutput()
{
  if (audio_sink_ && audio_sink_->IsActive() && output_buffer_) {
    const QByteArray fade = MakeFadeOut(TransportFadeFrames());

    if (!fade.isEmpty()) {
      output_buffer_->replace_buffer(fade);
      QElapsedTimer drain_timer;
      drain_timer.start();
      while (output_buffer_->buffered_bytes() > 0 && drain_timer.elapsed() < 25) {
        QThread::msleep(1);
      }
    }

    // Stop hardware stream safely: no DC pop, no ALSA underruns
    audio_sink_->Stop();

    output_buffer_->reset_last_frame();
    output_buffer_->clear();
    fade_in_next_ = true;
  } else if (output_buffer_) {
    output_buffer_->reset_last_frame();
    output_buffer_->clear();
    fade_in_next_ = true;
  }

  const quint64 callbacks = g_output_callbacks.exchange(0, std::memory_order_relaxed);
  const quint64 underruns = g_output_underruns.exchange(0, std::memory_order_relaxed);
  const quint64 underrun_frames = g_output_underrun_frames.exchange(0, std::memory_order_relaxed);
  const quint64 hw_underruns = g_output_hw_underruns.exchange(0, std::memory_order_relaxed);

  if (underruns > 0 || hw_underruns > 0) {
    qWarning() << "[Audio] Output underruns:"
               << "buffer-short" << underruns << "of" << callbacks << "callbacks"
               << "| missing frames" << underrun_frames
               << "| device-underflow flags" << hw_underruns;
  }

  for (int i = 0; i < kMaxMonitoredChannels; ++i) {
    output_peaks_[i].store(0.0f, std::memory_order_relaxed);
  }
}

bool AudioManager::RefreshPortAudioDevices()
{
  if (output_rescan_in_progress_) {
    return false;
  }

  if (input_stream_) {
    qWarning() << "[Audio] Cannot rebuild the device list while recording; stop recording first";
    return false;
  }

  output_rescan_in_progress_ = true;

  const QString previous_output_name = PortAudioDeviceName(output_device_);
  const QString previous_input_name = PortAudioDeviceName(input_device_);

  CloseOutputStream();

  Pa_Terminate();

  const PaError init_error = Pa_Initialize();
  if (init_error != paNoError) {
    output_device_ = paNoDevice;
    input_device_ = paNoDevice;
    output_rescan_in_progress_ = false;
    qWarning() << "[Audio] Failed to rebuild PortAudio device list:"
               << Pa_GetErrorText(init_error);
    return false;
  }

  PaDeviceIndex resolved_output = paNoDevice;

#if defined(__linux__)
  input_device_ = FindLinuxAudioDevice(false, previous_input_name);
  if (input_device_ == paNoDevice) {
    input_device_ = FindConfigDeviceByName(false);
  }

  auto blacklist_filter = [this](int id) {
    return audio_sink_ && audio_sink_->IsDeviceFailed(id);
  };
  resolved_output = FindLinuxAudioDevice(true, previous_output_name, blacklist_filter);
  if (resolved_output == paNoDevice) {
    resolved_output = FindConfigDeviceByName(true);
  }
#else
  input_device_ = previous_input_name.isEmpty()
      ? Pa_GetDefaultInputDevice()
      : FindDeviceByName(previous_input_name, false);

#if defined(__APPLE__)
  if (OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
    PaDeviceIndex mac_default_device = paNoDevice;
    QString mac_default_name;
    if (QueryMacSystemDefaultOutputDevice(&mac_default_device, &mac_default_name)) {
      resolved_output = mac_default_device;
    }
  }
#endif

  if (resolved_output == paNoDevice && !previous_output_name.isEmpty()) {
    resolved_output = FindDeviceByName(previous_output_name, true);
  }

  if (resolved_output == paNoDevice) {
    if (OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
      resolved_output = Pa_GetDefaultOutputDevice();
    } else {
      resolved_output = FindConfigDeviceByName(true);
    }
  }
#endif

  qInfo() << "[Audio] Rebuilt PortAudio device list"
          << "| output:" << (resolved_output == paNoDevice
                                 ? QStringLiteral("none")
                                 : PortAudioDeviceName(resolved_output))
          << "| input:" << (input_device_ == paNoDevice
                                ? QStringLiteral("none")
                                 : PortAudioDeviceName(input_device_));

  SetOutputDevice(resolved_output);
  SetInputDevice(input_device_);

  output_rescan_in_progress_ = false;
  return resolved_output != paNoDevice;
}

void AudioManager::SetOutputDevice(PaDeviceIndex device)
{
  const PaDeviceInfo *device_info = (device == paNoDevice) ? nullptr : Pa_GetDeviceInfo(device);
  if (device_info) {
    qInfo() << "Setting output audio device to" << device_info->name
            << "| host API:" << PortAudioHostApiName(device);
  } else {
    qInfo() << "No output device found";
    device = paNoDevice;
  }

  output_device_ = device;

  CloseOutputStream();

  emit OutputNotify();
  emit OutputNotify();
}

void AudioManager::SyncSystemOutputDevice()
{
  // Do not steal or interrupt audio devices while playback is actively running!
  if (audio_sink_ && audio_sink_->IsActive()) {
    return;
  }

#if defined(__linux__)
  const PaDeviceInfo *current_info =
      (output_device_ >= 0 && output_device_ < Pa_GetDeviceCount())
          ? Pa_GetDeviceInfo(output_device_)
          : nullptr;
  const bool invalid_output = (output_device_ == paNoDevice || !current_info
                               || current_info->maxOutputChannels <= 0);

  // Missing or channel-less input devices must NEVER trigger full sound system reconstruction!
  const PaDeviceInfo *current_input_info =
      (input_device_ >= 0 && input_device_ < Pa_GetDeviceCount())
          ? Pa_GetDeviceInfo(input_device_)
          : nullptr;
  if (input_device_ != paNoDevice && (!current_input_info || current_input_info->maxInputChannels <= 0)) {
    input_device_ = paNoDevice;
  }

  if (invalid_output
      && !output_rescan_in_progress_
      && (!output_rescan_timer_.isValid() || output_rescan_timer_.elapsed() >= 2000)) {
    output_rescan_timer_.restart();
    qWarning() << "[Audio] Current output device is no longer available; rebuilding the route";
    if (RefreshPortAudioDevices()) {
      return;
    }
  }

  const bool automatic_output = OLIVE_CONFIG("AudioOutputFollowSystem").toBool()
      || AudioRouteIsAutomatic(OLIVE_CONFIG_STR("AudioOutput").toString());
  if (automatic_output) {
    auto blacklist_filter = [this](int id) {
      return audio_sink_ && audio_sink_->IsDeviceFailed(id);
    };
    const PaDeviceIndex resolved = FindLinuxAudioDevice(true, QString(), blacklist_filter);
    if (resolved != paNoDevice && resolved != output_device_) {
      qInfo() << "[Audio] Linux automatic output route changed; following"
              << PortAudioDeviceName(resolved)
              << "| host API:" << PortAudioHostApiName(resolved);
      SetOutputDevice(resolved);
    }
  }

  if (!input_stream_
      && AudioRouteIsAutomatic(OLIVE_CONFIG_STR("AudioInput").toString())) {
    const PaDeviceIndex resolved_input = FindLinuxAudioDevice(false, QString());
    if (resolved_input != paNoDevice && resolved_input != input_device_) {
      qInfo() << "[Audio] Linux automatic input route changed; following"
              << PortAudioDeviceName(resolved_input)
              << "| host API:" << PortAudioHostApiName(resolved_input);
      SetInputDevice(resolved_input);
    }
  }
  return;

#else
  if (!OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
    return;
  }

  PaDeviceIndex default_device = Pa_GetDefaultOutputDevice();

#if defined(__APPLE__)
  static bool warned_unknown_device = false;
  PaDeviceIndex mac_default_device = paNoDevice;
  QString mac_default_name;
  if (QueryMacSystemDefaultOutputDevice(&mac_default_device, &mac_default_name)) {
    if (mac_default_device == paNoDevice) {
      if (!output_rescan_in_progress_
          && (!output_rescan_timer_.isValid() || output_rescan_timer_.elapsed() >= 2000)) {
        output_rescan_timer_.restart();
        qInfo() << "[Audio] System default output" << mac_default_name
                << "is not in PortAudio's device list; rebuilding it";
        if (RefreshPortAudioDevices()) {
          return;
        }
      }

      if (!warned_unknown_device) {
        warned_unknown_device = true;
        qWarning() << "[Audio] System default output" << mac_default_name
                   << "is still not in PortAudio's device list; will retry automatically";
      }
      return;
    }

    warned_unknown_device = false;
    default_device = mac_default_device;
  }
#endif

  if (default_device == output_device_) {
    return;
  }

  const PaDeviceInfo *device_info =
      (default_device == paNoDevice) ? nullptr : Pa_GetDeviceInfo(default_device);
  qInfo() << "[Audio] System default output changed; following"
          << (device_info ? device_info->name : "no device");
  SetOutputDevice(default_device);
#endif
}

void AudioManager::SetInputDevice(PaDeviceIndex device)
{
  if (device == paNoDevice) {
    qInfo() << "No input device found";
  } else {
    qInfo() << "Setting input audio device to" << Pa_GetDeviceInfo(device)->name
            << "| host API:" << PortAudioHostApiName(device);
  }

  input_device_ = device;
}

void AudioManager::HardReset()
{
  RefreshPortAudioDevices();
}

bool AudioManager::StartRecording(const EncodingParams &params, QString *error_str)
{
  if (input_device_ == paNoDevice) {
    return false;
  }

  input_encoder_ = new FFmpegEncoder(params);
  if (!input_encoder_->Open()) {
    qCritical() << "Failed to open encoder for recording";
    return false;
  }

  PaStreamParameters p = GetPortAudioParams(params.audio_params(), input_device_);

  PaError r = Pa_OpenStream(&input_stream_, &p, nullptr, params.audio_params().sample_rate(), paFramesPerBufferUnspecified, paNoFlag, InputCallback, input_encoder_);
  if (r == paNoError) {
    r = Pa_StartStream(input_stream_);
    if (r == paNoError) {
      return true;
    }
  }

  if (error_str) {
    *error_str = Pa_GetErrorText(r);
  }

  StopRecording();
  return false;
}

void AudioManager::StopRecording()
{
  if (input_stream_) {
    if (Pa_IsStreamActive(input_stream_)) {
      Pa_StopStream(input_stream_);
    }
    Pa_CloseStream(input_stream_);

    input_stream_ = nullptr;
  }

  if (input_encoder_) {
    input_encoder_->Close();
    delete input_encoder_;
    input_encoder_ = nullptr;
  }
}

PaDeviceIndex AudioManager::FindConfigDeviceByName(bool is_output_device)
{
  if (is_output_device && OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
#if defined(__linux__)
    const PaDeviceIndex resolved = FindLinuxAudioDevice(true, QString());
    if (resolved != paNoDevice) {
      qInfo() << "[Audio] Linux output route:"
              << PortAudioDeviceName(resolved)
              << "| requested: system default"
              << "| host API:" << PortAudioHostApiName(resolved);
      return resolved;
    }
#else
    return Pa_GetDefaultOutputDevice();
#endif
  }

  QString entry = is_output_device ? QStringLiteral("AudioOutput") : QStringLiteral("AudioInput");
  const QString configured = OLIVE_CONFIG_STR(entry).toString();

#if defined(__linux__)
  const PaDeviceIndex resolved = FindLinuxAudioDevice(is_output_device, configured);
  if (resolved != paNoDevice) {
    const QString device_name = PortAudioDeviceName(resolved);
    const QString host_api = PortAudioHostApiName(resolved);
    qInfo() << "[Audio] Linux" << (is_output_device ? "output" : "input") << "route:"
            << device_name
            << "| requested:" << (configured.isEmpty() ? "auto" : configured)
            << "| host API:" << host_api;

    if (is_output_device && AudioRouteIsAutomatic(configured)
        && device_name.compare(QStringLiteral("pipewire"), Qt::CaseInsensitive)
        && device_name.compare(QStringLiteral("pulse"), Qt::CaseInsensitive)
        && !host_api.contains(QStringLiteral("JACK"), Qt::CaseInsensitive)) {
      qWarning() << "[Audio] No PipeWire/Pulse audio bridge is available; using raw device"
                 << device_name
                 << "- install pipewire-alsa or libasound2-plugins for host-server routing";
    }

    return resolved;
  }
#endif

  return FindDeviceByName(configured, is_output_device);
}

PaDeviceIndex AudioManager::FindDeviceByName(const QString &s, bool is_output_device)
{
  if (!s.isEmpty()) {
    for (PaDeviceIndex i=0, end=Pa_GetDeviceCount(); i<end; i++) {
      const PaDeviceInfo *device = Pa_GetDeviceInfo(i);

      if (((is_output_device && device->maxOutputChannels) || (!is_output_device && device->maxInputChannels))
          && !s.compare(device->name)) {
        return i;
      }
    }
  }

  return is_output_device ? Pa_GetDefaultOutputDevice() : Pa_GetDefaultInputDevice();
}

PaStreamParameters AudioManager::GetPortAudioParams(const AudioParams &params, PaDeviceIndex device)
{
  PaStreamParameters p;

  p.channelCount = params.channel_count();
  p.device = device;
  p.hostApiSpecificStreamInfo = nullptr;
  p.sampleFormat = GetPortAudioSampleFormat(params.format());
  const PaDeviceInfo *info = (device == paNoDevice) ? nullptr : Pa_GetDeviceInfo(device);
  p.suggestedLatency = info ? std::max(info->defaultHighOutputLatency, 0.050) : 0.050;

  return p;
}

AudioManager::AudioManager() :
  output_device_(paNoDevice),
  output_buffer_(nullptr),
  output_follow_timer_(nullptr),
  output_rescan_in_progress_(false),
  fade_in_next_(true),
  input_device_(paNoDevice),
  input_stream_(nullptr),
  input_encoder_(nullptr)
{
  for (int i = 0; i < kMaxMonitoredChannels; ++i) {
    output_peaks_[i].store(0.0f, std::memory_order_relaxed);
  }

#ifdef PA_HAS_JACK
  PaJack_SetClientName("Olive");
#endif

  Pa_Initialize();

  audio_sink_ = std::make_unique<PortAudioSink>();

  output_buffer_ = new PreviewAudioDevice(this);
  output_buffer_->open(PreviewAudioDevice::ReadWrite);
  connect(output_buffer_, &PreviewAudioDevice::Notify, this, &AudioManager::OutputNotify);

  PaDeviceIndex output_device = FindConfigDeviceByName(true);
  PaDeviceIndex input_device = FindConfigDeviceByName(false);

  SetOutputDevice(output_device);
  SetInputDevice(input_device);

  output_follow_timer_ = new QTimer(this);
  output_follow_timer_->setInterval(250);
  connect(output_follow_timer_, &QTimer::timeout, this, &AudioManager::SyncSystemOutputDevice);
  output_follow_timer_->start();
}

AudioManager::~AudioManager()
{
  if (output_follow_timer_) {
    output_follow_timer_->stop();
  }

  CloseOutputStream();

  audio_sink_.reset();

  Pa_Terminate();
}

void AudioManager::UpdateOutputPeaks(const void *data, unsigned long frames)
{
  int channels = std::min(output_params_.channel_count(), kMaxMonitoredChannels);
  if (channels <= 0 || frames == 0 || !data) return;

  SampleFormat fmt = output_params_.format();
  if (fmt == SampleFormat::S16) {
    const int16_t *samples = reinterpret_cast<const int16_t*>(data);
    for (unsigned long f = 0; f < frames; ++f) {
      for (int ch = 0; ch < channels; ++ch) {
        float val = std::abs(static_cast<float>(samples[f * channels + ch])) / 32768.0f;
        float cur = output_peaks_[ch].load(std::memory_order_relaxed);
        while (val > cur && !output_peaks_[ch].compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
      }
    }
  } else if (fmt == SampleFormat::F32) {
    const float *samples = reinterpret_cast<const float*>(data);
    for (unsigned long f = 0; f < frames; ++f) {
      for (int ch = 0; ch < channels; ++ch) {
        float val = std::abs(samples[f * channels + ch]);
        float cur = output_peaks_[ch].load(std::memory_order_relaxed);
        while (val > cur && !output_peaks_[ch].compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
      }
    }
  } else if (fmt == SampleFormat::S32) {
    const int32_t *samples = reinterpret_cast<const int32_t*>(data);
    for (unsigned long f = 0; f < frames; ++f) {
      for (int ch = 0; ch < channels; ++ch) {
        float val = std::abs(static_cast<float>(samples[f * channels + ch])) / 2147483648.0f;
        float cur = output_peaks_[ch].load(std::memory_order_relaxed);
        while (val > cur && !output_peaks_[ch].compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
      }
    }
  }
}

float AudioManager::GetOutputPeak(int channel)
{
  if (channel < 0 || channel >= kMaxMonitoredChannels) {
    return 0.0f;
  }

  return output_peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
}

} // namespace olive
