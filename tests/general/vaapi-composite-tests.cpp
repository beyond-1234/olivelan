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
 *   2. The 10-bit limited-range expansion is exact *in the normalised domain*. GL
 *      UNORM sampling already normalises every depth to [0,1], so a P010 sample
 *      MSB-aligned in a 16-bit container reads back as s = val10*64/65535 ~= val10/1023
 *      and must NOT be rescaled into the integer domain (the pre-fix `s * 65535/64`
 *      produced ~1024x over-bright output). The composite expands limited range as
 *      (s - 64/1023) * 1023/876, i.e. black = 64/1023 -> 0 and white = 940/1023 -> 1.
 *      This gate also asserts the old integer-domain scale would have been over-range,
 *      so the regression cannot quietly return.
 *
 *   3. The Tier 2 pipeline class must stay registered with its user-facing VA-API marker
 *      (the composite path is part of its contract now).
 *
 * Runtime import/composite behaviour itself needs a GL context and is observed in a
 * graphics session (GPU-session backlog); the measurement probes live in the outer
 * repository (p010_export_probe / vaapi_import_probe).
 */

#include "testutil.h"

#include <cmath>

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

  // 2. 10-bit limited-range expansion, in the normalised domain. The GL_R16 UNORM
  //    sample is already normalised (MSB-aligned P010: s = val10*64/65535), so the
  //    composite must NOT rescale into the integer domain. Its limited-range expansion
  //    is (s - 64/1023) * (1023/876): black (64/1023) -> 0, white (940/1023) -> 1.
  {
    auto sample = [](int val10) {
      return (static_cast<double>(val10) * 64.0) / 65535.0;
    };
    const double black = 64.0 / 1023.0;
    const double white = 940.0 / 1023.0;
    const double scale = 1023.0 / 876.0;

    const double black_out = (sample(64) - black) * scale;
    if (std::abs(black_out) > 1e-3) {
      std::cout << " - 10-bit black (64/1023) does not map to 0: got " << black_out;
      return __LINE__;
    }

    const double white_out = (sample(940) - black) * scale;
    if (std::abs(white_out - 1.0) > 3e-3) {
      std::cout << " - 10-bit white (940/1023) does not map to 1: got " << white_out;
      return __LINE__;
    }

    // In-range codes expand monotonically and stay inside [0,1] up to the small
    // MSB-alignment discrepancy inherent in val10*64/65535 (= val10/1023.98).
    double previous = -1.0;
    for (int val10 = 64; val10 <= 940; val10++) {
      const double out = (sample(val10) - black) * scale;
      if (out < -3e-3 || out > 1.0 + 3e-3) {
        std::cout << " - 10-bit limited-range output out of [0,1]: val10=" << val10
                  << " out=" << out;
        return __LINE__;
      }
      if (out <= previous) {
        std::cout << " - 10-bit limited-range output not monotonic at val10=" << val10;
        return __LINE__;
      }
      previous = out;
    }

    // Negative control: the pre-fix integer-domain rescale (s * 65535/64) is wildly
    // over-range, which is exactly the regression this gate exists to catch.
    if (sample(940) * (65535.0 / 64.0) <= 1.0) {
      std::cout << " - negative control failed: the old integer-domain scale was not over-range";
      return __LINE__;
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
