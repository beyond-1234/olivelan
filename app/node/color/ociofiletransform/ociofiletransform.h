#ifndef OCIOFILETRANSFORMNODE_H
#define OCIOFILETRANSFORMNODE_H

#include "node/color/ociobase/ociobase.h"
#include "render/colorprocessor.h"

namespace olive {

class OCIOFileTransformNode : public OCIOBaseNode
{
  Q_OBJECT
public:
  enum Interpolation {
    kInterpDefault = 0,
    kInterpLinear,
    kInterpTetrahedral,
    kInterpNearest,
    kInterpBest
  };

  OCIOFileTransformNode();

  NODE_DEFAULT_FUNCTIONS(OCIOFileTransformNode)

  virtual QString Name() const override;
  virtual QString id() const override;
  virtual QVector<CategoryID> Category() const override;
  virtual QString Description() const override;

  virtual void Retranslate() override;
  virtual void InputValueChangedEvent(const QString &input, int element) override;

  QString GetFile() const;
  ColorProcessor::Direction GetDirection() const;
  Interpolation GetInterpolation() const;

  static const QString kFileInput;
  static const QString kDirectionInput;
  static const QString kInterpolationInput;

  virtual bool PassThroughWhenMissingColorProcessor() const override { return true; }

  QString GetLastError() const { return last_error_; }
  bool HasError() const { return !last_error_.isEmpty(); }

protected slots:
  virtual void ConfigChanged() override;

private:
  void GenerateProcessor();

  QString last_error_;
};

} // namespace olive

#endif // OCIOFILETRANSFORMNODE_H
