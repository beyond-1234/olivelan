#ifndef EXPORTADVANCEDVIDEODIALOG_H
#define EXPORTADVANCEDVIDEODIALOG_H

#include <QComboBox>
#include <QDialog>

#include "codec/encoder.h"
#include "widget/slider/integerslider.h"

namespace olive {

class ExportAdvancedVideoDialog : public QDialog
{
  Q_OBJECT
public:
  ExportAdvancedVideoDialog(const QList<QString>& pix_fmts,
                            QWidget* parent = nullptr);

  int threads() const
  {
    return static_cast<int>(thread_slider_->GetValue());
  }

  void set_threads(int t)
  {
    thread_slider_->SetValue(t);
  }

  QString pix_fmt() const
  {
    return pixel_format_combobox_->currentText();
  }

  void set_pix_fmt(const QString& s)
  {
    pixel_format_combobox_->setCurrentText(s);
  }

  VideoParams::ColorRange yuv_range() const
  {
    // Auto is the export default; the encoder converts to limited range regardless,
    // so present/round-trip it as the explicit limited choice.
    return (yuv_color_range_combobox_->currentData().toInt() == VideoParams::kColorRangeFull)
               ? VideoParams::kColorRangeFull
               : VideoParams::kColorRangeLimited;
  }

  void set_yuv_range(VideoParams::ColorRange i)
  {
    const int index = yuv_color_range_combobox_->findData(
        i == VideoParams::kColorRangeFull ? VideoParams::kColorRangeFull
                                          : VideoParams::kColorRangeLimited);
    yuv_color_range_combobox_->setCurrentIndex(index >= 0 ? index : 0);
  }

private:
  IntegerSlider* thread_slider_;

  QComboBox* pixel_format_combobox_;

  QComboBox* yuv_color_range_combobox_;

};

}

#endif // EXPORTADVANCEDVIDEODIALOG_H
