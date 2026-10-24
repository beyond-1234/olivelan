#include "cuda_nv12_to_rgba.h"

#include <cuda_fp16.h>

#include "preview_scale.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace {

__device__ __forceinline__ unsigned char ClampByte(float value)
{
  return static_cast<unsigned char>(fminf(fmaxf(value, 0.0f), 255.0f));
}

// Pack a normalised [0,1] float into the exact IEEE-754 binary16 bit pattern a
// GL_RGBA16F surface expects.
__device__ __forceinline__ unsigned short ClampHalfBits(float value)
{
  return __half_as_ushort(__float2half_rn(fminf(fmaxf(value, 0.0f), 1.0f)));
}

__global__ void Nv12ToRgbaSurfaceKernel(const uint8_t *y_plane,
                                        int y_pitch,
                                        const uint8_t *uv_plane,
                                        int uv_pitch,
                                        cudaSurfaceObject_t output_surface,
                                        int source_width,
                                        int source_height,
                                        int output_width,
                                        int output_height,
                                        int color_space,
                                        bool full_range,
                                        int dst_x,
                                        int dst_y)
{
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) {
    return;
  }

  // Divider-aware preview: map this output sample to the centre of its source footprint.
  // With divider == 1 this is the identity mapping.
  const int source_x = olive::MapPreviewSourceCoordinate(x, output_width, source_width);
  const int source_y = olive::MapPreviewSourceCoordinate(y, output_height, source_height);

  // Access Y plane strictly respecting driver line pitch
  const float sample_y = static_cast<float>(y_plane[source_y * y_pitch + source_x]);

  // Robust UV subsampling using bitwise ops to round down to even index
  const int uv_x = (source_x >> 1) << 1;
  const int uv_y = source_y >> 1;

  // Boundary clamp: ensure (uv_x + 1) does not exceed the source plane
  const int safe_uv_x = (uv_x + 1 < source_width) ? uv_x : max(0, source_width - 2);

  const float u = static_cast<float>(uv_plane[uv_y * uv_pitch + safe_uv_x]) - 128.0f;
  const float v = static_cast<float>(uv_plane[uv_y * uv_pitch + safe_uv_x + 1]) - 128.0f;

  float y_value = sample_y;
  float red_v;
  float green_u;
  float green_v;
  float blue_u;

  if (color_space == AVCOL_SPC_BT709) {
    red_v = full_range ? 1.5748f : 1.792741f;
    green_u = full_range ? 0.187324f : 0.213249f;
    green_v = full_range ? 0.468124f : 0.532909f;
    blue_u = full_range ? 1.8556f : 2.112402f;
    if (!full_range) {
      y_value = 1.164383f * (sample_y - 16.0f);
    }
  } else if (color_space == AVCOL_SPC_BT2020_NCL || color_space == AVCOL_SPC_BT2020_CL) {
    red_v = full_range ? 1.4746f : 1.678674f;
    green_u = full_range ? 0.16455f : 0.187326f;
    green_v = full_range ? 0.57135f : 0.650424f;
    blue_u = full_range ? 1.8814f : 2.14177f;
    if (!full_range) {
      y_value = 1.164383f * (sample_y - 16.0f);
    }
  } else {
    red_v = full_range ? 1.402f : 1.596027f;
    green_u = full_range ? 0.344136f : 0.391762f;
    green_v = full_range ? 0.714136f : 0.812968f;
    blue_u = full_range ? 1.772f : 2.017232f;
    if (!full_range) {
      y_value = 1.164383f * (sample_y - 16.0f);
    }
  }

  const float red = y_value + red_v * v;
  const float green = y_value - green_u * u - green_v * v;
  const float blue = y_value + blue_u * u;
  const uchar4 rgba = make_uchar4(ClampByte(red), ClampByte(green), ClampByte(blue), 255);

  const int write_x = dst_x + x;
  const int write_y = dst_y + y;
  surf2Dwrite(rgba, output_surface, write_x * sizeof(uchar4), write_y);
}

