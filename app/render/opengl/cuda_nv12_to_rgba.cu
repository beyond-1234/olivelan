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
                                        bool full_range)
{
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  const float sample_y = static_cast<float>(y_plane[y * y_pitch + x]);
  const int uv_x = (x / 2) * 2;
  const int uv_y = y / 2;
  const float u = static_cast<float>(uv_plane[uv_y * uv_pitch + uv_x]) - 128.0f;
  const float v = static_cast<float>(uv_plane[uv_y * uv_pitch + uv_x + 1]) - 128.0f;

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
  surf2Dwrite(rgba, output_surface, x * sizeof(uchar4), y);
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
                                         bool full_range)
{
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
                                               full_range);
  return cudaGetLastError();
}