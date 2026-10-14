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

enum TransferFunction
{
  kTransferLinear,
  kTransferSrgb,
  kTransferRec709,
  kTransferBt1886,
  kTransferHlg,
  kTransferPq,
  kTransferData
};

TransferFunction GetTransferFunction(const QString &name)
{
  if (name == QStringLiteral("Linear")
      || name == QStringLiteral("scene_linear")
      || name == QStringLiteral("Linear Raw")) {
    return kTransferLinear;
  }

  if (name == QStringLiteral("sRGB OETF") || name == QStringLiteral("sRGB")) {
    return kTransferSrgb;
  }

  if (name == QStringLiteral("Rec.709 OETF")) {
    return kTransferRec709;
  }

  if (name == QStringLiteral("BT.1886 EOTF")) {
    return kTransferBt1886;
  }

  if (name == QStringLiteral("Rec.2020 HLG")) {
    return kTransferHlg;
  }

  if (name == QStringLiteral("Rec.2020 PQ")) {
    return kTransferPq;
  }

  if (name == QStringLiteral("Non-Colour Data")) {
    return kTransferData;
  }

  return kTransferLinear;
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

ColorProcessorPtr ColorProcessor::Create(ColorManager *config, const QString& input, const ColorTransform &transform, Direction direction)
{
  return std::make_shared<ColorProcessor>(config, input, transform, direction);
}

void ColorProcessor::ConvertFrame(FramePtr f)
{
  ConvertFrame(f.get());
}

#endif // OLIVE_ENABLE_OCIO

}