// Shared 10-bit (P010) YUV -> RGB evaluation. The RGBA8 and RGBA16F kernels both
// route through this single definition so the colour math can never drift between
// them. Returns RGB scaled to the 0..255 range; the 16F kernel normalises below.
__device__ __forceinline__ void P010SampleToRgb(const uint16_t *y_plane,
                                                int y_stride,
                                                const uint16_t *uv_plane,
                                                int uv_stride,
                                                int x,
                                                int y,
                                                int color_space,
                                                bool full_range,
                                                float &red,
                                                float &green,
                                                float &blue)
{
  // In P010, the 10-bit sample is MSB-aligned in a 16-bit container
  const float sample_y = static_cast<float>(y_plane[y * y_stride + x] >> 6);

  const int uv_x = (x >> 1) << 1;
  const int uv_y = y >> 1;
  const int safe_uv_x = (uv_x + 1 < uv_stride) ? uv_x : max(0, uv_stride - 2);

  const float sample_u = static_cast<float>(uv_plane[uv_y * uv_stride + safe_uv_x] >> 6);
  const float sample_v = static_cast<float>(uv_plane[uv_y * uv_stride + safe_uv_x + 1] >> 6);

  float y_norm;
  float u_norm;
  float v_norm;

  if (full_range) {
    y_norm = sample_y * (255.0f / 1023.0f);
    u_norm = (sample_u - 512.0f) * (255.0f / 1023.0f);
    v_norm = (sample_v - 512.0f) * (255.0f / 1023.0f);
  } else {
    // 10-bit limited range: Y in [64, 940], UV in [64, 960]. The luma expansion is a
    // pure 255/876 (== 1.164383 * 255/219 has NO place here: it is the 8-bit factor and
    // double-applies the limited-range expansion, making P010 ~16% too bright - the
    // HAL/Phase6-8 review's F-5). The chroma normalisation 255/896 already accounts for
    // the limited-range chroma compression, so the full-range coefficients below are
    // correct for it.
    y_norm = (sample_y - 64.0f) * (255.0f / 876.0f);
    u_norm = (sample_u - 512.0f) * (255.0f / 896.0f);
    v_norm = (sample_v - 512.0f) * (255.0f / 896.0f);
  }

  float red_v;
  float green_u;
  float green_v;
  float blue_u;

  if (color_space == AVCOL_SPC_BT2020_NCL || color_space == AVCOL_SPC_BT2020_CL) {
    red_v = 1.4746f;
    green_u = 0.16455f;
    green_v = 0.57135f;
    blue_u = 1.8814f;
  } else if (color_space == AVCOL_SPC_BT709) {
    red_v = 1.5748f;
    green_u = 0.187324f;
    green_v = 0.468124f;
    blue_u = 1.8556f;
  } else {
    red_v = 1.402f;
    green_u = 0.344136f;
    green_v = 0.714136f;
    blue_u = 1.772f;
  }

  red = y_norm + red_v * v_norm;
  green = y_norm - green_u * u_norm - green_v * v_norm;
  blue = y_norm + blue_u * u_norm;
}

__global__ void P010ToRgbaSurfaceKernel(const uint16_t *y_plane,
                                        int y_pitch_bytes,
                                        const uint16_t *uv_plane,
                                        int uv_pitch_bytes,
                                        cudaSurfaceObject_t output_surface,
                                        int source_width,
                                        int source_height,
                                        int output_width,
                                        int output_height,
                                        int color_space,
                                        bool full_range,
                                        int dst_x,
                                        int dst_y)
{
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) {
    return;
  }

  const int source_x = olive::MapPreviewSourceCoordinate(x, output_width, source_width);
  const int source_y = olive::MapPreviewSourceCoordinate(y, output_height, source_height);

  const int y_stride = y_pitch_bytes / sizeof(uint16_t);
  const int uv_stride = uv_pitch_bytes / sizeof(uint16_t);

  float red;
  float green;
  float blue;
  P010SampleToRgb(y_plane, y_stride, uv_plane, uv_stride, source_x, source_y,
                  color_space, full_range, red, green, blue);

  const uchar4 rgba = make_uchar4(ClampByte(red), ClampByte(green), ClampByte(blue), 255);

  const int write_x = dst_x + x;
  const int write_y = dst_y + y;
  surf2Dwrite(rgba, output_surface, write_x * sizeof(uchar4), write_y);
}

