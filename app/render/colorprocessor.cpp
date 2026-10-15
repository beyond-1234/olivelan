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

#include "colorprocessor.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "common/define.h"
#include "common/ociobridge.h"
#include "common/ocioutils.h"
#include "node/color/colormanager/colormanager.h"
#include "render/builtincolorspaces.h"

#ifdef OLIVE_ENABLE_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#else
#include <olive/core/util/half.h>
#endif

namespace olive {

#ifdef OLIVE_ENABLE_OCIO

// Patch 0037: the OCIO members live in an opaque Impl so that colorprocessor.h
// (and everything that includes it) no longer needs <OpenColorIO/OpenColorIO.h>.
struct ColorProcessor::Impl
{
  OCIO::ConstProcessorRcPtr processor;
  OCIO::ConstCPUProcessorRcPtr cpu_processor;
};

ColorProcessor::ColorProcessor() :
  impl_(std::make_unique<Impl>())
{
}

ColorProcessor::ColorProcessor(ColorManager *config, const QString &input, const ColorTransform &transform, Direction direction) :
  impl_(std::make_unique<Impl>())
{
  const QString& output = (transform.output().isEmpty()) ? config->GetDefaultDisplay() : transform.output();
  const OCIO::ConstConfigRcPtr ocio_config = ColorManagerOCIO::GetConfig(config);

  if (transform.is_display()) {

    const QString& view = (transform.view().isEmpty()) ? config->GetDefaultView(output) : transform.view();

    auto display_transform = OCIO::DisplayViewTransform::Create();

    display_transform->setSrc(input.toUtf8());
    display_transform->setDisplay(output.toUtf8());
    display_transform->setView(view.toUtf8());
    display_transform->setDirection(direction == kNormal ? OCIO::TRANSFORM_DIR_FORWARD : OCIO::TRANSFORM_DIR_INVERSE);

    if (transform.look().isEmpty()) {
      impl_->processor = ocio_config->getProcessor(display_transform);
    } else {
      auto group = OCIO::GroupTransform::Create();

      const char* out_cs = OCIO::LookTransform::GetLooksResultColorSpace(ocio_config,
                                                                         ocio_config->getCurrentContext(),
                                                                         transform.look().toUtf8());

      auto lt = OCIO::LookTransform::Create();
      lt->setSrc(input.toUtf8());
      lt->setDst(out_cs);
      lt->setLooks(transform.look().toUtf8());
      lt->setSkipColorSpaceConversion(false);
      group->appendTransform(lt);

      display_transform->setSrc(out_cs);
      group->appendTransform(display_transform);

      impl_->processor = ocio_config->getProcessor(group);
    }

  } else {

    try {
      if (direction == kNormal) {
        impl_->processor = ocio_config->getProcessor(input.toUtf8(), output.toUtf8());
      } else {
        impl_->processor = ocio_config->getProcessor(output.toUtf8(), input.toUtf8());
      }
    } catch (OCIO::Exception &e) {
      qWarning() << "ColorProcessor exception:" << e.what();
    }

  }

  impl_->cpu_processor = impl_->processor->getDefaultCPUProcessor();
}

ColorProcessor::~ColorProcessor() = default;

void ColorProcessor::ConvertFrame(Frame *f)
{
  OCIO::BitDepth ocio_bit_depth = static_cast<OCIO::BitDepth>(OCIOUtils::GetOCIOBitDepthFromPixelFormat(f->format()));

  if (ocio_bit_depth == OCIO::BIT_DEPTH_UNKNOWN) {
    qCritical() << "Tried to color convert frame with no format";
    return;
  }

  OCIO::PackedImageDesc img(f->data(),
                            f->width(),
                            f->height(),
                            f->channel_count(),
                            ocio_bit_depth,
                            OCIO::AutoStride,
                            OCIO::AutoStride,
                            f->linesize_bytes());

  impl_->cpu_processor->apply(img);
}

Color ColorProcessor::ConvertColor(const Color& in)
{
  // I've been bamboozled
  float c[4] = {float(in.red()), float(in.green()), float(in.blue()), float(in.alpha())};

  impl_->cpu_processor->applyRGBA(c);

  return Color(c[0], c[1], c[2], c[3]);
}

const char *ColorProcessor::id() const
{
  return impl_->processor->getCacheID();
}

bool ColorProcessor::IsDefaulted() const
{
  // OpenColorIO resolves colour space names when the processor is built; an
  // unknown name throws rather than yielding an unconverted passthrough, so a
  // constructed processor is never silently defaulted on this path.
  return false;
}

ColorProcessorPtr ColorProcessor::Create(ColorManager *config, const QString& input, const ColorTransform &transform, Direction direction)
{
  return std::make_shared<ColorProcessor>(config, input, transform, direction);
}

void ColorProcessor::ConvertFrame(FramePtr f)
{
  ConvertFrame(f.get());
}

ColorProcessorPtr ColorProcessorOCIO::Create(OCIO::ConstProcessorRcPtr processor)
{
  ColorProcessorPtr p(new ColorProcessor());

  p->impl_->processor = processor;
  p->impl_->cpu_processor = processor->getDefaultCPUProcessor();

  return p;
}

OCIO::ConstProcessorRcPtr ColorProcessorOCIO::GetProcessor(const ColorProcessor *processor)
{
  return processor->impl_->processor;
}

#else // OLIVE_ENABLE_OCIO

// ============================================================================
// Patch 0039 (Phase 8): built-in colour engine (OLIVE_ENABLE_OCIO=OFF).
//
// Implements a single ingest -> working (scene-linear) -> display transfer
// function pipeline over a set of named colour spaces:
//
//   * no matrix / gamut conversion (see the HDR note below for the exceptions),
//   * no 3D LUT, no looks, no Lift/Gamma/Gain grading,
//   * unknown colour spaces fall back to "already linear".
//
// Patch 0042 added the BT.2100 HLG and SMPTE ST 2084 (PQ) transfer functions, so
// the "Rec.2020 HLG" / "Rec.2020 PQ" spaces the built-in manager advertises are
// actually interpreted instead of silently passing through as linear. Those two
// spaces exist *only* in the built-in engine: the bundled OCIO config cannot
// express them, and neither can OCIO 2.1 (its fixed-function transform set has
// no PQ or HLG style), so there is no ON-side counterpart to match.
//
// Two honest caveats on the HDR curves:
//   * the two standards use different linear scales - PQ normalises so that
//     1.0 == 10000 cd/m2, HLG so that the inverse OETF maps 1.0 to the nominal
//     1000 cd/m2 peak. Nothing here renormalises them onto a common scale.
//   * the HLG OOTF / system gamma and the BT.2408 & Reinhard display roll-off are
//     NOT applied here. They belong to the GPU display transform (colormanage.frag)
//     and are still outstanding.
// Also note that the Rec.2020 gamut matrix is not applied: as with every other
// space in this engine, values are treated as if they were already in the
// working primaries.
//
// The remaining pieces (gamut matrices, .cube LUTs, Lift/Gamma/Gain grading)
// land in follow-up patches. The engine is therefore NOT expected to match the
// OCIO backend numerically; its acceptance gate is that the tree builds and
// links with zero OpenColorIO symbols, with the colour-equivalence gates
// tracked separately.
// ============================================================================

namespace {

// Patch 0045 (Phase 8): the built-in engine's transfer functions come from
// `render/builtincolorspaces.h`, which is also what `ColorManager` advertises from.
// Aliasing the shared type keeps the call sites below readable while leaving exactly
// one definition of the enumerators.
using TransferFunction = BuiltinTransferFunction;

TransferFunction GetTransferFunction(const QString &name, bool *recognized = nullptr)
{
  TransferFunction tf = TransferFunction::kTransferLinear;
  bool ok = false;

  for (int i = 0; i < kBuiltinColorSpaceCount; i++) {
    if (name == QString::fromUtf8(kBuiltinColorSpaces[i].name)) {
      tf = static_cast<TransferFunction>(kBuiltinColorSpaces[i].transfer);
      ok = true;
      break;
    }
  }

  if (!ok) {
    // "scene_linear" / "Linear Raw" are historical aliases for Linear that the
    // project serializers and OCIO roles emit; "sRGB" is the shorthand the bundled
    // config uses. They resolve to the same maths as their canonical entries.
    if (name == QStringLiteral("scene_linear") || name == QStringLiteral("Linear Raw")) {
      tf = kTransferLinear;
      ok = true;
    } else if (name == QStringLiteral("sRGB")) {
      tf = kTransferSrgb;
      ok = true;
    } else {
      // Patch 0045 (Phase 8): this fall-through is the root cause of the "fake
      // capability" trap - an unrecognised colour space was decoded with a straight
      // passthrough and no diagnostic whatsoever. The behaviour is deliberately kept
      // (linear is the documented fallback) but callers can now detect it via
      // `recognized` and surface an honest warning instead of degrading in silence.
      tf = kTransferLinear;
      ok = false;
    }
  }

  if (recognized) {
    *recognized = ok;
  }

  return tf;
}

double PowSigned(double base, double exponent)
{
  // Transfer functions are extended to negative values symmetrically, which is
  // what OCIO's built-in transfer functions do too.
  return base < 0.0 ? -std::pow(-base, exponent) : std::pow(base, exponent);
}

// --- Patch 0042: HDR transfer functions -------------------------------------

// ITU-R BT.2100 reference HLG OETF constants (Table 5).
const double kHlgA = 0.17883277;
const double kHlgB = 0.28466892;
const double kHlgC = 0.55991073;

// SMPTE ST 2084 (PQ) EOTF constants.
const double kPqM1 = 2610.0 / 16384.0;
const double kPqM2 = 2523.0 / 4096.0 * 128.0;
const double kPqC1 = 3424.0 / 4096.0;
const double kPqC2 = 2413.0 / 4096.0 * 32.0;
const double kPqC3 = 2392.0 / 4096.0 * 32.0;

double HlgToLinear(double signal)
{
  // Inverse BT.2100 HLG OETF (scene light). The branch boundary is signal 0.5
  // (scene 1/12); note the published a/b/c constants are rounded to 8 decimals,
  // so the two branches only agree to ~4e-5 either side of the knee. Negative
  // values are mirrored to match the rest of this engine.
  const double sign = signal < 0.0 ? -1.0 : 1.0;
  const double s = std::fabs(signal);
  const double e = (s <= 0.5) ? (s * s) / 3.0
                              : (std::exp((s - kHlgC) / kHlgA) + kHlgB) / 12.0;
  return sign * e;
}

double HlgFromLinear(double linear)
{
  const double sign = linear < 0.0 ? -1.0 : 1.0;
  const double e = std::fabs(linear);
  const double s = (e <= 1.0 / 12.0) ? std::sqrt(3.0 * e)
                                     : kHlgA * std::log(12.0 * e - kHlgB) + kHlgC;
  return sign * s;
}

double PqToLinear(double signal)
{
  // SMPTE ST 2084 EOTF. The `max(..., 0)` is the standard's own clamp: signals
  // below the minimum representable code value have no luminance.
  const double sign = signal < 0.0 ? -1.0 : 1.0;
  const double s = std::fabs(signal);
  const double p = std::pow(s, 1.0 / kPqM2);
  const double denominator = kPqC2 - kPqC3 * p;
  const double y = (denominator > 0.0)
                       ? std::pow(std::max(p - kPqC1, 0.0) / denominator, 1.0 / kPqM1)
                       : 0.0;
  return sign * y;
}

// --- Patch 0046: the common display-linear reference ------------------------
//
// PQ decodes to a scale where 1.0 == 10000 cd/m2 and HLG to one where 1.0 is the
// nominal 1000 cd/m2 peak, so the two are not directly comparable. The engine
// normalises both onto a single scale whose unit is the BT.2408 HDR reference diffuse
// white (203 cd/m2) - which is also the SDR white point - so that:
//   * SDR content sits at 1.0 unchanged (no regression for existing projects);
//   * the roll-off knee below has a meaningful, standardised anchor.
constexpr double kPqPeakNits = 10000.0;
constexpr double kHlgPeakNits = 1000.0;

// PQ: divide by the 10000 cd/m2 PQ peak, then express in units of 203 cd/m2.
constexpr double kPqLinearToDisplay = kPqPeakNits / kDisplayReferenceWhiteNits;

// HLG: the OOTF is applied first (see HlgApplyOotf), then scaled from the 1000
// cd/m2 nominal peak into units of 203 cd/m2.
constexpr double kHlgLinearToDisplay = kHlgPeakNits / kDisplayReferenceWhiteNits;

// BT.2100 OOTF system gamma for the 1000 cd/m2 reference display. BT.2100 gives
// gamma = 1.2 + 0.42 * log10(Lw / 1000), which reduces to 1.2 at Lw = 1000.
constexpr double kHlgSystemGamma = 1.2;

// Rec.709 luma coefficients (also used by BT.2020 luma derivation below).
constexpr double kLumaCoefR = 0.2627;
constexpr double kLumaCoefG = 0.6780;
constexpr double kLumaCoefB = 0.0593;

/**
 * @brief Apply the BT.2100 HLG OOTF to a linear scene-referred RGB triple.
 *
 * The OOTF is a *luminance* power law applied as a per-channel scale:
 *
 *     Y  = 0.2627 R + 0.6780 G + 0.0593 B
 *     RGB' = Y^(gamma - 1) * RGB
 *
 * The crucial detail is the Y^(gamma-1) *factor*, which is computed once from
 * luminance and then multiplied into each channel. Raising each channel to the power
 * gamma on its own is the classic wrong implementation and shifts hue visibly on
 * saturated colours, so it must not be substituted here.
 */
void HlgApplyOotf(double *rgb)
{
  const double y = kLumaCoefR * rgb[0] + kLumaCoefG * rgb[1] + kLumaCoefB * rgb[2];

  if (y <= 0.0) {
    return;
  }

  const double scale = std::pow(y, kHlgSystemGamma - 1.0);

  rgb[0] *= scale;
  rgb[1] *= scale;
  rgb[2] *= scale;
}

double PqFromLinear(double linear)
{
  const double sign = linear < 0.0 ? -1.0 : 1.0;
  const double y = std::fabs(linear);
  const double p = std::pow(y, kPqM1);
  return sign * std::pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

double ToLinear(double v, TransferFunction tf)
{
  switch (tf) {
  case kTransferSrgb:
    return (v <= 0.04045) ? v / 12.92 : PowSigned((v + 0.055) / 1.055, 2.4);
  case kTransferRec709:
    return (v < 0.081) ? v / 4.5 : PowSigned((v + 0.099) / 1.099, 1.0 / 0.45);
  case kTransferBt1886:
    return PowSigned(v, 2.4);
  case kTransferHlg:
    return HlgToLinear(v);
  case kTransferPq:
    return PqToLinear(v);
  case kTransferLinear:
  case kTransferData:
    break;
  }

  return v;
}

double FromLinear(double v, TransferFunction tf)
{
  switch (tf) {
  case kTransferSrgb:
    return (v <= 0.0031308) ? v * 12.92 : 1.055 * PowSigned(v, 1.0 / 2.4) - 0.055;
  case kTransferRec709:
    return (v < 0.018) ? 4.5 * v : 1.099 * PowSigned(v, 0.45) - 0.099;
  case kTransferBt1886:
    return PowSigned(v, 1.0 / 2.4);
  case kTransferHlg:
    return HlgFromLinear(v);
  case kTransferPq:
    return PqFromLinear(v);
  case kTransferLinear:
  case kTransferData:
    break;
  }

  return v;
}

float FromStorage(const char *p, PixelFormat::Format f)
{
  switch (f) {
  case PixelFormat::U8:
    return float(*reinterpret_cast<const uint8_t *>(p)) / 255.0f;
  case PixelFormat::U16:
    return float(*reinterpret_cast<const uint16_t *>(p)) / 65535.0f;
  case PixelFormat::F16:
    return core::half_to_float(*reinterpret_cast<const uint16_t *>(p));
  case PixelFormat::F32:
    return *reinterpret_cast<const float *>(p);
  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    break;
  }

  return 0.0f;
}

void ToStorage(char *p, PixelFormat::Format f, float v)
{
  switch (f) {
  case PixelFormat::U8:
    *reinterpret_cast<uint8_t *>(p) = uint8_t(qBound(0.0f, v, 1.0f) * 255.0f + 0.5f);
    break;
  case PixelFormat::U16:
    *reinterpret_cast<uint16_t *>(p) = uint16_t(qBound(0.0f, v, 1.0f) * 65535.0f + 0.5f);
    break;
  case PixelFormat::F16:
    *reinterpret_cast<uint16_t *>(p) = core::float_to_half(v);
    break;
  case PixelFormat::F32:
    *reinterpret_cast<float *>(p) = v;
    break;
  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    break;
  }
}

}

struct ColorProcessor::Impl
{
  QString input;
  QString output;
  bool is_display = false;
  QString display_name;
  QString view;
  Direction direction = kNormal;
  std::string id;
};

ColorProcessor::ColorProcessor() :
  impl_(std::make_unique<Impl>())
{
}

ColorProcessor::ColorProcessor(ColorManager *config, const QString &input, const ColorTransform &transform, Direction direction) :
  impl_(std::make_unique<Impl>())
{
  impl_->input = input;
  impl_->direction = direction;

  if (transform.is_display()) {
    impl_->is_display = true;
    impl_->display_name = (transform.output().isEmpty()) ? config->GetDefaultDisplay() : transform.output();
    impl_->view = (transform.view().isEmpty()) ? config->GetDefaultView(impl_->display_name) : transform.view();

    // The built-in engine has no config to resolve a view to a colour space, so
    // it treats the view name as the destination colour space. In the bundled
    // config the two happen to share a name (e.g. view "sRGB OETF" maps to
    // colour space "sRGB OETF").
    impl_->output = impl_->view;
  } else {
    impl_->output = (transform.output().isEmpty()) ? config->GetDefaultDisplay() : transform.output();
  }

  // Patch 0046: an unrecognised endpoint is a hard error, not a silent passthrough.
  // Pixels that "convert" by doing nothing are indistinguishable from a correct
  // conversion to every caller, which is precisely the failure mode the built-in
  // engine was written to eliminate. Legacy projects are no longer supported, so an
  // unknown name here is a programming error.
  //
  // The validator deliberately accepts the historical aliases ("scene_linear",
  // "Linear Raw", "sRGB") that GetTransferFunction() recognises - the shared table
  // holds only canonical entries, so a bare table lookup would wrongly reject them.
  if (!IsBuiltinColorSpace(impl_->input)) {
    qCritical() << "[ColorProcessor] Unknown input colour space:" << impl_->input
                << "- refusing to build a processor that would pass pixels through.";
    std::abort();
  }

  if (!IsBuiltinColorSpace(impl_->output)) {
    qCritical() << "[ColorProcessor] Unknown output colour space:" << impl_->output
                << "- refusing to build a processor that would pass pixels through.";
    std::abort();
  }

  impl_->id = QStringLiteral("builtin|%1|%2|%3|%4")
                .arg(impl_->input,
                     impl_->output,
                     impl_->view,
                     (direction == kNormal) ? QStringLiteral("fwd") : QStringLiteral("inv"))
                .toStdString();
}

ColorProcessor::~ColorProcessor() = default;

void ColorProcessor::ConvertFrame(Frame *f)
{
  const PixelFormat::Format fmt = f->format();

  if (fmt == PixelFormat::INVALID || fmt == PixelFormat::COUNT) {
    qCritical() << "Tried to color convert frame with no format";
    return;
  }

  TransferFunction src = GetTransferFunction(impl_->input);
  TransferFunction dst = GetTransferFunction(impl_->output);

  if (impl_->direction == kInverse) {
    std::swap(src, dst);
  }

  // "Non-Colour Data" is passed through untouched, matching the bundled config
  // where the corresponding view is a no-op.
  if (src == kTransferData || dst == kTransferData) {
    return;
  }

  const int bytes = f->format().byte_count();
  const int channels = f->channel_count();
  const int color_channels = (channels >= 4) ? 3 : channels;

  for (int y = 0; y < f->height(); y++) {
    char *row = f->data() + y * f->linesize_bytes();

    for (int x = 0; x < f->width(); x++) {
      char *pixel = row + x * bytes * channels;

      for (int c = 0; c < color_channels; c++) {
        char *p = pixel + c * bytes;
        float v = FromStorage(p, fmt);
        v = float(FromLinear(ToLinear(double(v), src), dst));
        ToStorage(p, fmt, v);
      }
    }
  }
}

Color ColorProcessor::ConvertColor(const Color& in)
{
  TransferFunction src = GetTransferFunction(impl_->input);
  TransferFunction dst = GetTransferFunction(impl_->output);

  if (impl_->direction == kInverse) {
    std::swap(src, dst);
  }

  if (src == kTransferData || dst == kTransferData) {
    return in;
  }

  return Color(FromLinear(ToLinear(in.red(), src), dst),
               FromLinear(ToLinear(in.green(), src), dst),
               FromLinear(ToLinear(in.blue(), src), dst),
               in.alpha());
}

const char *ColorProcessor::id() const
{
  return impl_->id.c_str();
}

bool ColorProcessor::IsDefaulted() const
{
  // Unreachable in practice: Create() refuses to build a processor whose endpoints
  // are unknown (patch 0046), so any processor a caller can hold is well-formed.
  return false;
}

ColorProcessorPtr ColorProcessor::Create(ColorManager *config, const QString& input, const ColorTransform &transform, Direction direction)
{
  // Patch 0046: fail loudly rather than degrade silently. Under the built-in
  // engine an unrecognised colour space used to fall through to a linear
  // passthrough with no diagnostic at all, which is indistinguishable from a
  // correct conversion to the caller. Legacy projects are no longer supported,
  // so an unknown name here is a programming error, not data to be tolerated.
  const QString in = input;
  const QString out = (transform.is_display())
                          ? ((transform.view().isEmpty())
                                 ? config->GetDefaultView(transform.output().isEmpty()
                                                              ? config->GetDefaultDisplay()
                                                              : transform.output())
                                 : transform.view())
                          : ((transform.output().isEmpty()) ? config->GetDefaultDisplay()
                                                           : transform.output());

  if (!IsBuiltinColorSpace(in)) {
    qCritical() << "[ColorProcessor] Unknown input colour space: " << in;
    return nullptr;
  }

  if (!IsBuiltinColorSpace(out)) {
    qCritical() << "[ColorProcessor] Unknown output colour space: " << out;
    return nullptr;
  }

  return std::make_shared<ColorProcessor>(config, input, transform, direction);
}

void ColorProcessor::ConvertFrame(FramePtr f)
{
  ConvertFrame(f.get());
}

#endif // OLIVE_ENABLE_OCIO

}
