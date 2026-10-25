#include "splashscreen.h"

#include <QApplication>
#include <QPainter>
#include <QLinearGradient>
#include <QScreen>
#include <QGraphicsDropShadowEffect>

namespace olive {

SplashScreen::SplashScreen(QWidget *parent)
  : QWidget(parent, Qt::FramelessWindowHint | Qt::SplashScreen | Qt::WindowStaysOnTopHint),
    status_message_(tr("Starting Olivelan...")),
    version_string_(QCoreApplication::applicationVersion().isEmpty()
                        ? QStringLiteral("v0.1.0-alpha")
                        : QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()))
{
  setAttribute(Qt::WA_TranslucentBackground, false);
  setAttribute(Qt::WA_DeleteOnClose, true);

  setFixedSize(620, 360);

  // Center on active screen
  if (QScreen *primary = QGuiApplication::primaryScreen()) {
    const QRect geom = primary->availableGeometry();
    move(geom.center() - rect().center());
  }
}

void SplashScreen::SetStatus(const QString &message, int progress_percent)
{
  status_message_ = message;
  if (progress_percent >= 0) {
    progress_percent_ = qBound(0, progress_percent, 100);
  }
  update();
  qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void SplashScreen::Finish(QWidget *main_window)
{
  if (main_window) {
    main_window->raise();
    main_window->activateWindow();
  }
  close();
}

void SplashScreen::paintEvent(QPaintEvent *)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  const QRect r = rect();

  // 1. Deep obsidian canvas background
  p.fillRect(r, QColor(0x16, 0x17, 0x1A));

  // 2. Artistic modern gradient glow (Creative Suite aesthetic)
  QLinearGradient bg_grad(0, 0, width(), height());
  bg_grad.setColorAt(0.0, QColor(0x1E, 0x24, 0x2B, 180));
  bg_grad.setColorAt(0.5, QColor(0x16, 0x17, 0x1A, 255));
  bg_grad.setColorAt(1.0, QColor(0x0E, 0x22, 0x26, 200));
  p.fillRect(r, bg_grad);

  // Subtle decorative radial glow in top-left
  QRadialGradient glow(80, 80, 200);
  glow.setColorAt(0.0, QColor(0x00, 0xC4, 0xCC, 40));
  glow.setColorAt(1.0, QColor(0x16, 0x17, 0x1A, 0));
  p.fillRect(r, glow);

  // 3. Hairline border
  p.setPen(QPen(QColor(0x2C, 0x2F, 0x37), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRect(r.adjusted(0, 0, -1, -1));

  // 4. Logo badge (Adobe CC style iconic square badge)
  const QRect badge_rect(44, 44, 60, 60);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x00, 0xC4, 0xCC));
  p.drawRoundedRect(badge_rect, 6, 6);

  // "Ol" badge text
  p.setPen(QColor(0x04, 0x17, 0x19));
  QFont badge_font = p.font();
  badge_font.setBold(true);
  badge_font.setPixelSize(30);
  p.setFont(badge_font);
  p.drawText(badge_rect, Qt::AlignCenter, QStringLiteral("Ol"));

  // 5. Title & Subtitle
  // App Title
  p.setPen(QColor(0xF2, 0xF4, 0xF7));
  QFont title_font = p.font();
  title_font.setBold(true);
  title_font.setPixelSize(28);
  title_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
  p.setFont(title_font);
  p.drawText(120, 72, QStringLiteral("Olivelan"));

  // Subtitle / Architecture tag
  p.setPen(QColor(0x00, 0xC4, 0xCC));
  QFont tag_font = p.font();
  tag_font.setBold(false);
  tag_font.setPixelSize(12);
  tag_font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
  p.setFont(tag_font);
  p.drawText(122, 94, QStringLiteral("GPU-RESIDENT HIGH PERFORMANCE NLE"));

  // 6. Center feature banner / creative illustration area
  p.setPen(QColor(0x73, 0x79, 0x85));
  QFont info_font = p.font();
  info_font.setPixelSize(11);
  info_font.setLetterSpacing(QFont::PercentageSpacing, 100);
  p.setFont(info_font);
  p.drawText(46, 170, QStringLiteral("OpenColorIO 2.5.1 Single-Track • Tier 1/2 Hardware Zero-Copy HAL • Metro Fluent UI"));

  // 7. Status message & progress bar
  // Status message
  p.setPen(QColor(0xA8, 0xAD, 0xB8));
  QFont status_font = p.font();
  status_font.setPixelSize(11);
  p.setFont(status_font);
  p.drawText(46, 290, status_message_);

  // Ultra-thin 2px progress bar
  const int bar_x = 44;
  const int bar_y = 302;
  const int bar_width = width() - 88;
  const int bar_height = 2;

  // Background track
  p.fillRect(bar_x, bar_y, bar_width, bar_height, QColor(0x23, 0x25, 0x2B));

  // Progress fill
  if (progress_percent_ >= 0) {
    const int fill_w = static_cast<int>((bar_width * progress_percent_) / 100.0);
    p.fillRect(bar_x, bar_y, fill_w, bar_height, QColor(0x00, 0xC4, 0xCC));
  } else {
    // Indeterminate subtle pulse
    p.fillRect(bar_x, bar_y, bar_width / 3, bar_height, QColor(0x00, 0xC4, 0xCC));
  }

  // 8. Bottom legal & version footer
  p.setPen(QColor(0x55, 0x5A, 0x64));
  QFont footer_font = p.font();
  footer_font.setPixelSize(10);
  p.setFont(footer_font);
  p.drawText(44, 334, QStringLiteral("© 2024-2026 Olivelan Contributors. All rights reserved."));

  // Version identifier on bottom right
  p.drawText(QRect(width() - 200, 322, 156, 16), Qt::AlignRight | Qt::AlignVCenter, version_string_);
}

} // namespace olive
