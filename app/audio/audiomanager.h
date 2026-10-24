#ifndef AUDIOMANAGER_H
#define AUDIOMANAGER_H

#include <atomic>
#include <memory>
#include <QtConcurrent/QtConcurrent>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <portaudio.h>

#include "audiovisualwaveform.h"
#include "audio/audioprocessor.h"
#include "audio/iaudiosink.h"
#include "common/define.h"
#include "codec/ffmpeg/ffmpegencoder.h"
#include "render/audioplaybackcache.h"
#include "render/previewaudiodevice.h"

namespace olive {

/**
 * @brief Unified cross-platform Audio Input and Output Manager.
 *
 * Coordinates timeline audio playback, lock-free ring buffering, pop-free ramp-drain-stop transport,
 * and hardware output sink routing.
 */
class AudioManager : public QObject
{
  Q_OBJECT
public:
  static void CreateInstance();
  static void DestroyInstance();

  static AudioManager* instance();

  void SetOutputNotifyInterval(int n);

  bool PushToOutput(const AudioParams &params, const QByteArray& samples, QString *error = nullptr,
                    bool fade_out_tail = false);

  void ClearBufferedOutput();

  void StopOutput();

  PaDeviceIndex GetOutputDevice() const
  {
    return output_device_;
  }

  PaDeviceIndex GetInputDevice() const
  {
    return input_device_;
  }

  void SetOutputDevice(PaDeviceIndex device);

  void SetInputDevice(PaDeviceIndex device);

  void HardReset();

  bool StartRecording(const EncodingParams &params, QString *error_str = nullptr);

  void StopRecording();

  static PaDeviceIndex FindConfigDeviceByName(bool is_output_device);
  static PaDeviceIndex FindDeviceByName(const QString &s, bool is_output_device);

  static PaStreamParameters GetPortAudioParams(const AudioParams &p, PaDeviceIndex device);

  static constexpr int kMaxMonitoredChannels = 8;
  void UpdateOutputPeaks(const void *data, unsigned long frames);
  float GetOutputPeak(int channel);

signals:
  void OutputNotify();

  void OutputParamsChanged();

private slots:
  void SyncSystemOutputDevice();

private:
  AudioManager();

  virtual ~AudioManager() override;

  static PaSampleFormat GetPortAudioSampleFormat(SampleFormat fmt);

  void CloseOutputStream();

  bool RefreshPortAudioDevices();

  int TransportFadeFrames() const;
  QByteArray MakeFadeOut(int frames) const;
  void ApplyFadeIn(QByteArray &samples, int frames) const;
  void FadeOutBuffer(QByteArray &samples, int frames) const;
  void FadeOutTail(QByteArray &samples, int frames) const;

  static AudioManager* instance_;

  std::unique_ptr<IAudioSink> audio_sink_;

  PaDeviceIndex output_device_;
  AudioParams output_params_;
  PreviewAudioDevice *output_buffer_;
  QTimer *output_follow_timer_;
  QElapsedTimer output_rescan_timer_;
  bool output_rescan_in_progress_;
  bool fade_in_next_;

  PaDeviceIndex input_device_;
  PaStream *input_stream_;

  FFmpegEncoder *input_encoder_;

  std::atomic<float> output_peaks_[kMaxMonitoredChannels];
};

} // namespace olive

#endif // AUDIOMANAGER_H
