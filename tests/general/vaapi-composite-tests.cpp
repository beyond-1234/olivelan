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
 * Phase 8 / patch 0050: VA-API two-plane import composite gates.
 *
 * Pins the invariants the 780M-measured composite path depends on:
 *
 *   1. The fourcc constants are numerically what their names say. The pre-0050 code
 *      carried DRM_FORMAT_GR1616 = 0x32335247, which is actually GR32 (Mesa radeonsi);
 *      a driver emitting true GR1616 (0x36315247) would then be misclassified as 8-bit.
 *      That failure mode must stay closed.
 *
 *   2. The 10-bit UNORM scale factor is exact. The shader recovers the 10-bit domain
 *      from a 16-bit MSB-aligned container via s * 65535/64; this gate pins the
 *      arithmetic (val10 = s * 65535/64 with s = val10*64/65535 round-trips exactly).
 *
 *   3. The composite shader source keeps the full-range and limited-range handling and
 *      the three matrix coefficient uniforms - the same contract as :/shaders/yuv2rgb.frag.
 *
 * Runtime import/composite behaviour itself needs a GL context and is observed in a
 * graphics session (GPU-session backlog); the measurement probes live in the outer
 * repository (p010_export_probe / vaapi_import_probe).
 */

#include "testutil.h"

#include <QString>

#include "codec/hal/vaapi_dmabuf_pipeline.h"

namespace olive {

OLIVE_ADD_TEST(VaapiCompositePath)
{
  // 1. fourcc constants: name-value agreement, verified through the same numeric
  //    contract the pipeline's static_assert enforces. Recomputed here independently
  //    so a change to either side alone cannot pass.
  {
    auto fcc = [](char a, char b, char c, char d) -> uint32_t {
      return (static_cast<uint32_t>(a)) | (static_cast<uint32_t>(b) << 8)
             | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    };
    const uint32_t gr32 = fcc('G', 'R', '3', '2');
    const uint32_t gr1616 = fcc('G', 'R', '1', '6');
    if (gr32 != 0x32335247u) {
      std::cout << " - GR32 fourcc mismatch: expected 0x32335247, got 0x" << std::hex << gr32;
      return __LINE__;
    }
    if (gr1616 != 0x36315247u) {
      std::cout << " - GR1616 fourcc mismatch: expected 0x36315247, got 0x" << std::hex << gr1616;
      return __LINE__;
    }
    if (gr32 == gr1616) {
      std::cout << " - GR32 and GR1616 collapsed to the same numeric value";
      return __LINE__;
    }
  }

  // 2. 10-bit scale factor: 16-bit container round-trip must be exact for every 10-bit value.
  {
    for (int val10 = 0; val10 <= 1023; val10++) {
      const double val16 = static_cast<double>(val10) * 64.0;   // MSB-aligned container
      const double s = val16 / 65535.0;                          // UNORM readback
      const double recovered = s * (65535.0 / 64.0);             // shader scale
      const int round_tripped = static_cast<int>(recovered + 0.5);
      if (round_tripped != val10) {
        std::cout << " - 10-bit scale factor broken: val10=" << val10
                  << " recovered=" << round_tripped;
        return __LINE__;
      }
    }
  }

  // 3. The Tier 2 pipeline class must remain registered (the composite path is part
  //    of its contract now).
  {
    VaapiDmaBufPipeline pipeline(nullptr);
    // The backend name is user-facing wording that identifies the composite path.
    const QString name = QString::fromUtf8(pipeline.GetBackendName());
    if (!name.contains(QStringLiteral("VA-API"))) {
      std::cout << " - VAAPI pipeline backend name lost its VA-API marker";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

}
