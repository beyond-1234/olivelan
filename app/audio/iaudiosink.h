#ifndef IAUDIOSINK_H
#define IAUDIOSINK_H

#include <QString>
#include <QVector>
#include <cstdint>
#include <functional>

#include <olive/core/render/audioparams.h>

namespace olive {

using core::AudioParams;
using core::SampleFormat;

struct AudioDeviceDescriptor {
  int id{-1};
  QString name;
  QString host_api;
  int max_output_channels{0};
  int max_input_channels{0};
  bool is_default{false};
};

class IAudioSink {
public:
  using AudioCallback = std::function<int(const void *input, void *output,
                                          unsigned long frame_count,
                                          uint32_t status_flags)>;

  virtual ~IAudioSink() = default;

  virtual bool Open(const AudioParams &params, int device_id,
                    AudioCallback callback, QString *error = nullptr) = 0;
  virtual bool Start() = 0;
  virtual bool Stop() = 0;
  virtual void Close() = 0;

  virtual bool IsActive() const = 0;
  virtual int current_device() const = 0;
  virtual AudioParams current_params() const = 0;
  virtual double suggested_latency() const = 0;

  virtual QVector<AudioDeviceDescriptor> EnumerateDevices() = 0;
  virtual int GetDefaultOutputDevice() = 0;
  virtual int GetDefaultInputDevice() = 0;
  virtual bool IsFormatSupported(const AudioParams &params, int device_id) = 0;

  virtual void RegisterDeviceFailure(int device_id) = 0;
  virtual bool IsDeviceFailed(int device_id) const = 0;
  virtual void ClearDeviceFailures() = 0;
};

} // namespace olive

#endif // IAUDIOSINK_H
