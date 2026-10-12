#ifndef HALBACKENDDIALOG_H
#define HALBACKENDDIALOG_H

#include <QDialog>

#include "codec/hal/gpu_environment_probe.h"

class QComboBox;
class QLabel;
class QTreeWidget;

namespace olive {

/**
 * @brief Startup dialog showing GPU environment probe results and letting the
 *        user force a HAL backend for this process (phase5-07).
 *
 * Opened via the -hal-dialog command line switch before Core initialization.
 * The selected backend is applied through HardwarePipelineFactory::SetForcedBackend
 * and is process-wide, non-persistent.
 */
class HALBackendDialog : public QDialog {
  Q_OBJECT
public:
  HALBackendDialog(QWidget* parent = nullptr);

  /**
   * @brief Backend selected by the user; empty string means "auto".
   */
  QString SelectedBackend() const { return selected_backend_; }

private:
  QWidget* CreateProbeInfoSection();
  QWidget* CreateSelectionSection();

  void PopulateProbeInfo();
  void PopulateBackendCombo();

  static QString VendorToString(GpuVendor vendor);
  static QString DisplayServerToString(DisplayServer server);

  QTreeWidget* probe_tree_ = nullptr;
  QComboBox* backend_combo_ = nullptr;
  QString selected_backend_;
};

} // namespace olive

#endif // HALBACKENDDIALOG_H