// Full-precision variant (patch 0027): writes IEEE-754 binary16 RGBA to a GL_RGBA16F
// surface, preserving the full 10-bit input precision that the RGBA8 kernel truncates
// to 8 bits. Values are normalised to [0,1] as GL expects for floating-point colour.
__global__ void P010ToRgba16fSurfaceKernel(const uint16_t *y_plane,
                                           int y_pitch_bytes,
                                           const uint16_t *uv_plane,
                                           int uv_pitch_bytes,
                                           cudaSurfaceObject_t output_surface,
                                           int source_width,
                                           int source_height,
                                           int output_width,
                                           int output_height,
                                           int color_space,
                                           bool full_range,
                                           int dst_x,
                                           int dst_y)
{
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) {
    return;
  }

  const int source_x = olive::MapPreviewSourceCoordinate(x, output_width, source_width);
  const int source_y = olive::MapPreviewSourceCoordinate(y, output_height, source_height);

  const int y_stride = y_pitch_bytes / sizeof(uint16_t);
  const int uv_stride = uv_pitch_bytes / sizeof(uint16_t);

  float red;
  float green;
  float blue;
  P010SampleToRgb(y_plane, y_stride, uv_plane, uv_stride, source_x, source_y,
                  color_space, full_range, red, green, blue);

  const float normalise = 1.0f / 255.0f;
  const ushort4 rgba = make_ushort4(ClampHalfBits(red * normalise),
                                    ClampHalfBits(green * normalise),
                                    ClampHalfBits(blue * normalise),
                                    __half_as_ushort(__float2half_rn(1.0f)));

  const int write_x = dst_x + x;
  const int write_y = dst_y + y;
  surf2Dwrite(rgba, output_surface, write_x * sizeof(ushort4), write_y);
}

}

cudaError_t OliveLaunchNv12ToRgbaSurface(const uint8_t *y_plane,
                                         int y_pitch,
                                         const uint8_t *uv_plane,
                                         int uv_pitch,
                                         cudaSurfaceObject_t output_surface,
                                         int width,
                                         int height,
                                         int color_space,
                                         bool full_range,
                                         int dst_x,
                                         int dst_y)
{
  return OliveLaunchNv12ToRgbaScaledSurface(y_plane, y_pitch, uv_plane, uv_pitch,
                                            output_surface, width, height, width, height,
                                            color_space, full_range, dst_x, dst_y);
}

cudaError_t OliveLaunchNv12ToRgbaScaledSurface(const uint8_t *y_plane,
                                               int y_pitch,
                                               const uint8_t *uv_plane,
                                               int uv_pitch,
                                               cudaSurfaceObject_t output_surface,
                                               int source_width,
                                               int source_height,
                                               int output_width,
                                               int output_height,
                                               int color_space,
                                               bool full_range,
                                               int dst_x,
                                               int dst_y)
{
  if (!y_plane || !uv_plane || !output_surface || source_width <= 0 || source_height <= 0
      || output_width <= 0 || output_height <= 0 || y_pitch <= 0 || uv_pitch <= 0) {
    return cudaErrorInvalidValue;
  }

  const dim3 threads(16, 16);
  const dim3 blocks((output_width + threads.x - 1) / threads.x,
                    (output_height + threads.y - 1) / threads.y);
  Nv12ToRgbaSurfaceKernel<<<blocks, threads>>>(y_plane,
                                               y_pitch,
                                               uv_plane,
                                               uv_pitch,
                                               output_surface,
                                               source_width,
                                               source_height,
                                               output_width,
                                               output_height,
                                               color_space,
                                               full_range,
                                               dst_x,
                                               dst_y);
  return cudaGetLastError();
}

