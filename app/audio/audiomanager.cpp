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

#include "config/config.h"
#include "audio/audioroute.h"

namespace olive {

namespace {

constexpr int kTransportFadeMs = 5;

// Diagnostic counters for the PortAudio callback. These are deliberately lock-free because the
// callback runs on CoreAudio's real-time thread.
std::atomic<quint64> g_output_callbacks{0};
std::atomic<quint64> g_output_underruns{0};
std::atomic<quint64> g_output_underrun_frames{0};
std::atomic<quint64> g_output_hw_underruns{0};
std::atomic<bool> g_output_stopping{false};
std::atomic<bool> g_output_paused{false};

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

PaDeviceIndex FindLinuxAudioDevice(bool is_output, const QString &configured)
{
  const AudioRouteDirection direction = is_output ? AudioRouteDirection::Output
                                                  : AudioRouteDirection::Input;
  const QVector<AudioRouteDevice> candidates = BuildLinuxAudioCandidates(is_output);
  return static_cast<PaDeviceIndex>(
      AudioRouteChoose(candidates, direction, configured));
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

/**
 * @brief Query CoreAudio's current default output and map it to PortAudio.
 *
 * Pa_GetDefaultOutputDevice() cannot be used for live following: PortAudio's CoreAudio
 * backend reads kAudioHardwarePropertyDefaultOutputDevice once during Pa_Initialize() and
 * caches the resulting index. We therefore query CoreAudio directly, then match the device
 * name against PortAudio's snapshot.
 *
 * @return True if CoreAudio answered. @p out_device is then either the matching PortAudio
 *         index or paNoDevice when the device is missing from PortAudio's current snapshot.
 */
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

  address.mSelector = kAudioObjectPropertyName;
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

  // The default may be a device connected after PortAudio built its snapshot. Returning
  // paNoDevice lets the caller rebuild the device list and retry.
  *out_device = first_match;
  return true;
}
#endif

} // namespace

AudioManager* AudioManager::instance_ = nullptr;

void AudioManager::CreateInstance()
{
  if (instance_ == nullptr) {
    instance_ = new AudioManager();
  }
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
  output_buffer_->set_notify_interval(n);
}

int OutputCallback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
  Q_UNUSED(input)
  Q_UNUSED(timeInfo)

  PreviewAudioDevice *device = static_cast<PreviewAudioDevice*>(userData);

  g_output_callbacks.fetch_add(1, std::memory_order_relaxed);

  if ((statusFlags & paOutputUnderflow)
      && !g_output_stopping.load(std::memory_order_relaxed)
      && !g_output_paused.load(std::memory_order_relaxed)) {
    g_output_hw_underruns.fetch_add(1, std::memory_order_relaxed);
  }

  qint64 max_read = frameCount * device->bytes_per_frame();
  qint64 read_count = device->read(reinterpret_cast<char*>(output), max_read);
  if (read_count < max_read) {
    memset(reinterpret_cast<uint8_t*>(output) + read_count, 0, max_read - read_count);

    // During an intentional stop/pause the tail is expected to drain through zero; that is not a
    // playback underrun and would otherwise be reported as a dropout.
    if (!g_output_stopping.load(std::memory_order_relaxed)
        && !g_output_paused.load(std::memory_order_relaxed)) {
      g_output_underruns.fetch_add(1, std::memory_order_relaxed);
      g_output_underrun_frames.fetch_add(frameCount - (read_count / std::max(1, device->bytes_per_frame())),
                                         std::memory_order_relaxed);
    }
  }

  if ((!g_output_paused.load(std::memory_order_relaxed) || read_count > 0)
      && output && frameCount > 0) {
    AudioManager::instance()->UpdateOutputPeaks(output, frameCount);
  }

  return paContinue;
}

int InputCallback(const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
  FFmpegEncoder *f = static_cast<FFmpegEncoder*>(userData);

  AudioParams our_params = f->params().audio_params();
  our_params.set_format(f->params().audio_params().format().to_packed_equivalent());

  f->WriteAudioData(our_params, reinterpret_cast<const uint8_t**>(&input), frameCount);

  return paContinue;
}

