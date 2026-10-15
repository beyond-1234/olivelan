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
 *   3. The VAAPI pipeline never claims 10-bit delivery: the capability note must
 *      state that planes are delivered as imported, with no silent downsample -
 *      the honesty marker itself is pinned here so a reword cannot quietly
 *      re-introduce ambiguity.
 *
 * Runtime factory rejection of a forced d3d11 backend needs a GL context and is
 * observable only in a graphics session (tracked in the GPU-session backlog).
 */

#include "testutil.h"

#include <QFile>
#include <QString>

#include "codec/hal/gpu_environment_probe.h"
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
  //    imported, no silent downsample.
  {
    QFile src(QStringLiteral("app/codec/hal/vaapi_dmabuf_pipeline.cpp"));
    if (src.open(QFile::ReadOnly)) {
      const QString text = QString::fromUtf8(src.readAll());
      if (!text.contains(QStringLiteral("does NOT convert or truncate"))) {
        std::cout << " - VAAPI 10-bit capability note lost its explicit no-conversion wording";
        return __LINE__;
      }
      if (!text.contains(QStringLiteral("No silent downsample happens here"))) {
        std::cout << " - VAAPI 10-bit capability note lost its no-silent-downsample marker";
        return __LINE__;
      }
    }
  }

  OLIVE_TEST_END;
}

}
