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

  /**
   * @brief Patch 0080 (Phase 9): offers real alternatives when the requested backend cannot work.
   *
   * @return the backend id to use instead (may be the requested one if the user insists), or an
   *         empty string if the user cancelled and nothing should be applied.
   */
  QString ResolveUnavailableBackend(const QString& requested);

  void PopulateProbeInfo();
  void PopulateBackendCombo();

  /// Patch 0090 (P2-1): recompute the "will actually use" sentence from the current combo entry.
  void UpdateEffectivePathLabel();

  static QString VendorToString(GpuVendor vendor);
  static QString DisplayServerToString(DisplayServer server);

  QTreeWidget* probe_tree_ = nullptr;
  QComboBox* backend_combo_ = nullptr;
  QLabel* effective_label_ = nullptr;
  QString selected_backend_;
};

} // namespace olive

#endif // HALBACKENDDIALOG_H