bool AudioManager::PushToOutput(const AudioParams &params, const QByteArray &samples, QString *error,
                                bool fade_out_tail)
{
  // Re-check the route when a stream is about to be created. While a stream is already running,
  // the timer handles route changes; querying CoreAudio for every audio buffer is unnecessary.
  if (output_stream_ == nullptr) {
    SyncSystemOutputDevice();
  }

  if (output_device_ == paNoDevice) {
    if (error) *error = tr("No output device is set");
    return false;
  }

  if (output_params_ != params || output_stream_ == nullptr) {
    // Close with the old params still in place so a transport fade (if the stream is active) is
    // generated with the format the device is actually using.
    CloseOutputStream();

    output_params_ = params;

    PaStreamParameters p = GetPortAudioParams(params, output_device_);

    const PaError format_support =
        Pa_IsFormatSupported(nullptr, &p, output_params_.sample_rate());
    if (format_support != paNoError) {
      qWarning() << "[Audio] Output format may not be natively supported by"
                 << PortAudioDeviceName(output_device_) << ":"
                 << Pa_GetErrorText(format_support)
                 << "- the host audio server will convert if possible";
    }

    PaError r = Pa_OpenStream(&output_stream_, nullptr, &p, output_params_.sample_rate(), paFramesPerBufferUnspecified, paNoFlag, OutputCallback, output_buffer_);

#if defined(__linux__)
    if (r != paNoError && output_device_ != Pa_GetDefaultOutputDevice()) {
      // A bridge device can be enumerated while its server is restarting, or an explicitly
      // selected USB/HDMI device can disappear between the poll and the open. Try PortAudio's
      // default route once rather than leaving playback permanently silent.
      const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
      if (fallback != paNoDevice && fallback != output_device_) {
        const PaStreamParameters fallback_params = GetPortAudioParams(params, fallback);
        const PaError fallback_result =
            Pa_OpenStream(&output_stream_, nullptr, &fallback_params,
                          output_params_.sample_rate(), paFramesPerBufferUnspecified,
                          paNoFlag, OutputCallback, output_buffer_);
        if (fallback_result == paNoError) {
          qWarning() << "[Audio] Failed to open" << PortAudioDeviceName(output_device_)
                     << "(" << Pa_GetErrorText(r) << "); falling back to"
                     << PortAudioDeviceName(fallback)
                     << "| host API:" << PortAudioHostApiName(fallback);
          output_device_ = fallback;
          r = paNoError;
        }
      }
    }
#endif

    if (r != paNoError) {
      // Unhandled error
      //qCritical() << "Failed to open output stream:" << Pa_GetErrorText(r);
      if (error) *error = Pa_GetErrorText(r);
      return false;
    }

    qInfo() << "[Audio] Opened output stream:" << output_params_.sample_rate() << "Hz,"
            << output_params_.channel_count() << "ch,"
            << QString::fromStdString(output_params_.format().to_string())
            << "| device:" << (output_device_ == paNoDevice ? "none" : Pa_GetDeviceInfo(output_device_)->name)
            << "| host API:" << PortAudioHostApiName(output_device_)
            << "| suggested latency:" << p.suggestedLatency << "s";

    output_buffer_->set_bytes_per_frame(output_params_.samples_to_bytes(1));

    // Start the device while it is still silent. Starting it at the first audible sample makes
    // CoreAudio produce a small click on some devices even when the waveform itself starts at
    // zero; priming with silence keeps the stream alive across pause/play.
    g_output_paused.store(true, std::memory_order_relaxed);
    if (!Pa_IsStreamActive(output_stream_)) {
      Pa_StartStream(output_stream_);
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
      // Scrub chunks are the only buffers that may be followed directly by silence (when the user
      // stops moving the playhead), so give their end the same short ramp the transport uses.
      FadeOutTail(processed, TransportFadeFrames());
    }

    payload = &processed;
  }

  output_buffer_->write(*payload);

  // A scrub chunk may be the last thing the user hears before the playhead stops moving. Keep the
  // stream in the paused bookkeeping state after it drains so the callback's silence is not
  // reported as a playback underrun. Normal playback clears this flag and is monitored normally.
  g_output_paused.store(fade_out_tail, std::memory_order_relaxed);

  if (!Pa_IsStreamActive(output_stream_)) {
    Pa_StartStream(output_stream_);
  }

  return true;
}

