/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2023 Olive Studios LLC

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

#ifndef LIBOLIVECORE_HALF_H
#define LIBOLIVECORE_HALF_H

#include <cstdint>
#include <cstring>

namespace olive::core {

/**
 * @brief IEEE 754 binary16 <-> binary32 conversion.
 *
 * Replaces Imath::half so olivecore no longer depends on the Imath library.
 * The conversion is round-to-nearest-even, matching the Imath / OpenEXR half
 * type that Olive previously used, so existing F16 data - including the phase-6
 * colour Golden set in tests/golden/pixel_convert - stays bit-identical.
 */

inline uint16_t float_to_half(float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));

  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t exponent = int32_t((bits >> 23) & 0xffu);
  const uint32_t mantissa = bits & 0x7fffffu;

  if (exponent == 0xff) {
    // Inf or NaN. Preserve NaN-ness and collapse any payload to a canonical
    // non-zero mantissa so we never turn a NaN into an Inf.
    if (mantissa == 0) {
      return uint16_t(sign | 0x7c00u);
    }
    uint16_t nan_mantissa = uint16_t(mantissa >> 13);
    return uint16_t(sign | 0x7c00u | (nan_mantissa ? nan_mantissa : 1));
  }

  // Re-bias the exponent from binary32 (bias 127) to binary16 (bias 15).
  int32_t half_exponent = exponent - 112;

  if (half_exponent >= 0x1f) {
    return uint16_t(sign | 0x7c00u); // overflow to infinity
  }

  if (half_exponent <= 0) {
    if (half_exponent < -10) {
      return uint16_t(sign); // underflow to (signed) zero
    }

    // Subnormal: implicit leading 1 shifted back into the mantissa.
    const uint32_t m = mantissa | 0x800000u;
    const uint32_t shift = uint32_t(14 - half_exponent);
    uint32_t half_mantissa = m >> shift;
    const uint32_t remainder = m & ((uint32_t(1) << shift) - 1u);
    const uint32_t halfway = uint32_t(1) << (shift - 1u);

    if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u))) {
      ++half_mantissa;
    }

    return uint16_t(sign | half_mantissa);
  }

  uint32_t half_mantissa = mantissa >> 13;
  const uint32_t remainder = mantissa & 0x1fffu;

  if (remainder > 0x1000u || (remainder == 0x1000u && (half_mantissa & 1u))) {
    ++half_mantissa;
    if (half_mantissa == 0x400u) {
      half_mantissa = 0;
      if (++half_exponent >= 0x1f) {
        return uint16_t(sign | 0x7c00u); // rounded up into infinity
      }
    }
  }

  return uint16_t(sign | (uint32_t(half_exponent) << 10) | half_mantissa);
}

inline float half_to_float(uint16_t value)
{
  const uint32_t sign = (uint32_t(value) & 0x8000u) << 16;
  uint32_t exponent = (uint32_t(value) >> 10) & 0x1fu;
  uint32_t mantissa = uint32_t(value) & 0x3ffu;

  uint32_t bits;

  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign; // +/- zero
    } else {
      // Subnormal: normalise the mantissa into the binary32 form.
      exponent = 113; // 127 - 15 + 1
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;

      bits = sign | (exponent << 23) | (mantissa << 13);
    }
  } else if (exponent == 0x1f) {
    bits = sign | 0x7f800000u | (mantissa << 13); // Inf / NaN
  } else {
    bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }

  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

}

#endif // LIBOLIVECORE_HALF_H
