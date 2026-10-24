#include "previewaudiodevice.h"

#include <algorithm>
#include <cstring>

namespace olive {

PreviewAudioDevice::PreviewAudioDevice(QObject *parent) :
  QIODevice(parent),
  ring_storage_(kRingBufferCapacity, 0),
  write_pos_(0),
  read_pos_(0),
  bytes_per_frame_(0),
  notify_interval_(0),
  bytes_read_(0),
  last_frame_seq_(0),
  last_frame_len_(0)
{
  std::memset(last_frame_buf_, 0, sizeof(last_frame_buf_));
}

PreviewAudioDevice::~PreviewAudioDevice()
{
  close();
}

bool PreviewAudioDevice::isSequential() const
{
  return true;
}

void PreviewAudioDevice::set_bytes_per_frame(int b)
{
  bytes_per_frame_.store(b, std::memory_order_relaxed);
  reset_last_frame();
}

void PreviewAudioDevice::SaveLastFrame(const char *data, int len)
{
  if (!data || len <= 0 || len > static_cast<int>(sizeof(last_frame_buf_))) {
    return;
  }

  // Increment sequence lock to odd (writing in progress)
  uint32_t seq = last_frame_seq_.load(std::memory_order_relaxed);
  last_frame_seq_.store(seq + 1, std::memory_order_release);

  std::memcpy(last_frame_buf_, data, len);
  last_frame_len_ = len;

  // Increment sequence lock to even (writing finished)
  last_frame_seq_.store(seq + 2, std::memory_order_release);
}

QByteArray PreviewAudioDevice::last_frame() const
{
  const int bpf = bytes_per_frame_.load(std::memory_order_relaxed);
  if (bpf <= 0 || bpf > static_cast<int>(sizeof(last_frame_buf_))) {
    return QByteArray();
  }

  char buf[64];
  int len = 0;

  // Seqlock read loop: retry if a write happened concurrently
  for (int retry = 0; retry < 5; ++retry) {
    uint32_t seq1 = last_frame_seq_.load(std::memory_order_acquire);
    if (seq1 & 1) {
      continue; // Odd means currently writing
    }
    len = last_frame_len_;
    if (len > 0 && len <= static_cast<int>(sizeof(buf))) {
      std::memcpy(buf, last_frame_buf_, len);
    }
    uint32_t seq2 = last_frame_seq_.load(std::memory_order_acquire);
    if (seq1 == seq2) {
      return (len > 0) ? QByteArray(buf, len) : QByteArray(bpf, '\0');
    }
  }

  return QByteArray(bpf, '\0');
}

void PreviewAudioDevice::reset_last_frame()
{
  const int bpf = bytes_per_frame_.load(std::memory_order_relaxed);
  uint32_t seq = last_frame_seq_.load(std::memory_order_relaxed);
  last_frame_seq_.store(seq + 1, std::memory_order_release);

  std::memset(last_frame_buf_, 0, sizeof(last_frame_buf_));
  last_frame_len_ = (bpf > 0 && bpf <= static_cast<int>(sizeof(last_frame_buf_))) ? bpf : 0;

  last_frame_seq_.store(seq + 2, std::memory_order_release);
}

qint64 PreviewAudioDevice::readData(char *data, qint64 maxSize)
{
  if (!data || maxSize <= 0) {
    return 0;
  }

  const size_t r = read_pos_.load(std::memory_order_relaxed);
  const size_t w = write_pos_.load(std::memory_order_acquire);
  const size_t available = (w >= r) ? (w - r) : 0;
  const size_t to_read = std::min(static_cast<size_t>(maxSize), available);

  if (to_read > 0) {
    constexpr size_t mask = kRingBufferCapacity - 1;
    const size_t start_idx = r & mask;
    const size_t first_chunk = std::min(to_read, kRingBufferCapacity - start_idx);

    std::memcpy(data, &ring_storage_[start_idx], first_chunk);
    if (to_read > first_chunk) {
      std::memcpy(data + first_chunk, &ring_storage_[0], to_read - first_chunk);
    }

    read_pos_.store(r + to_read, std::memory_order_release);

    const int bpf = bytes_per_frame_.load(std::memory_order_relaxed);
    if (bpf > 0 && to_read >= static_cast<size_t>(bpf)) {
      SaveLastFrame(data + to_read - bpf, bpf);
    }

    qint64 cur_read = bytes_read_.load(std::memory_order_relaxed);
    qint64 new_read = cur_read + to_read;
    bytes_read_.store(new_read, std::memory_order_relaxed);

    const qint64 interval = notify_interval_.load(std::memory_order_relaxed);
    if (interval > 0 && (cur_read / interval) != (new_read / interval)) {
      emit Notify();
    }
  }

  return static_cast<qint64>(to_read);
}

qint64 PreviewAudioDevice::writeData(const char *data, qint64 length)
{
  if (!data || length <= 0) {
    return 0;
  }

  const size_t w = write_pos_.load(std::memory_order_relaxed);
  const size_t r = read_pos_.load(std::memory_order_acquire);
  const size_t occupied = (w >= r) ? (w - r) : 0;
  const size_t available = (kRingBufferCapacity > occupied) ? (kRingBufferCapacity - occupied) : 0;
  const size_t to_write = std::min(static_cast<size_t>(length), available);

  if (to_write == 0) {
    return 0;
  }

  constexpr size_t mask = kRingBufferCapacity - 1;
  const size_t start_idx = w & mask;
  const size_t first_chunk = std::min(to_write, kRingBufferCapacity - start_idx);

  std::memcpy(&ring_storage_[start_idx], data, first_chunk);
  if (to_write > first_chunk) {
    std::memcpy(&ring_storage_[0], data + first_chunk, to_write - first_chunk);
  }

  write_pos_.store(w + to_write, std::memory_order_release);
  return static_cast<qint64>(to_write);
}

qint64 PreviewAudioDevice::buffered_bytes() const
{
  const size_t w = write_pos_.load(std::memory_order_acquire);
  const size_t r = read_pos_.load(std::memory_order_relaxed);
  return (w >= r) ? static_cast<qint64>(w - r) : 0;
}

void PreviewAudioDevice::clear()
{
  const size_t w = write_pos_.load(std::memory_order_acquire);
  read_pos_.store(w, std::memory_order_release);
  bytes_read_.store(0, std::memory_order_relaxed);
}

void PreviewAudioDevice::replace_buffer(const QByteArray &data)
{
  clear();
  if (!data.isEmpty()) {
    writeData(data.constData(), data.size());
  }
}

void PreviewAudioDevice::modify_buffer(const std::function<void(QByteArray&)> &fn)
{
  if (!fn) return;

  const qint64 available = buffered_bytes();
  if (available <= 0) {
    QByteArray empty;
    fn(empty);
    return;
  }

  // Drain current unread buffer into QByteArray
  QByteArray current(static_cast<int>(available), Qt::Uninitialized);
  const size_t r = read_pos_.load(std::memory_order_relaxed);
  constexpr size_t mask = kRingBufferCapacity - 1;
  const size_t start_idx = r & mask;
  const size_t first_chunk = std::min(static_cast<size_t>(available), kRingBufferCapacity - start_idx);

  std::memcpy(current.data(), &ring_storage_[start_idx], first_chunk);
  if (static_cast<size_t>(available) > first_chunk) {
    std::memcpy(current.data() + first_chunk, &ring_storage_[0], available - first_chunk);
  }

  // Atomically discard unread
  clear();

  // Apply transformation and write back
  fn(current);
  if (!current.isEmpty()) {
    writeData(current.constData(), current.size());
  }
}

} // namespace olive
