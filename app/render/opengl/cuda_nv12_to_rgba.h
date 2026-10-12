#ifndef OLIVE_CUDA_NV12_TO_RGBA_H
#define OLIVE_CUDA_NV12_TO_RGBA_H

#include <cuda_runtime_api.h>

#include <stdint.h>

cudaError_t OliveLaunchNv12ToRgbaSurface(const uint8_t *y_plane,
                                         int y_pitch,
                                         const uint8_t *uv_plane,
                                         int uv_pitch,
                                         cudaSurfaceObject_t output_surface,
                                         int width,
                                         int height,
                                         int color_space,
                                         bool full_range,
                                         int dst_x = 0,
                                         int dst_y = 0);

cudaError_t OliveLaunchP010ToRgbaSurface(const uint16_t *y_plane,
                                         int y_pitch_bytes,
                                         const uint16_t *uv_plane,
                                         int uv_pitch_bytes,
                                         cudaSurfaceObject_t output_surface,
                                         int width,
                                         int height,
                                         int color_space,
                                         bool full_range,
                                         int dst_x = 0,
                                         int dst_y = 0);

#endif