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
 *   * the BT.2408 / Reinhard display roll-off and the HLG OOTF, which belong to
 *     the GPU display transform (colormanage.frag) and are still outstanding;
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
#ifdef OLIVE_ENABLE_OCIO
  // Neither the bundled config nor OCIO 2.1 itself can express PQ or HLG, so the
  // built-in curves are the only implementation of these two spaces and there is
  // nothing to compare against in an OCIO build.
  std::cout << " (built-in engine only; skipped under OCIO)";
  OLIVE_TEST_END;
#else
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  const ColorTransform scene_linear(QStringLiteral("scene_linear"));

  ColorProcessorPtr pq_to_linear = ColorProcessor::Create(manager, QStringLiteral("Rec.2020 PQ"), scene_linear);
  ColorProcessorPtr linear_to_pq = ColorProcessor::Create(manager, QStringLiteral("scene_linear"), ColorTransform(QStringLiteral("Rec.2020 PQ")));
  ColorProcessorPtr hlg_to_linear = ColorProcessor::Create(manager, QStringLiteral("Rec.2020 HLG"), scene_linear);
  ColorProcessorPtr linear_to_hlg = ColorProcessor::Create(manager, QStringLiteral("scene_linear"), ColorTransform(QStringLiteral("Rec.2020 HLG")));

  // St 2084 endpoints: signal 0 is 0 cd/m2 and signal 1 is the 10000 cd/m2 peak.
  if (!NearlyEqual(pq_to_linear->ConvertColor(Color(0.0f, 0.0f, 0.0f)).red(), 0.0, 1e-12)) {
    std::cout << " - PQ signal 0 did not decode to 0";
    return __LINE__;
  }

  if (!NearlyEqual(pq_to_linear->ConvertColor(Color(1.0f, 1.0f, 1.0f)).red(), 1.0, 1e-6)) {
    std::cout << " - PQ signal 1 did not decode to the 10000 cd/m2 peak";
    return __LINE__;
  }

  // St 2084 anchors (1.0 == 10000 cd/m2): Y = 0.1 is 1000 cd/m2 and decodes from
  // a PQ code of ~0.751827; Y = 0.01 is 100 cd/m2 and encodes to ~0.508078.
  if (!NearlyEqual(linear_to_pq->ConvertColor(Color(0.1f, 0.1f, 0.1f)).red(), 0.7518270962, 1e-6)) {
    std::cout << " - PQ code for 1000 cd/m2 drifted from the ST 2084 reference";
    return __LINE__;
  }

  if (!NearlyEqual(linear_to_pq->ConvertColor(Color(0.01f, 0.01f, 0.01f)).red(), 0.5080784215, 1e-6)) {
    std::cout << " - PQ code for 100 cd/m2 drifted from the ST 2084 reference";
    return __LINE__;
  }

  // BT.2100 HLG: the two OETF branches meet at scene 1/12 <-> signal 0.5, and
  // the low branch is signal = sqrt(3 * scene). The 0.18/12 case is the worked
  // example published for the reference implementation (0.2121320344).
  //
  // The knee itself only agrees to ~4e-5: the published a/b/c constants are
  // rounded to 8 decimal places, so the two branches disagree by that much either
  // side of scene 1/12. The tolerance below records that gap instead of hiding it.
  if (!NearlyEqual(linear_to_hlg->ConvertColor(Color(1.0f / 12.0f, 1.0f / 12.0f, 1.0f / 12.0f)).red(), 0.5, 1e-4)) {
    std::cout << " - HLG scene 1/12 did not encode to signal ~0.5";
    return __LINE__;
  }

  if (!NearlyEqual(hlg_to_linear->ConvertColor(Color(0.5f, 0.5f, 0.5f)).red(), 1.0 / 12.0, 1e-6)) {
    std::cout << " - HLG signal 0.5 did not decode to scene 1/12";
    return __LINE__;
  }

  if (!NearlyEqual(linear_to_hlg->ConvertColor(Color(0.18f / 12.0f, 0.18f / 12.0f, 0.18f / 12.0f)).red(), 0.2121320344, 1e-6)) {
    std::cout << " - HLG low-branch encoding drifted from the BT.2100 reference";
    return __LINE__;
  }

  // Monotonicity and invertibility over the whole signal range. Both curves are
  // defined on [0, 1] and must be strictly increasing and round-trip.
  double pq_previous = -1.0;
  double hlg_previous = -1.0;

  for (int i = 0; i <= 256; i++) {
    const float signal = float(i) / 256.0f;

    const Color pq_signal(signal, signal, signal);
    const float pq_linear = pq_to_linear->ConvertColor(pq_signal).red();

    if (pq_linear < pq_previous) {
      std::cout << " - PQ decode is not monotonic at signal " << i;
      return __LINE__;
    }

    pq_previous = pq_linear;

    if (!NearlyEqual(linear_to_pq->ConvertColor(Color(pq_linear, pq_linear, pq_linear)).red(), signal, 1e-5)) {
      std::cout << " - PQ round-trip failed at signal " << i;
      return __LINE__;
    }

    const Color hlg_signal(signal, signal, signal);
    const float hlg_linear = hlg_to_linear->ConvertColor(hlg_signal).red();

    if (hlg_linear < hlg_previous) {
      std::cout << " - HLG decode is not monotonic at signal " << i;
      return __LINE__;
    }

    hlg_previous = hlg_linear;

    // Looser than the PQ tolerance: see the note on the rounded HLG constants
    // above, which caps round-trip accuracy at ~4e-5.
    if (!NearlyEqual(linear_to_hlg->ConvertColor(Color(hlg_linear, hlg_linear, hlg_linear)).red(), signal, 2e-4)) {
      std::cout << " - HLG round-trip failed at signal " << i;
      return __LINE__;
    }
  }

  // The HDR curves must differ from a straight linear passthrough - otherwise the
  // spaces would still be doing nothing, which is exactly the bug 0042 fixes.
  if (NearlyEqual(hlg_to_linear->ConvertColor(Color(0.75f, 0.75f, 0.75f)).red(), 0.75, 1e-3)
      || NearlyEqual(pq_to_linear->ConvertColor(Color(0.75f, 0.75f, 0.75f)).red(), 0.75, 1e-3)) {
    std::cout << " - an HDR space is still behaving as a linear passthrough";
    return __LINE__;
  }

  OLIVE_TEST_END;
#endif
}

}