void AudioManager::ClearBufferedOutput()
{
  if (!output_buffer_) {
    return;
  }

  // If the queue is already empty the last sample is already in the device buffer. Emit a short
  // ramp from that sample before the next scrub chunk so the device still gets a clean edge.
  if (output_buffer_->buffered_bytes() == 0) {
    const QByteArray fade = MakeFadeOut(TransportFadeFrames());
    if (!fade.isEmpty()) {
      output_buffer_->replace_buffer(fade);
    }

    output_buffer_->reset_last_frame();
    fade_in_next_ = true;
    return;
  }

  // Scrubbing clears the queue on every mouse event. Discarding it outright leaves the device
  // with an abrupt edge; instead keep only a few milliseconds of the material that is about to
  // play, ramp that to zero, and let the next chunk fade in behind it.
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

  // The retained prefix ends at zero, so the next chunk must start from zero rather than
  // crossfading from whatever was last read by the callback.
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
    // The first frame is the last frame actually read by the callback, so the fade connects
    // seamlessly to whatever the device is currently playing out.
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
    // Crossfade from the previous sample to the new material. On a fresh start the previous
    // sample is zero, which produces a normal fade-in.
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
    // Ramp from unity at the start of the tail to zero on the final frame, leaving the audio
    // before the tail untouched.
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
  switch (fmt) {
  case SampleFormat::U8:
  case SampleFormat::U8P:
    return paUInt8;
  case SampleFormat::S16:
  case SampleFormat::S16P:
    return paInt16;
  case SampleFormat::S32:
  case SampleFormat::S32P:
    return paInt32;
  case SampleFormat::F32:
  case SampleFormat::F32P:
    return paFloat32;
  case SampleFormat::S64:
  case SampleFormat::S64P:
  case SampleFormat::F64:
  case SampleFormat::F64P:
  case SampleFormat::INVALID:
  case SampleFormat::COUNT:
    break;
  }

  return 0;
}

void AudioManager::CloseOutputStream()
{
  if (output_stream_) {
    if (Pa_IsStreamActive(output_stream_)) {
      StopOutput();

      // Unlike a normal pause, closing the device is a one-time operation: drain the fade and
      // silence before shutting the stream down, so device changes/quit are clean too.
      if (Pa_IsStreamActive(output_stream_)) {
        Pa_StopStream(output_stream_);
      }
    }

    g_output_paused.store(true, std::memory_order_relaxed);
    Pa_CloseStream(output_stream_);
    output_stream_ = nullptr;
  }

  if (output_buffer_) {
    output_buffer_->clear();
    fade_in_next_ = true;
  }
}

