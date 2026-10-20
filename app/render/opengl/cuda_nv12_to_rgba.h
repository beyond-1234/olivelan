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

// Divider-aware preview variant: the output surface is only output_width x output_height, and
// every output sample reads the centre of its footprint in the full-resolution source planes.
// The unsuffixed launchers above remain the identity-size wrappers for callers that need them.
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
                                               int dst_x = 0,
                                               int dst_y = 0);

// Patch 0027: full-precision 10-bit conversion writing IEEE-754 binary16 RGBA into a
// GL_RGBA16F surface (no 8-bit truncation at the kernel exit).
cudaError_t OliveLaunchP010ToRgba16fSurface(const uint16_t *y_plane,
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
                                                  int dst_x = 0,
                                                  int dst_y = 0);

#endif