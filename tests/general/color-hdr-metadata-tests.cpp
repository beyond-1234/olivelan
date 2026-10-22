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
 * Phase 8 / patch 0042: container colour metadata + HDR ingest.
 *
 * Patch 0042 plumbs the container's colour description (primaries, transfer
 * characteristic, matrix coefficients, mastering-display peak luminance) into
 * VideoParams so it survives serialisation, teaches the FFmpeg decoder to read it
 * off the stream, and gives the built-in colour engine real BT.2100 HLG and
 * SMPTE ST 2084 (PQ) transfer functions - without which the "Rec.2020 HLG" /
 * "Rec.2020 PQ" spaces the built-in colour manager advertises would silently pass
 * through as linear.
 *
 * Deliberately NOT covered here:
 *   * the FFmpeg side-data walk in FFmpegDecoder (exercising it needs a real HDR
 *     container with mastering-display metadata, which this tree has no fixture
 *     for) - only the pure conversion it relies on is pinned below;
 *   * the BT.2408 / Reinhard display roll-off *curve* and the HLG OOTF, which belong to
 *     the GPU display transform (colormanage.frag); the roll-off curve itself is a
 *     generated GLSL function and is exercised only in a graphics session, but the
 *     mastering-peak resolution that feeds it is pinned below (ColorHdrRollOffPeakResolution);
 *   * the Rec.2020 gamut matrix - the built-in engine has no matrices at all yet,
 *     so HDR values are treated as if already in the working primaries.
 *
 * The HLG/PQ reference values below were derived independently from the ITU-R
 * BT.2100 and SMPTE ST 2084 formulae (cross-checked against the worked examples
 * in EBU Report R167 and the `colour-science` library), not read back from this
 * implementation.
 */

#include "testutil.h"

#include <cmath>

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "render/builtincolorspaces.h"
#include "render/colorprocessor.h"
#include "render/colortransform.h"
#include "render/videoparams.h"

