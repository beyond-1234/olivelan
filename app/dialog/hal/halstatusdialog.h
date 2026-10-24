#ifndef HALSTATUSDIALOG_H
#define HALSTATUSDIALOG_H

#include <QDialog>

class QTreeWidget;
class QLabel;

namespace olive {

/**
 * @brief Read-only HAL decode status panel.
 *
 * Shows Tier 1/2/3 availability and the concrete reason behind each state, plus the
 * expected routing and the backend selected by the most recent frame. It is intentionally
 * separate from HALBackendDialog: inspecting the status must not force a backend or
 * restart the process.
 */
class HALStatusDialog : public QDialog {
  Q_OBJECT
public:
  explicit HALStatusDialog(QWidget* parent = nullptr);

private slots:
  void PopulateStatus();

private:
  QTreeWidget* tiers_{nullptr};
  QLabel* expected_{nullptr};
  QLabel* forced_label_{nullptr};
  QLabel* selected_label_{nullptr};
};

} // namespace olive

#endif // HALSTATUSDIALOG_H
