#include "portaudiosink.h"

#include <QDebug>
#include <algorithm>

namespace olive {

PortAudioSink::PortAudioSink() = default;

PortAudioSink::~PortAudioSink()
{
  Close();
}

PaSampleFormat PortAudioSink::ToPortAudioFormat(SampleFormat fmt)
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
  case SampleFormat::COUNT:
  case SampleFormat::INVALID:
    break;
  }
  return paCustomFormat;
}

int PortAudioSink::StreamCallback(const void *input, void *output,
                                  unsigned long frame_count,
                                  const PaStreamCallbackTimeInfo *time_info,
                                  PaStreamCallbackFlags status_flags,
                                  void *user_data)
{
  Q_UNUSED(time_info);
  auto *sink = static_cast<PortAudioSink*>(user_data);
  if (sink && sink->callback_) {
    return sink->callback_(input, output, frame_count, static_cast<uint32_t>(status_flags));
  }
  return paContinue;
}

bool PortAudioSink::Open(const AudioParams &params, int device_id,
                         AudioCallback callback, QString *error)
{
  Close();

  if (device_id == paNoDevice) {
    device_id = GetDefaultOutputDevice();
  }

  if (device_id == paNoDevice) {
    if (error) *error = QStringLiteral("No audio output device available");
    return false;
  }

  const PaDeviceInfo *info = Pa_GetDeviceInfo(device_id);
  if (!info || info->maxOutputChannels <= 0) {
    RegisterDeviceFailure(device_id);
    if (error) *error = QStringLiteral("Specified audio device has no output channels");
    return false;
  }

  callback_ = std::move(callback);
  current_params_ = params;
  current_device_ = device_id;

  PaStreamParameters p;
  p.channelCount = params.channel_count();
  p.device = device_id;
  p.hostApiSpecificStreamInfo = nullptr;
  p.sampleFormat = ToPortAudioFormat(params.format());
  // Use a stable jitter-absorbing latency suitable for timeline video preview (>= 50ms)
  p.suggestedLatency = std::max(info->defaultHighOutputLatency, 0.050);
  suggested_latency_ = p.suggestedLatency;

  PaError r = Pa_OpenStream(&stream_, nullptr, &p, current_params_.sample_rate(),
                            paFramesPerBufferUnspecified, paNoFlag,
                            StreamCallback, this);

#if defined(__linux__)
  if (r != paNoError && device_id != Pa_GetDefaultOutputDevice()) {
    // Record device failure to prevent 250ms periodic oscillating retries
    RegisterDeviceFailure(device_id);

    const int fallback = Pa_GetDefaultOutputDevice();
    if (fallback != paNoDevice && fallback != device_id && !IsDeviceFailed(fallback)) {
      const PaDeviceInfo *fb_info = Pa_GetDeviceInfo(fallback);
      if (fb_info && fb_info->maxOutputChannels > 0) {
        PaStreamParameters fb_p = p;
        fb_p.device = fallback;
        fb_p.suggestedLatency = std::max(fb_info->defaultHighOutputLatency, 0.050);

        const PaError fb_err = Pa_OpenStream(&stream_, nullptr, &fb_p,
                                             current_params_.sample_rate(),
                                             paFramesPerBufferUnspecified,
                                             paNoFlag, StreamCallback, this);
        if (fb_err == paNoError) {
          qWarning() << "[Audio] Failed to open device" << info->name << "(" << Pa_GetErrorText(r)
                     << "); successfully fell back to" << fb_info->name;
          current_device_ = fallback;
          suggested_latency_ = fb_p.suggestedLatency;
          r = paNoError;
        }
      }
    }
  }
#endif

  if (r != paNoError) {
    RegisterDeviceFailure(device_id);
    if (error) *error = QString::fromUtf8(Pa_GetErrorText(r));
    stream_ = nullptr;
    current_device_ = paNoDevice;
    return false;
  }

  return true;
}

bool PortAudioSink::Start()
{
  if (!stream_) return false;
  if (Pa_IsStreamActive(stream_)) return true;

  PaError r = Pa_StartStream(stream_);
  if (r != paNoError) {
    qWarning() << "[Audio] Pa_StartStream failed:" << Pa_GetErrorText(r);
    return false;
  }
  return true;
}

bool PortAudioSink::Stop()
{
  if (!stream_) return false;
  if (!Pa_IsStreamActive(stream_)) return true;

  PaError r = Pa_StopStream(stream_);
  if (r != paNoError) {
    qWarning() << "[Audio] Pa_StopStream failed:" << Pa_GetErrorText(r);
    return false;
  }
  return true;
}

void PortAudioSink::Close()
{
  if (stream_) {
    if (Pa_IsStreamActive(stream_)) {
      Pa_StopStream(stream_);
    }
    Pa_CloseStream(stream_);
    stream_ = nullptr;
  }
  current_device_ = paNoDevice;
  suggested_latency_ = 0.0;
}

bool PortAudioSink::IsActive() const
{
  return stream_ && (Pa_IsStreamActive(stream_) == 1);
}

QVector<AudioDeviceDescriptor> PortAudioSink::EnumerateDevices()
{
  QVector<AudioDeviceDescriptor> list;
  const int count = Pa_GetDeviceCount();
  if (count <= 0) return list;

  const int default_out = Pa_GetDefaultOutputDevice();
  const int default_in = Pa_GetDefaultInputDevice();

  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
    if (!info) continue;

    AudioDeviceDescriptor desc;
    desc.id = i;
    desc.name = QString::fromUtf8(info->name);
    desc.max_output_channels = info->maxOutputChannels;
    desc.max_input_channels = info->maxInputChannels;
    desc.is_default = (i == default_out || i == default_in);

    const PaHostApiInfo *host = Pa_GetHostApiInfo(info->hostApi);
    desc.host_api = host ? QString::fromUtf8(host->name) : QString();

    list.push_back(desc);
  }

  return list;
}

int PortAudioSink::GetDefaultOutputDevice()
{
  return Pa_GetDefaultOutputDevice();
}

int PortAudioSink::GetDefaultInputDevice()
{
  return Pa_GetDefaultInputDevice();
}

bool PortAudioSink::IsFormatSupported(const AudioParams &params, int device_id)
{
  if (device_id == paNoDevice) {
    device_id = GetDefaultOutputDevice();
  }
  if (device_id == paNoDevice) return false;

  PaStreamParameters p;
  p.channelCount = params.channel_count();
  p.device = device_id;
  p.hostApiSpecificStreamInfo = nullptr;
  p.sampleFormat = ToPortAudioFormat(params.format());
  p.suggestedLatency = 0.050;

  return Pa_IsFormatSupported(nullptr, &p, params.sample_rate()) == paNoError;
}

void PortAudioSink::RegisterDeviceFailure(int device_id)
{
  if (device_id >= 0) {
    failed_devices_.insert(device_id);
  }
}

bool PortAudioSink::IsDeviceFailed(int device_id) const
{
  return failed_devices_.find(device_id) != failed_devices_.end();
}

void PortAudioSink::ClearDeviceFailures()
{
  failed_devices_.clear();
}

} // namespace olive
