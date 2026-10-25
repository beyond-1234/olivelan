#ifndef OLIVELAN_SPLASHSCREEN_H
#define OLIVELAN_SPLASHSCREEN_H

#include <QWidget>
#include <QString>

namespace olive {

class SplashScreen : public QWidget {
  Q_OBJECT

public:
  explicit SplashScreen(QWidget *parent = nullptr);
  ~SplashScreen() override = default;

  void SetStatus(const QString &message, int progress_percent = -1);
  void Finish(QWidget *main_window);

protected:
  void paintEvent(QPaintEvent *event) override;

  void changeEvent(QEvent *event) override;

private:
  QString status_message_;
  int progress_percent_{-1};
  QString version_string_;
};

} // namespace olive

#endif // OLIVELAN_SPLASHSCREEN_H
