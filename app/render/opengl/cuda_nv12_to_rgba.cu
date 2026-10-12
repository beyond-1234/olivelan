#include "cuda_nv12_to_rgba.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace {

__device__ __forceinline__ unsigned char ClampByte(float value)
{
  return static_cast<unsigned char>(fminf(fmaxf(value, 0.0f), 255.0f));
}

__global__ void Nv12ToRgbaSurfaceKernel(const uint8_t *y_plane,
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
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  // Access Y plane strictly respecting driver line pitch
  const float sample_y = static_cast<float>(y_plane[y * y_pitch + x]);

  // Robust UV subsampling using bitwise ops to round down to even index
  const int uv_x = (x >> 1) << 1;
  const int uv_y = y >> 1;

  // Boundary clamp: ensure (uv_x + 1) does not exceed valid line pitch
  const int safe_uv_x = (uv_x + 1 < uv_pitch) ? uv_x : max(0, uv_pitch - 2);

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

__global__ void P010ToRgbaSurfaceKernel(const uint16_t *y_plane,
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
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  const int y_stride = y_pitch_bytes / sizeof(uint16_t);
  const int uv_stride = uv_pitch_bytes / sizeof(uint16_t);

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
    // 10-bit limited range: Y in [64, 940], UV in [64, 960]
    y_norm = 1.164383f * (sample_y - 64.0f) * (255.0f / 876.0f);
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

  const float red = y_norm + red_v * v_norm;
  const float green = y_norm - green_u * u_norm - green_v * v_norm;
  const float blue = y_norm + blue_u * u_norm;
  const uchar4 rgba = make_uchar4(ClampByte(red), ClampByte(green), ClampByte(blue), 255);

  const int write_x = dst_x + x;
  const int write_y = dst_y + y;
  surf2Dwrite(rgba, output_surface, write_x * sizeof(uchar4), write_y);
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
  if (!y_plane || !uv_plane || !output_surface || width <= 0 || height <= 0 || y_pitch <= 0 || uv_pitch <= 0) {
    return cudaErrorInvalidValue;
  }

  const dim3 threads(16, 16);
  const dim3 blocks((width + threads.x - 1) / threads.x,
                    (height + threads.y - 1) / threads.y);
  Nv12ToRgbaSurfaceKernel<<<blocks, threads>>>(y_plane,
                                               y_pitch,
                                               uv_plane,
                                               uv_pitch,
                                               output_surface,
                                               width,
                                               height,
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
  if (!y_plane || !uv_plane || !output_surface || width <= 0 || height <= 0 || y_pitch_bytes <= 0 || uv_pitch_bytes <= 0) {
    return cudaErrorInvalidValue;
  }

  const dim3 threads(16, 16);
  const dim3 blocks((width + threads.x - 1) / threads.x,
                    (height + threads.y - 1) / threads.y);
  P010ToRgbaSurfaceKernel<<<blocks, threads>>>(y_plane,
                                               y_pitch_bytes,
                                               uv_plane,
                                               uv_pitch_bytes,
                                               output_surface,
                                               width,
                                               height,
                                               color_space,
                                               full_range,
                                               dst_x,
                                               dst_y);
  return cudaGetLastError();
}