void AudioManager::StopOutput()
{
  // The stream stays open while paused and is fed silence, so mark it paused up front. This also
  // keeps the intentional fade/drain from being reported as an underrun.
  g_output_paused.store(true, std::memory_order_relaxed);

  if (output_stream_ && output_buffer_) {
    if (Pa_IsStreamActive(output_stream_)) {
      const QByteArray fade = MakeFadeOut(TransportFadeFrames());

      g_output_stopping.store(true, std::memory_order_relaxed);

      // Replace queued material atomically with a short ramp from the last sample the callback
      // read down to zero. Cutting the stream at an arbitrary sample is what causes the
      // click/pop on CoreAudio when pausing or stopping. Replacing (rather than clearing then
      // writing) prevents the callback from observing an empty buffer in between.
      output_buffer_->replace_buffer(fade);

      if (!fade.isEmpty()) {
        // Let the callback pull the ramp into the device buffer. After the ramp the device keeps
        // running and simply receives silence; keeping the stream alive avoids the CoreAudio
        // stop/start transient that remains audible even with a perfectly ramped waveform.
        QElapsedTimer drain_timer;
        drain_timer.start();
        while (output_buffer_->buffered_bytes() > 0 && drain_timer.elapsed() < 100) {
          QThread::msleep(1);
        }

        if (output_buffer_->buffered_bytes() > 0) {
          // The callback never consumed the ramp (stalled or disconnected device); do not leave
          // the stream in a half-running state.
          Pa_AbortStream(output_stream_);
        }
      }

      g_output_paused.store(true, std::memory_order_relaxed);
      g_output_stopping.store(false, std::memory_order_relaxed);
    }

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
    // Pa_Terminate() invalidates the recording stream. Do not interrupt an active take; the
    // caller will retry after recording stops (or the user can refresh in Preferences).
    qWarning() << "[Audio] Cannot rebuild the device list while recording; stop recording first";
    return false;
  }

  output_rescan_in_progress_ = true;

  const QString previous_output_name = PortAudioDeviceName(output_device_);
  const QString previous_input_name = PortAudioDeviceName(input_device_);

  // The stream must be gone before Pa_Terminate(). The custom ring buffer is independent of
  // PortAudio and survives the reinitialization.
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
  // Every PaDeviceIndex is invalid after reinitialization, so remap by name through the same
  // Linux policy. If the old explicit device disappeared, this automatically falls back to the
  // host bridge instead of leaving the app on a dead hardware index.
  input_device_ = FindLinuxAudioDevice(false, previous_input_name);
  if (input_device_ == paNoDevice) {
    input_device_ = FindConfigDeviceByName(false);
  }

  resolved_output = FindLinuxAudioDevice(true, previous_output_name);
  if (resolved_output == paNoDevice) {
    resolved_output = FindConfigDeviceByName(true);
  }
#else
  // Every PaDeviceIndex is invalid after reinitialization, so remap by name.
  input_device_ = previous_input_name.isEmpty()
      ? Pa_GetDefaultInputDevice()
      : FindDeviceByName(previous_input_name, false);

#if defined(__APPLE__)
  // Prefer CoreAudio's live default. If it still is not exposed by PortAudio, fall through and
  // keep the previous route rather than silently switching to an unrelated device.
  if (OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
    PaDeviceIndex mac_default_device = paNoDevice;
    QString mac_default_name;
    if (QueryMacSystemDefaultOutputDevice(&mac_default_device, &mac_default_name)) {
      resolved_output = mac_default_device;
    }
  }
#endif

  if (resolved_output == paNoDevice && !previous_output_name.isEmpty()) {
    // The live default is still absent from PortAudio. Keep the previous route until the next
    // poll rather than jumping to an unrelated device.
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

  // Closing the stream stops the callback that normally asks the viewer for another audio
  // buffer. Request the normal two-chunk prequeue so playback can reopen the stream on the new
  // device without waiting for a callback notification that cannot arrive while it is closed.
  emit OutputNotify();
  emit OutputNotify();
}

void AudioManager::SyncSystemOutputDevice()
{
#if defined(__linux__)
  // PortAudio caches device indices at Pa_Initialize(). An unplugged USB interface, a stopped
  // PipeWire/Pulse server, or a container recreated without the bridge can invalidate the index.
  // Validate it even when "follow system" is off; otherwise playback would keep writing to a
  // stream that can never reopen.
  const PaDeviceInfo *current_info =
      (output_device_ >= 0 && output_device_ < Pa_GetDeviceCount())
          ? Pa_GetDeviceInfo(output_device_)
          : nullptr;
  const bool invalid_output = (output_device_ == paNoDevice || !current_info
                               || current_info->maxOutputChannels <= 0);

  const PaDeviceInfo *current_input_info =
      (input_device_ >= 0 && input_device_ < Pa_GetDeviceCount())
          ? Pa_GetDeviceInfo(input_device_)
          : nullptr;
  const bool invalid_input = (input_device_ != paNoDevice
                              && (!current_input_info
                                  || current_input_info->maxInputChannels <= 0));

  if ((invalid_output || invalid_input)
      && !output_rescan_in_progress_
      && (!output_rescan_timer_.isValid() || output_rescan_timer_.elapsed() >= 2000)) {
    output_rescan_timer_.restart();
    qWarning() << "[Audio] Current"
               << (invalid_output ? "output" : "input")
               << "device is no longer available; rebuilding the route";
    if (RefreshPortAudioDevices()) {
      return;
    }
  }

  // The implicit/default setting is a request for the best host route, not a one-time choice.
  // Keep resolving it so a server that starts after Olive (or a bridge that reappears after a
  // restart) takes over from a temporary HDMI fallback. An explicitly selected hardware device
  // is never stolen.
  const bool automatic_output = OLIVE_CONFIG("AudioOutputFollowSystem").toBool()
      || AudioRouteIsAutomatic(OLIVE_CONFIG_STR("AudioOutput").toString());
  if (automatic_output) {
    const PaDeviceIndex resolved = FindLinuxAudioDevice(true, QString());
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
  // The system default route can change at runtime (Control Center, Bluetooth connection,
  // unplugging an already-known device, etc.). PortAudio snapshots its device list at
  // Pa_Initialize(), so rebuild it when the new default is not in the current snapshot.
  if (!OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
    return;
  }

  PaDeviceIndex default_device = Pa_GetDefaultOutputDevice();

#if defined(__APPLE__)
  static bool warned_unknown_device = false;
  // PortAudio caches its default device at Pa_Initialize(), so query CoreAudio directly and
  // map the result back to PortAudio's device list. This keeps the existing PortAudio stream
  // handling while making route changes visible at runtime.
  PaDeviceIndex mac_default_device = paNoDevice;
  QString mac_default_name;
  if (QueryMacSystemDefaultOutputDevice(&mac_default_device, &mac_default_name)) {
    if (mac_default_device == paNoDevice) {
      // A device that was not present when PortAudio initialized (e.g. Bluetooth headphones
      // connected after Olive started) has no valid index in the snapshot. Rebuild the list and
      // retry. Rate-limit this because Pa_Terminate()/Pa_Initialize() is relatively expensive and
      // the OS may expose a new route a moment after it becomes the default.
      if (!output_rescan_in_progress_
          && (!output_rescan_timer_.isValid() || output_rescan_timer_.elapsed() >= 2000)) {
        output_rescan_timer_.restart();
        qInfo() << "[Audio] System default output" << mac_default_name
                << "is not in PortAudio's device list; rebuilding it";
        if (RefreshPortAudioDevices()) {
          // RefreshPortAudioDevices() has already remapped and reopened the output.
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
    //const PaStreamInfo* info = Pa_GetStreamInfo(input_stream_);
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
  // On macOS the system default output follows the user's active route (Bluetooth headphones,
  // USB interface, etc.). Pinning the device name saved on first run keeps sending audio to the
  // built-in speakers after the user has switched the system output. Follow the OS default unless
  // the user explicitly turned this off in Preferences.
  if (is_output_device && OLIVE_CONFIG("AudioOutputFollowSystem").toBool()) {
#if defined(__linux__)
    // The `pipewire`/`pulse` bridge follows the host server's default sink, unlike
    // Pa_GetDefaultOutputDevice() which can be the first raw ALSA/HDMI device.
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
  // PortAudio's ALSA backend enumerates hardware devices in addition to the
  // ALSA bridge plugins. In a distrobox/container the hardware default is often
  // an unconnected HDMI output, while the host audio server is exposed through
  // `pulse` (or `pipewire` on hosts that install the native plugin). Treat the
  // implicit `default` setting as "the host route" rather than pinning the
  // first HDMI device. Explicit device names still win.
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
  p.suggestedLatency = Pa_GetDeviceInfo(device)->defaultLowOutputLatency;

  return p;
}

AudioManager::AudioManager() :
  output_stream_(nullptr),
  output_buffer_(nullptr),
  output_follow_timer_(nullptr),
  output_rescan_in_progress_(false),
  fade_in_next_(true),
  input_stream_(nullptr),
  input_encoder_(nullptr)
{
  for (int i = 0; i < kMaxMonitoredChannels; ++i) {
    output_peaks_[i].store(0.0f, std::memory_order_relaxed);
  }

#ifdef PA_HAS_JACK
  // PortAudio doesn't do a strcpy, so we need a const char that's readily accessible (i.e. not
  // a QString converted to UTF-8)
  PaJack_SetClientName("Olive");
#endif

  Pa_Initialize();

  // Get device from config
  PaDeviceIndex output_device = FindConfigDeviceByName(true);
  PaDeviceIndex input_device = FindConfigDeviceByName(false);

  SetOutputDevice(output_device);
  SetInputDevice(input_device);

  output_buffer_ = new PreviewAudioDevice(this);
  output_buffer_->open(PreviewAudioDevice::ReadWrite);
  connect(output_buffer_, &PreviewAudioDevice::Notify, this, &AudioManager::OutputNotify);

  // Poll frequently enough to follow route changes made in Control Center. If the live default
  // is absent from PortAudio's snapshot (e.g. Bluetooth headphones connected after startup),
  // SyncSystemOutputDevice() rebuilds the PortAudio device list and reopens the stream.
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
  if (channel >= 0 && channel < kMaxMonitoredChannels) {
    return output_peaks_[channel].exchange(0.0f, std::memory_order_relaxed);
  }
  return 0.0f;
}

}
