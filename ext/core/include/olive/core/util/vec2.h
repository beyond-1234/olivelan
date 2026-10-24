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

#ifndef LIBOLIVECORE_VEC2_H
#define LIBOLIVECORE_VEC2_H

namespace olive::core {

/**
 * @brief Minimal double-precision 2D vector.
 *
 * Replaces Imath::V2d so that olivecore (and the app) no longer pull in the
 * Imath library for a two-component vector. The public surface intentionally
 * mirrors exactly the subset of Imath::V2d that Olive uses: public .x/.y
 * members, the (x, y) and default constructors, and component-wise addition.
 */
struct Vec2d
{
  double x;
  double y;

  Vec2d() :
    x(0.0),
    y(0.0)
  {
  }

  Vec2d(double x_, double y_) :
    x(x_),
    y(y_)
  {
  }

  Vec2d operator+(const Vec2d &rhs) const { return Vec2d(x + rhs.x, y + rhs.y); }
  Vec2d operator-(const Vec2d &rhs) const { return Vec2d(x - rhs.x, y - rhs.y); }
  Vec2d operator*(double s) const { return Vec2d(x * s, y * s); }
  Vec2d operator/(double s) const { return Vec2d(x / s, y / s); }

  Vec2d &operator+=(const Vec2d &rhs) { x += rhs.x; y += rhs.y; return *this; }
  Vec2d &operator-=(const Vec2d &rhs) { x -= rhs.x; y -= rhs.y; return *this; }

  double dot(const Vec2d &rhs) const { return x * rhs.x + y * rhs.y; }
};

}

#endif // LIBOLIVECORE_VEC2_H
