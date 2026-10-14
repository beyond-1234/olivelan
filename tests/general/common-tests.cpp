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

#include "testutil.h"

#include "common/digit.h"

#include <cstdint>

#include <olive/core/util/color.h>
#include <olive/core/util/half.h>

namespace olive {

OLIVE_ADD_TEST(DigitTest)
{
  OLIVE_ASSERT(GetDigitCount(1) == 1);
  OLIVE_ASSERT(GetDigitCount(69) == 2);
  OLIVE_ASSERT(GetDigitCount(420) == 3);
  OLIVE_ASSERT(GetDigitCount(1337) == 4);
  OLIVE_ASSERT(GetDigitCount(80085) == 5);
  OLIVE_ASSERT(GetDigitCount(555555) == 6);
  OLIVE_ASSERT(GetDigitCount(8675309) == 7);
  OLIVE_ASSERT(GetDigitCount(78956423) == 8);
  OLIVE_ASSERT(GetDigitCount(148497523) == 9);
  OLIVE_ASSERT(GetDigitCount(4845821233) == 10);
  OLIVE_ASSERT(GetDigitCount(18002738255) == 11);
  OLIVE_ASSERT(GetDigitCount(180027382556) == 12);
  OLIVE_ASSERT(GetDigitCount(1800273825568) == 13);
  OLIVE_ASSERT(GetDigitCount(18002738255685) == 14);
  OLIVE_ASSERT(GetDigitCount(180027382556857) == 15);
  OLIVE_ASSERT(GetDigitCount(1800273825564857) == 16);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorFp16Conversion)
{
  // Phase 7 / patch 0030: proves the local IEEE-754 half conversion that
  // replaced Imath::half is bit-exact against the binary16 specification.
  using olive::core::float_to_half;
  using olive::core::half_to_float;

  // Reference encodings (round-to-nearest-even).
  OLIVE_ASSERT(float_to_half(0.0f) == 0x0000);
  OLIVE_ASSERT(float_to_half(-0.0f) == 0x8000);
  OLIVE_ASSERT(float_to_half(1.0f) == 0x3C00);
  OLIVE_ASSERT(float_to_half(-1.0f) == 0xBC00);
  OLIVE_ASSERT(float_to_half(2.0f) == 0x4000);
  OLIVE_ASSERT(float_to_half(0.5f) == 0x3800);
  OLIVE_ASSERT(float_to_half(0.1f) == 0x2E66);
  OLIVE_ASSERT(float_to_half(65504.0f) == 0x7BFF);          // largest finite half
  OLIVE_ASSERT(float_to_half(65520.0f) == 0x7C00);          // ties up to +inf
  OLIVE_ASSERT(float_to_half(5.96046448e-08f) == 0x0001);   // smallest subnormal (2^-24)
  OLIVE_ASSERT(float_to_half(2.98023224e-08f) == 0x0000);   // 2^-25 ties to even zero

  // Every representable half round-trips exactly (NaN payloads excepted, since
  // NaN mantissas are canonicalised on encode).
  for (uint32_t h = 0; h <= 0xFFFFu; ++h) {
    if ((h & 0x7c00u) == 0x7c00u && (h & 0x03ffu) != 0) {
      continue;
    }
    OLIVE_ASSERT(float_to_half(half_to_float(uint16_t(h))) == uint16_t(h));
  }

  OLIVE_ASSERT(half_to_float(0x3C00) == 1.0f);
  OLIVE_ASSERT(half_to_float(0xC000) == -2.0f);
  OLIVE_ASSERT(half_to_float(0x0001) == 5.96046448e-08f);
  OLIVE_ASSERT(half_to_float(0x7C00) > 3.0e38f);            // +infinity

  // The public Color F16 path must survive the replacement end to end.
  {
    const float in = 0.333333343f;
    char buffer[2];
    olive::core::Color(in, 0.0f, 0.0f, 1.0f).toData(buffer, olive::core::PixelFormat::F16, 1);
    const olive::core::Color out = olive::core::Color::fromData(buffer, olive::core::PixelFormat::F16, 1);
    OLIVE_ASSERT(out.red() == half_to_float(float_to_half(in)));
  }

  OLIVE_TEST_END;
}

}
