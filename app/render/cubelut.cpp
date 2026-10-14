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

#include "cubelut.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QStringList>

namespace olive {

namespace {

/**
 * @brief Split a line into whitespace-separated tokens.
 *
 * Written by hand rather than with QRegularExpression so the parser has no
 * dependency on Qt's split-behaviour enum, which changed name in Qt 5.14.
 */
QVector<QString> Tokenize(const QString &line)
{
  QVector<QString> tokens;

  int i = 0;
  while (i < line.size()) {
    while (i < line.size() && line.at(i).isSpace()) {
      i++;
    }

    const int start = i;

    while (i < line.size() && !line.at(i).isSpace()) {
      i++;
    }

    if (i > start) {
      tokens.append(line.mid(start, i - start));
    }
  }

  return tokens;
}

float Clamp01(float v)
{
  return std::min(std::max(v, 0.0f), 1.0f);
}

}

bool CubeLUT::Load(const QString &filename)
{
  QFile f(filename);

  if (!f.open(QFile::ReadOnly | QFile::Text)) {
    return false;
  }

  return Parse(QString::fromUtf8(f.readAll()));
}

bool CubeLUT::LoadFromString(const QString &text)
{
  return Parse(text);
}

bool CubeLUT::Parse(const QString &text)
{
  size_ = 0;
  is_1d_ = false;
  data_.clear();

  for (int i=0; i<3; i++) {
    domain_min_[i] = 0.0f;
    domain_max_[i] = 1.0f;
  }

  const QStringList lines = text.split(QLatin1Char('\n'));

  for (const QString &raw : lines) {
    const QString line = raw.trimmed();

    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
      continue;
    }

    if (line.startsWith(QStringLiteral("TITLE"))) {
      continue;
    }

    if (line.startsWith(QStringLiteral("LUT_1D_SIZE")) || line.startsWith(QStringLiteral("LUT_3D_SIZE"))) {
      const QVector<QString> tokens = Tokenize(line);

      if (tokens.size() != 2) {
        return false;
      }

      bool ok = false;
      const int n = tokens.at(1).toInt(&ok);

      if (!ok || n < 2) {
        return false;
      }

      size_ = n;
      is_1d_ = line.startsWith(QStringLiteral("LUT_1D_SIZE"));
      continue;
    }

    if (line.startsWith(QStringLiteral("DOMAIN_MIN")) || line.startsWith(QStringLiteral("DOMAIN_MAX"))) {
      const bool is_min = line.startsWith(QStringLiteral("DOMAIN_MIN"));
      const QVector<QString> tokens = Tokenize(line);

      if (tokens.size() != 4) {
        return false;
      }

      for (int i=0; i<3; i++) {
        bool ok = false;
        const float v = tokens.at(i + 1).toFloat(&ok);

        if (!ok) {
          return false;
        }

        (is_min ? domain_min_ : domain_max_)[i] = v;
      }

      continue;
    }

    // Anything else must be an entry row.
    const QVector<QString> tokens = Tokenize(line);

    if (tokens.size() != 3) {
      return false;
    }

    for (int i=0; i<3; i++) {
      bool ok = false;
      const float v = tokens.at(i).toFloat(&ok);

      if (!ok) {
        return false;
      }

      data_.append(v);
    }
  }

  if (size_ < 2) {
    return false;
  }

  const int expected_entries = is_1d_ ? size_ : size_ * size_ * size_;

  if (data_.size() != expected_entries * 3) {
    return false;
  }

  return true;
}

void CubeLUT::Sample(float r, float g, float b, float *out_rgb) const
{
  out_rgb[0] = r;
  out_rgb[1] = g;
  out_rgb[2] = b;

  if (!IsValid()) {
    return;
  }

  if (is_1d_) {
    Sample1D(0, r, out_rgb + 0);
    Sample1D(1, g, out_rgb + 1);
    Sample1D(2, b, out_rgb + 2);
  } else {
    Sample3D(r, g, b, out_rgb);
  }
}

void CubeLUT::Sample1D(int channel, float v, float *out) const
{
  const float span = domain_max_[channel] - domain_min_[channel];
  const float u = Clamp01((span != 0.0f) ? (v - domain_min_[channel]) / span : 0.0f);

  const float scaled = u * float(size_ - 1);
  const int i0 = int(std::floor(scaled));
  const int i1 = std::min(i0 + 1, size_ - 1);
  const float t = scaled - float(i0);

  const float a = data_.at((i0 * 3) + channel);
  const float b = data_.at((i1 * 3) + channel);

  *out = a + (b - a) * t;
}

void CubeLUT::Sample3D(float r, float g, float b, float *out) const
{
  const float in[3] = {r, g, b};

  int i0[3];
  int i1[3];
  float t[3];

  for (int c=0; c<3; c++) {
    const float span = domain_max_[c] - domain_min_[c];
    const float u = Clamp01((span != 0.0f) ? (in[c] - domain_min_[c]) / span : 0.0f);

    const float scaled = u * float(size_ - 1);
    i0[c] = int(std::floor(scaled));
    i1[c] = std::min(i0[c] + 1, size_ - 1);
    t[c] = scaled - float(i0[c]);
  }

  // In a .cube 3D LUT, red varies fastest, then green, then blue.
  auto at = [this](int rr, int gg, int bb, int ch) -> float {
    return data_.at((((bb * size_) + gg) * size_ + rr) * 3 + ch);
  };

  for (int ch=0; ch<3; ch++) {
    const float c000 = at(i0[0], i0[1], i0[2], ch);
    const float c100 = at(i1[0], i0[1], i0[2], ch);
    const float c010 = at(i0[0], i1[1], i0[2], ch);
    const float c110 = at(i1[0], i1[1], i0[2], ch);
    const float c001 = at(i0[0], i0[1], i1[2], ch);
    const float c101 = at(i1[0], i0[1], i1[2], ch);
    const float c011 = at(i0[0], i1[1], i1[2], ch);
    const float c111 = at(i1[0], i1[1], i1[2], ch);

    const float c00 = c000 + (c100 - c000) * t[0];
    const float c10 = c010 + (c110 - c010) * t[0];
    const float c01 = c001 + (c101 - c001) * t[0];
    const float c11 = c011 + (c111 - c011) * t[0];

    const float c0 = c00 + (c10 - c00) * t[1];
    const float c1 = c01 + (c11 - c01) * t[1];

    out[ch] = c0 + (c1 - c0) * t[2];
  }
}

}
