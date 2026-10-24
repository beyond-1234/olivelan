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
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

namespace olive {

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
  const QString output = transform.is_display()
      ? (transform.display().isEmpty() ? config->GetDefaultDisplay() : transform.display())
      : (transform.output().isEmpty() ? config->GetDefaultInputColorSpace() : transform.output());
  const OCIO::ConstConfigRcPtr ocio_config = ColorManagerOCIO::GetConfig(config);

  if (transform.is_display()) {

    const QString view = (transform.view().isEmpty()) ? config->GetDefaultView(output) : transform.view();

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

    if (direction == kNormal) {
      impl_->processor = ocio_config->getProcessor(input.toUtf8(), output.toUtf8());
    } else {
      impl_->processor = ocio_config->getProcessor(output.toUtf8(), input.toUtf8());
    }

  }

  // Do not substitute a raw identity transform when OCIO cannot resolve the
  // requested conversion. A silent identity would display an incorrect image
  // while making the project look healthy. Create() catches the exception and
  // returns nullptr so callers can report the actual failure.
  impl_->cpu_processor = impl_->processor->getDefaultCPUProcessor();
}

ColorProcessor::~ColorProcessor() = default;

void ColorProcessor::ConvertFrame(Frame *f)
{
  if (!impl_->cpu_processor) {
    return;
  }

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
  if (!impl_->cpu_processor) {
    return in;
  }

  // I've been bamboozled
  float c[4] = {float(in.red()), float(in.green()), float(in.blue()), float(in.alpha())};

  impl_->cpu_processor->applyRGBA(c);

  return Color(c[0], c[1], c[2], c[3]);
}

const char *ColorProcessor::id() const
{
  return impl_->processor ? impl_->processor->getCacheID() : "invalid";
}

bool ColorProcessor::IsDefaulted() const
{
  // A successfully constructed processor always has an OCIO processor. This
  // accessor exists for callers that receive a processor from a context where
  // construction may have failed.
  return !impl_->processor;
}

ColorProcessorPtr ColorProcessor::Create(ColorManager *config, const QString& input, const ColorTransform &transform, Direction direction)
{
  try {
    return std::make_shared<ColorProcessor>(config, input, transform, direction);
  } catch (const OCIO::Exception &e) {
    qWarning() << "[Color] Failed to create processor for" << input << "->"
               << (transform.is_display() ? transform.display() : transform.output())
               << ":" << e.what();
  } catch (const std::exception &e) {
    qWarning() << "[Color] Failed to create processor:" << e.what();
  }

  return nullptr;
}

void ColorProcessor::ConvertFrame(FramePtr f)
{
  ConvertFrame(f.get());
}

ColorProcessorPtr ColorProcessorOCIO::Create(OCIO::ConstProcessorRcPtr processor)
{
  if (!processor) {
    return nullptr;
  }

  ColorProcessorPtr p(new ColorProcessor());

  p->impl_->processor = processor;
  p->impl_->cpu_processor = processor->getDefaultCPUProcessor();

  return p;
}

OCIO::ConstProcessorRcPtr ColorProcessorOCIO::GetProcessor(const ColorProcessor *processor)
{
  return processor->impl_->processor;
}



}
