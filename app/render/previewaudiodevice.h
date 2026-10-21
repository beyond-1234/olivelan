#ifndef PREVIEWAUDIODEVICE_H
#define PREVIEWAUDIODEVICE_H

#include <QIODevice>
#include <QByteArray>
#include <atomic>
#include <vector>
#include <functional>

namespace olive {

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer Audio Device.
 *
 * Writer: Main / Playback queue thread
 * Reader: Real-time PortAudio / CoreAudio / ALSA callback thread
 *
 * Guarantees zero memory allocations and zero memmove shifts inside the real-time audio callback.
 */
class PreviewAudioDevice : public QIODevice
{
  Q_OBJECT
public:
  static constexpr size_t kRingBufferCapacity = 1024 * 1024 * 2; // 2 MB power of two

  explicit PreviewAudioDevice(QObject *parent = nullptr);
  ~PreviewAudioDevice() override;

  bool isSequential() const override;

  qint64 readData(char *data, qint64 maxSize) override;
  qint64 writeData(const char *data, qint64 length) override;

  int bytes_per_frame() const
  {
    return bytes_per_frame_.load(std::memory_order_relaxed);
  }

  void set_bytes_per_frame(int b);

  void set_notify_interval(qint64 i)
  {
    notify_interval_.store(i, std::memory_order_relaxed);
  }

  void clear();

  QByteArray last_frame() const;

  qint64 buffered_bytes() const;

  void modify_buffer(const std::function<void(QByteArray&)> &fn);

  void replace_buffer(const QByteArray &data);

  void reset_last_frame();

signals:
  void Notify();

private:
  void SaveLastFrame(const char *data, int len);

  std::vector<char> ring_storage_;

  alignas(64) std::atomic<size_t> write_pos_{0};
  alignas(64) std::atomic<size_t> read_pos_{0};

  std::atomic<int> bytes_per_frame_{0};
  std::atomic<qint64> notify_interval_{0};
  std::atomic<qint64> bytes_read_{0};

  // Seqlock protected last frame (max 64 bytes)
  alignas(64) mutable std::atomic<uint32_t> last_frame_seq_{0};
  char last_frame_buf_[64]{0};
  int last_frame_len_{0};
};

} // namespace olive

#endif // PREVIEWAUDIODEVICE_H
