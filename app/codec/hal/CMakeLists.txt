# Olive - Non-Linear Video Editor
# Hardware Abstraction Layer (HAL) Sources

set(OLIVE_SOURCES
  ${OLIVE_SOURCES}
  codec/hal/video_hardware_pipeline.h
  codec/hal/gpu_environment_probe.h
  codec/hal/gpu_environment_probe.cpp
  codec/hal/gpu_session_advisor.h
  codec/hal/gpu_session_advisor.cpp
  codec/hal/hardware_pipeline_factory.h
  codec/hal/hardware_pipeline_factory.cpp
  codec/hal/cuda_nvdec_pipeline.h
  codec/hal/cuda_nvdec_pipeline.cpp
  codec/hal/vaapi_dmabuf_pipeline.h
  codec/hal/vaapi_dmabuf_pipeline.cpp
  codec/hal/videotoolbox_pipeline.h
  codec/hal/videotoolbox_pipeline.cpp
  codec/hal/d3d11_interop_pipeline.h
  codec/hal/d3d11_interop_pipeline.cpp
  codec/hal/reinforced_software_pipeline.h
  codec/hal/reinforced_software_pipeline.cpp
  PARENT_SCOPE
)
