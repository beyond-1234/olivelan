#ifndef PORTAUDIOSINK_H
#define PORTAUDIOSINK_H

#include "iaudiosink.h"

#include <portaudio.h>
#include <unordered_set>

namespace olive {

class PortAudioSink : public IAudioSink {
public:
  PortAudioSink();
  ~PortAudioSink() override;

  bool Open(const AudioParams &params, int device_id,
            AudioCallback callback, QString *error = nullptr) override;
  bool Start() override;
  bool Stop() override;
  void Close() override;

  bool IsActive() const override;
  int current_device() const override { return current_device_; }
  AudioParams current_params() const override { return current_params_; }
  double suggested_latency() const override { return suggested_latency_; }

  QVector<AudioDeviceDescriptor> EnumerateDevices() override;
  int GetDefaultOutputDevice() override;
  int GetDefaultInputDevice() override;
  bool IsFormatSupported(const AudioParams &params, int device_id) override;

  void RegisterDeviceFailure(int device_id) override;
  bool IsDeviceFailed(int device_id) const override;
  void ClearDeviceFailures() override;

  static PaSampleFormat ToPortAudioFormat(SampleFormat fmt);

private:
  static int StreamCallback(const void *input, void *output,
                            unsigned long frame_count,
                            const PaStreamCallbackTimeInfo *time_info,
                            PaStreamCallbackFlags status_flags,
                            void *user_data);

  PaStream *stream_{nullptr};
  int current_device_{paNoDevice};
  AudioParams current_params_;
  double suggested_latency_{0.0};
  AudioCallback callback_;
  std::unordered_set<int> failed_devices_;
};

} // namespace olive

#endif // PORTAUDIOSINK_H
