#include "testutil.h"

#include <thread>
#include <vector>
#include <numeric>
#include <atomic>

#include "render/previewaudiodevice.h"

namespace olive {

OLIVE_ADD_TEST(AudioRingBufferBasicReadWrite)
{
  PreviewAudioDevice device;
  device.set_bytes_per_frame(4);

  const char test_data[] = "12345678abcdefgh";
  const qint64 written = device.writeData(test_data, 16);
  OLIVE_ASSERT_EQUAL(written, 16);
  OLIVE_ASSERT_EQUAL(device.buffered_bytes(), 16);

  char out_buf[32] = {0};
  const qint64 read_bytes = device.readData(out_buf, 16);
  OLIVE_ASSERT_EQUAL(read_bytes, 16);
  OLIVE_ASSERT_EQUAL(device.buffered_bytes(), 0);
  OLIVE_ASSERT_EQUAL(std::string(out_buf, 16), std::string(test_data, 16));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(AudioRingBufferWraparoundIntegrity)
{
  PreviewAudioDevice device;
  device.set_bytes_per_frame(4);

  // Write and read repeatedly to advance pointers across ring buffer capacity
  std::vector<char> chunk(4096);
  for (size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<char>(i & 0xFF);
  }

  std::vector<char> read_back(4096);

  // Advance by more than 2MB (buffer capacity)
  const int iterations = 600; // 600 * 4KB = 2.4 MB
  for (int iter = 0; iter < iterations; ++iter) {
    qint64 w = device.writeData(chunk.data(), chunk.size());
    OLIVE_ASSERT_EQUAL(w, static_cast<qint64>(chunk.size()));

    qint64 r = device.readData(read_back.data(), read_back.size());
    OLIVE_ASSERT_EQUAL(r, static_cast<qint64>(read_back.size()));
    OLIVE_ASSERT(std::memcmp(chunk.data(), read_back.data(), chunk.size()) == 0);
  }

  OLIVE_ASSERT_EQUAL(device.buffered_bytes(), 0);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(AudioRingBufferConcurrentSPSC)
{
  PreviewAudioDevice device;
  device.set_bytes_per_frame(4);

  constexpr int kTotalFrames = 50000;
  constexpr int kFrameBytes = 4;
  constexpr size_t kTotalBytes = kTotalFrames * kFrameBytes;

  std::vector<uint32_t> produced(kTotalFrames);
  std::iota(produced.begin(), produced.end(), 1); // 1, 2, 3 ...

  std::vector<uint32_t> consumed;
  consumed.reserve(kTotalFrames);

  std::atomic<bool> producer_done{false};

  // Consumer thread (simulating real-time audio callback)
  std::thread consumer([&]() {
    std::vector<uint32_t> buf(256);
    while (!producer_done.load(std::memory_order_acquire) || device.buffered_bytes() > 0) {
      qint64 r = device.readData(reinterpret_cast<char*>(buf.data()), buf.size() * sizeof(uint32_t));
      if (r > 0) {
        size_t frames = r / sizeof(uint32_t);
        consumed.insert(consumed.end(), buf.begin(), buf.begin() + frames);
      } else {
        std::this_thread::yield();
      }
    }
  });

  // Producer thread (main thread)
  const char *src = reinterpret_cast<const char*>(produced.data());
  size_t bytes_left = kTotalBytes;
  while (bytes_left > 0) {
    size_t chunk_size = std::min(bytes_left, static_cast<size_t>(512));
    qint64 w = device.writeData(src, chunk_size);
    if (w > 0) {
      src += w;
      bytes_left -= w;
    } else {
      std::this_thread::yield();
    }
  }
  producer_done.store(true, std::memory_order_release);

  consumer.join();

  OLIVE_ASSERT_EQUAL(consumed.size(), produced.size());
  OLIVE_ASSERT(consumed == produced);

  OLIVE_TEST_END;
}

} // namespace olive