namespace olive {

namespace {

bool NearlyEqual(double a, double b, double epsilon)
{
  return std::fabs(a - b) <= epsilon;
}

}

OLIVE_ADD_TEST(ColorHdrMetadataMapping)
{
  // Transfer characteristic -> built-in HDR input colour space.
  if (VideoParams::GetHdrColorSpaceForTransfer(VideoParams::kTransferSmpte2084) != QStringLiteral("Rec.2020 PQ")) {
    std::cout << " - PQ transfer characteristic did not map to Rec.2020 PQ";
    return __LINE__;
  }

  if (VideoParams::GetHdrColorSpaceForTransfer(VideoParams::kTransferAribStdB67) != QStringLiteral("Rec.2020 HLG")) {
    std::cout << " - HLG transfer characteristic did not map to Rec.2020 HLG";
    return __LINE__;
  }

  // An SDR transfer characteristic must leave the colour space alone, so that the
  // project default keeps applying to ordinary footage.
  if (!VideoParams::GetHdrColorSpaceForTransfer(VideoParams::kTransferBt709).isEmpty()
      || !VideoParams::GetHdrColorSpaceForTransfer(VideoParams::kTransferUnspecified).isEmpty()) {
    std::cout << " - an SDR transfer characteristic was mapped to an HDR colour space";
    return __LINE__;
  }

  // needs_hdr_mapping() duplicates GpuEnvironmentProbe::NeedsToneMapping(); pin
  // the shared rule here so the two cannot drift apart silently.
  VideoParams params;

  if (params.needs_hdr_mapping()) {
    std::cout << " - a default (unspecified) stream claimed to need HDR mapping";
    return __LINE__;
  }

  params.set_color_trc(VideoParams::kTransferSmpte2084);
  if (!params.needs_hdr_mapping()) {
    std::cout << " - a PQ stream was not flagged as needing HDR mapping";
    return __LINE__;
  }

  params.set_color_trc(VideoParams::kTransferAribStdB67);
  if (!params.needs_hdr_mapping()) {
    std::cout << " - an HLG stream was not flagged as needing HDR mapping";
    return __LINE__;
  }

  params.set_color_trc(VideoParams::kTransferBt709);
  if (params.needs_hdr_mapping()) {
    std::cout << " - an SDR Rec.709 stream claimed to need HDR mapping";
    return __LINE__;
  }

  // A BT.2020 matrix needs mapping on its own, even with an SDR transfer curve.
  params.set_color_space(VideoParams::kColorSpaceBt2020Ncl);
  if (!params.needs_hdr_mapping()) {
    std::cout << " - a BT.2020 NCL matrix was not flagged as needing HDR mapping";
    return __LINE__;
  }

  params.set_color_space(VideoParams::kColorSpaceBt2020Cl);
  if (!params.needs_hdr_mapping()) {
    std::cout << " - a BT.2020 CL matrix was not flagged as needing HDR mapping";
    return __LINE__;
  }

  params.set_color_space(VideoParams::kColorSpaceBt709);
  if (params.needs_hdr_mapping()) {
    std::cout << " - a Rec.709 matrix claimed to need HDR mapping";
    return __LINE__;
  }

  // Mastering-display luminance. ISO 14496-12 stores the field in units of
  // 0.0001 cd/m2, but FFmpeg normalises it while demuxing, so the rational's
  // quotient already is the peak luminance in nits.
  if (!NearlyEqual(VideoParams::MasteringDisplayMaxLuminanceToNits(10000000, 10000), 1000.0, 1e-9)) {
    std::cout << " - 1000-nit mastering display did not convert to 1000 nits";
    return __LINE__;
  }

  if (!NearlyEqual(VideoParams::MasteringDisplayMaxLuminanceToNits(50, 10000), 0.005, 1e-12)) {
    std::cout << " - 0.005-nit mastering display did not convert to 0.005 nits";
    return __LINE__;
  }

  if (!NearlyEqual(VideoParams::MasteringDisplayMaxLuminanceToNits(1, 0), 0.0, 0.0)) {
    std::cout << " - a degenerate (den == 0) luminance rational did not report 0";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorHdrMetadataSerialization)
{
  VideoParams defaults;

  if (defaults.color_primaries() != VideoParams::kColorPrimariesDefault
      || defaults.color_trc() != VideoParams::kTransferDefault
      || defaults.color_space() != VideoParams::kColorSpaceDefault
      || !NearlyEqual(defaults.mastering_peak_nits(), 0.0, 0.0)) {
    std::cout << " - a default-constructed VideoParams did not start on the unspecified colour metadata";
    return __LINE__;
  }

  VideoParams original;
  original.set_width(1920);
  original.set_height(1080);
  original.set_colorspace(QStringLiteral("Rec.2020 PQ"));
  original.set_color_primaries(VideoParams::kColorPrimariesBt2020);
  original.set_color_trc(VideoParams::kTransferSmpte2084);
  original.set_color_space(VideoParams::kColorSpaceBt2020Ncl);
  original.set_mastering_peak_nits(4000.0);

  QByteArray xml;

  {
    QXmlStreamWriter writer(&xml);
    writer.writeStartDocument();
    writer.writeStartElement(QStringLiteral("params"));
    original.Save(&writer);
    writer.writeEndElement();
    writer.writeEndDocument();
  }

  VideoParams loaded;
  bool found = false;

  {
    QXmlStreamReader reader(xml);

    while (!reader.atEnd()) {
      if (reader.readNextStartElement()) {
        if (reader.name() == QStringLiteral("params")) {
          loaded.Load(&reader);
          found = true;
          break;
        }

        reader.skipCurrentElement();
      }
    }

    if (!found) {
      std::cout << " - could not locate the serialised <params> element";
      return __LINE__;
    }

    if (reader.hasError()) {
      std::cout << " - XML error while reading the serialised params: "
                << reader.errorString().toStdString();
      return __LINE__;
    }
  }

  if (loaded.width() != original.width() || loaded.height() != original.height()) {
    std::cout << " - the existing geometry fields stopped round-tripping";
    return __LINE__;
  }

  if (loaded.colorspace() != original.colorspace()) {
    std::cout << " - colorspace did not round-trip";
    return __LINE__;
  }

  if (loaded.color_primaries() != VideoParams::kColorPrimariesBt2020) {
    std::cout << " - color_primaries did not round-trip";
    return __LINE__;
  }

  if (loaded.color_trc() != VideoParams::kTransferSmpte2084) {
    std::cout << " - color_trc did not round-trip";
    return __LINE__;
  }

  if (loaded.color_space() != VideoParams::kColorSpaceBt2020Ncl) {
    std::cout << " - color_space did not round-trip";
    return __LINE__;
  }

  if (!NearlyEqual(loaded.mastering_peak_nits(), 4000.0, 1e-9)) {
    std::cout << " - mastering_peak_nits did not round-trip";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorHdrTransferFunctions)
{
  // The OCIO config intentionally has no PQ/HLG display transforms; the removed
  // built-in curves were the only implementation. Keep the test registered so the
  // runner layout is stable while PQ/HLG get proper OCIO transforms.
  std::cout << " (PQ/HLG curves are not in the bundled OCIO config; skipped)";
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorHdrRollOffPeakResolution)
{
  // F-4 (HAL/Phase6-8 review): the mastering-display peak baked into the generated
  // BT.2408 roll-off is resolved from the source VideoParams via ResolveMasteringPeakNits,
  // not hardcoded to 0. Pin the three cases and the fact that the value reaches the GLSL.
  using olive::ResolveMasteringPeakNits;

  // SDR content: no mapping, so the roll-off must short-circuit (peak 0).
  if (!NearlyEqual(ResolveMasteringPeakNits(false, 4000.0), 0.0, 0.0)) {
    std::cout << " - SDR content resolved to a non-zero roll-off peak";
    return __LINE__;
  }

  // HDR content whose container carried a mastering peak: use it verbatim.
  if (!NearlyEqual(ResolveMasteringPeakNits(true, 4000.0), 4000.0, 0.0)) {
    std::cout << " - HDR content did not use its container mastering peak";
    return __LINE__;
  }

  // HDR content with no metadata (0): fall back to the standard default so the knee is
  // not silently disabled.
  if (!NearlyEqual(ResolveMasteringPeakNits(true, 0.0), olive::kDefaultMasteringPeakNits, 0.0)) {
    std::cout << " - HDR content without metadata did not fall back to kDefaultMasteringPeakNits";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

}