cudaError_t OliveLaunchP010ToRgbaSurface(const uint16_t *y_plane,
                                         int y_pitch_bytes,
                                         const uint16_t *uv_plane,
                                         int uv_pitch_bytes,
                                         cudaSurfaceObject_t output_surface,
                                         int width,
                                         int height,
                                         int color_space,
                                         bool full_range,
                                         int dst_x,
                                         int dst_y)
{
  return OliveLaunchP010ToRgbaScaledSurface(y_plane, y_pitch_bytes, uv_plane, uv_pitch_bytes,
                                            output_surface, width, height, width, height,
                                            color_space, full_range, dst_x, dst_y);
}

cudaError_t OliveLaunchP010ToRgbaScaledSurface(const uint16_t *y_plane,
                                               int y_pitch_bytes,
                                               const uint16_t *uv_plane,
                                               int uv_pitch_bytes,
                                               cudaSurfaceObject_t output_surface,
                                               int source_width,
                                               int source_height,
                                               int output_width,
                                               int output_height,
                                               int color_space,
                                               bool full_range,
                                               int dst_x,
                                               int dst_y)
{
  if (!y_plane || !uv_plane || !output_surface || source_width <= 0 || source_height <= 0
      || output_width <= 0 || output_height <= 0 || y_pitch_bytes <= 0 || uv_pitch_bytes <= 0) {
    return cudaErrorInvalidValue;
  }

  const dim3 threads(16, 16);
  const dim3 blocks((output_width + threads.x - 1) / threads.x,
                    (output_height + threads.y - 1) / threads.y);
  P010ToRgbaSurfaceKernel<<<blocks, threads>>>(y_plane,
                                               y_pitch_bytes,
                                               uv_plane,
                                               uv_pitch_bytes,
                                               output_surface,
                                               source_width,
                                               source_height,
                                               output_width,
                                               output_height,
                                               color_space,
                                               full_range,
                                               dst_x,
                                               dst_y);
  return cudaGetLastError();
}

cudaError_t OliveLaunchP010ToRgba16fSurface(const uint16_t *y_plane,
                                            int y_pitch_bytes,
                                            const uint16_t *uv_plane,
                                            int uv_pitch_bytes,
                                            cudaSurfaceObject_t output_surface,
                                            int width,
                                            int height,
                                            int color_space,
                                            bool full_range,
                                            int dst_x,
                                            int dst_y)
{
  return OliveLaunchP010ToRgba16fScaledSurface(y_plane, y_pitch_bytes, uv_plane, uv_pitch_bytes,
                                               output_surface, width, height, width, height,
                                               color_space, full_range, dst_x, dst_y);
}

cudaError_t OliveLaunchP010ToRgba16fScaledSurface(const uint16_t *y_plane,
                                                  int y_pitch_bytes,
                                                  const uint16_t *uv_plane,
                                                  int uv_pitch_bytes,
                                                  cudaSurfaceObject_t output_surface,
                                                  int source_width,
                                                  int source_height,
                                                  int output_width,
                                                  int output_height,
                                                  int color_space,
                                                  bool full_range,
                                                  int dst_x,
                                                  int dst_y)
{
  if (!y_plane || !uv_plane || !output_surface || source_width <= 0 || source_height <= 0
      || output_width <= 0 || output_height <= 0 || y_pitch_bytes <= 0 || uv_pitch_bytes <= 0) {
    return cudaErrorInvalidValue;
  }

  const dim3 threads(16, 16);
  const dim3 blocks((output_width + threads.x - 1) / threads.x,
                    (output_height + threads.y - 1) / threads.y);
  P010ToRgba16fSurfaceKernel<<<blocks, threads>>>(y_plane,
                                                  y_pitch_bytes,
                                                  uv_plane,
                                                  uv_pitch_bytes,
                                                  output_surface,
                                                  source_width,
                                                  source_height,
                                                  output_width,
                                                  output_height,
                                                  color_space,
                                                  full_range,
                                                  dst_x,
                                                  dst_y);
  return cudaGetLastError();
}