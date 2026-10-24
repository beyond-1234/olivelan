#include "ociofiletransform.h"

#include <QFileInfo>
#include <QDebug>

#include "common/ociobridge.h"
#include "common/ocioutils.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"

namespace olive {

const QString OCIOFileTransformNode::kFileInput = QStringLiteral("file_in");
const QString OCIOFileTransformNode::kDirectionInput = QStringLiteral("dir_in");
const QString OCIOFileTransformNode::kInterpolationInput = QStringLiteral("interp_in");

#define super OCIOBaseNode

OCIOFileTransformNode::OCIOFileTransformNode()
{
  AddInput(kFileInput, NodeValue::kFile, QString(), InputFlags(kInputFlagNotKeyframable | kInputFlagNotConnectable));

  AddInput(kDirectionInput, NodeValue::kCombo, 0, InputFlags(kInputFlagNotKeyframable | kInputFlagNotConnectable));

  AddInput(kInterpolationInput, NodeValue::kCombo, 0, InputFlags(kInputFlagNotKeyframable | kInputFlagNotConnectable));
}

QString OCIOFileTransformNode::Name() const
{
  return tr("LUT (OpenColorIO)");
}

QString OCIOFileTransformNode::id() const
{
  return QStringLiteral("org.olivevideoeditor.Olive.ociofiletransform");
}

QVector<Node::CategoryID> OCIOFileTransformNode::Category() const
{
  return {kCategoryColor};
}

QString OCIOFileTransformNode::Description() const
{
  return tr("Applies a 1D or 3D LUT (.cube, .spi1d, .spi3d, etc.) using OpenColorIO.");
}

void OCIOFileTransformNode::Retranslate()
{
  super::Retranslate();

  SetInputName(kTextureInput, tr("Input"));
  SetInputName(kFileInput, tr("File"));
  SetInputName(kDirectionInput, tr("Direction"));
  SetComboBoxStrings(kDirectionInput, {tr("Forward"), tr("Inverse")});
  SetInputName(kInterpolationInput, tr("Interpolation"));
  SetComboBoxStrings(kInterpolationInput, {
      tr("Default / Auto"),
      tr("Linear"),
      tr("Tetrahedral"),
      tr("Nearest"),
      tr("Best")
  });
}

void OCIOFileTransformNode::InputValueChangedEvent(const QString &input, int element)
{
  super::InputValueChangedEvent(input, element);

  if (input == kFileInput || input == kDirectionInput || input == kInterpolationInput) {
    GenerateProcessor();
  }
}

QString OCIOFileTransformNode::GetFile() const
{
  return GetStandardValue(kFileInput).toString();
}

ColorProcessor::Direction OCIOFileTransformNode::GetDirection() const
{
  return static_cast<ColorProcessor::Direction>(GetStandardValue(kDirectionInput).toInt());
}

OCIOFileTransformNode::Interpolation OCIOFileTransformNode::GetInterpolation() const
{
  return static_cast<Interpolation>(GetStandardValue(kInterpolationInput).toInt());
}

void OCIOFileTransformNode::ConfigChanged()
{
  GenerateProcessor();
}

void OCIOFileTransformNode::GenerateProcessor()
{
  if (!manager()) {
    last_error_.clear();
    SetInputProperty(kFileInput, QStringLiteral("error"), QString());
    set_processor(nullptr);
    return;
  }

  const QString raw_path = GetFile().trimmed();
  if (raw_path.isEmpty()) {
    last_error_.clear();
    SetInputProperty(kFileInput, QStringLiteral("error"), QString());
    set_processor(nullptr);
    return;
  }

  const QString resolved_path = OCIOUtils::ResolveProjectRelativePath(project(), raw_path);

  if (!QFileInfo::exists(resolved_path)) {
    last_error_ = tr("LUT file does not exist: %1").arg(raw_path);
    qWarning() << "[OCIOFileTransform]" << last_error_;
    SetInputProperty(kFileInput, QStringLiteral("error"), last_error_);
    set_processor(nullptr);
    return;
  }

  try {
    auto ocio_config = ColorManagerOCIO::GetConfig(manager());
    if (!ocio_config) {
      last_error_ = tr("No active OpenColorIO configuration.");
      SetInputProperty(kFileInput, QStringLiteral("error"), last_error_);
      set_processor(nullptr);
      return;
    }

    auto file_transform = OCIO::FileTransform::Create();
    file_transform->setSrc(resolved_path.toUtf8().constData());
    file_transform->setDirection(GetDirection() == ColorProcessor::kNormal
                                     ? OCIO::TRANSFORM_DIR_FORWARD
                                     : OCIO::TRANSFORM_DIR_INVERSE);

    OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
    switch (GetInterpolation()) {
    case kInterpLinear:
      interp = OCIO::INTERP_LINEAR;
      break;
    case kInterpTetrahedral:
      interp = OCIO::INTERP_TETRAHEDRAL;
      break;
    case kInterpNearest:
      interp = OCIO::INTERP_NEAREST;
      break;
    case kInterpBest:
      interp = OCIO::INTERP_BEST;
      break;
    case kInterpDefault:
    default:
      interp = OCIO::INTERP_DEFAULT;
      break;
    }
    file_transform->setInterpolation(interp);

    auto ocio_processor = ocio_config->getProcessor(file_transform);
    if (!ocio_processor) {
      last_error_ = tr("OpenColorIO failed to create processor for LUT: %1").arg(raw_path);
      SetInputProperty(kFileInput, QStringLiteral("error"), last_error_);
      set_processor(nullptr);
      return;
    }

    last_error_.clear();
    SetInputProperty(kFileInput, QStringLiteral("error"), QString());
    set_processor(ColorProcessorOCIO::Create(ocio_processor));
  } catch (const OCIO::Exception &e) {
    last_error_ = QString::fromUtf8(e.what());
    qWarning() << "[OCIOFileTransform] Failed to load LUT" << raw_path << ":" << last_error_;
    SetInputProperty(kFileInput, QStringLiteral("error"), last_error_);
    set_processor(nullptr);
  }
}

} // namespace olive
