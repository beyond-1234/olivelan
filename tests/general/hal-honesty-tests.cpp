/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

/**
 * Phase 8 / patch 0049: HAL capability-honesty gate.
 *
 * Three headless-verifiable invariants from the HAL capability audit:
 *
 *   1. D3D11 interop is a PLACEHOLDER: SupportsD3D11() must answer false
 *      unconditionally, on every platform. If someone later "enables" it without
 *      implementing it, this gate fails before a user can select a backend that
 *      initializes a device and then silently drops every frame to Tier 3.
 *
 *   2. Tier 3 is the safety net: its factory entry point must always be available
 *      (the backend dialog advertises it unconditionally).
 *
 *   3. The VAAPI 10-bit import must stay honest: the capability note must state that the
 *      16-bit planes are imported intact and composed into a GL_RGBA16F slot with no
 *      silent downsample. The honesty markers are pinned here so a reword cannot quietly
 *      re-introduce ambiguity.
 *
 *   4. VA-API support is reported only when a VA-API device can actually be created at
 *      runtime, not merely because the backend was compiled in (F-7).
 *
 * Runtime factory rejection of a forced d3d11 backend needs a GL context and is
 * observable only in a graphics session (tracked in the GPU-session backlog).
 */

#include "testutil.h"

#include <QFile>
#include <QString>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "codec/hal/gpu_environment_probe.h"
#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/reinforced_software_pipeline.h"

namespace olive {

OLIVE_ADD_TEST(HalCapabilityHonesty)
{
  auto& probe = GpuEnvironmentProbe::Instance();

  // 1. D3D11 placeholder: hard false, everywhere, always.
  if (probe.SupportsD3D11()) {
    std::cout << " - SupportsD3D11() answered true: the placeholder policy has been"
                 " broken (a device could initialize and then drop every frame)";
    return __LINE__;
  }

  // 2. Tier 3 is the unconditional safety net: its class must be instantiated by the
  //    factory (compile/link-level presence is what matters, not the working dir).
  {
    ReinforcedSoftwarePipeline safety_net(nullptr);
    // Constructing must not throw or crash; the instance is the proof the pipeline
    // translation unit is compiled into the test binary's link set.
  }

  // 3. The VAAPI 10-bit note must keep the honesty wording: planes delivered as
  //    imported, no silent downsample, 10-bit precision preserved in the output slot.
  //
  //    Gate-quality fix (HAL/Phase6-8 review §五 #3/#4): the path is injected at build
  //    time instead of being resolved relative to the process CWD, and a missing file is
  //    now a FAILURE rather than a silent skip (the old "if (src.open()) {...}" made this
  //    whole check a no-op under ctest). The markers were also updated to the wording
  //    patch 0050 actually introduced.
  {
    QFile src(QStringLiteral(OLIVE_TEST_SOURCE_DIR "/app/codec/hal/vaapi_dmabuf_pipeline.cpp"));
    if (!src.open(QFile::ReadOnly)) {
      std::cout << " - could not open the VAAPI pipeline source ("
                << OLIVE_TEST_SOURCE_DIR "/app/codec/hal/vaapi_dmabuf_pipeline.cpp"
                << ") for the honesty check";
      return __LINE__;
    }

    const QString text = QString::fromUtf8(src.readAll());

    if (!text.contains(QStringLiteral("imported intact (GL_R16/GL_RG16)"))) {
      std::cout << " - VAAPI 10-bit capability note lost its intact-import (GL_R16/GL_RG16) wording";
      return __LINE__;
    }
    if (!text.contains(QStringLiteral("No silent downsample."))) {
      std::cout << " - VAAPI 10-bit capability note lost its no-silent-downsample marker";
      return __LINE__;
    }
    if (!text.contains(QStringLiteral("GL_RGBA16F"))) {
      std::cout << " - VAAPI 10-bit capability note no longer states the 10-bit output slot";
      return __LINE__;
    }
  }

  // 4. F-7: SupportsVaapiDmaBuf() must reflect real device availability, not the compile
  //    flag. Recreate a VA-API device and require the two answers to agree.
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  {
    AVBufferRef *device = nullptr;
    const bool creatable =
        (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) >= 0);
    if (device) {
      av_buffer_unref(&device);
    }
    if (probe.SupportsVaapiDmaBuf() != creatable) {
      std::cout << " - SupportsVaapiDmaBuf()=" << probe.SupportsVaapiDmaBuf()
                << " disagrees with actual VA-API device availability=" << creatable;
      return __LINE__;
    }
  }
#endif

  // 5. The recommendation must describe the Tier 2 backend that auto routing actually uses.
  //    macOS selects VideoToolbox, so an assessment that only checks VA-API used to fall through
  //    to "Tier 3" even while the factory was decoding with VideoToolbox.
  {
    const GpuSessionAdvisor::Assessment assessment = GpuSessionAdvisor::Assess();

    if (probe.SupportsVideoToolbox()) {
      if (assessment.tier2_backend != GpuSessionAdvisor::Tier2Backend::VideoToolbox) {
        std::cout << " - VideoToolbox is available but the advisor did not report it as Tier 2";
        return __LINE__;
      }
      if (!assessment.tier2_possible) {
        std::cout << " - VideoToolbox is available but the advisor marked Tier 2 unavailable";
        return __LINE__;
      }
      if (!GpuSessionAdvisor::DescribeEffectivePath(assessment)
               .startsWith(QStringLiteral("Tier 2"))) {
        std::cout << " - the advisor described auto routing as Tier 3 despite VideoToolbox";
        return __LINE__;
      }
    }

    if (probe.SupportsVaapiDmaBuf()
        && assessment.tier2_backend != GpuSessionAdvisor::Tier2Backend::VaapiDmaBuf) {
      std::cout << " - a VA-API device is available but the advisor did not report Tier 2 VA-API";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

}
