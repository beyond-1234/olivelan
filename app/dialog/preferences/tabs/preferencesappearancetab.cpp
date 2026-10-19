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

#include "preferencesappearancetab.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>

#include "node/node.h"
#include "widget/colorbutton/colorbutton.h"
#include "widget/menu/menushared.h"
#include "ui/colorcoding.h"
#include "ui/style/thememanager.h"

namespace olive {

PreferencesAppearanceTab::PreferencesAppearanceTab()
{
  QVBoxLayout* layout = new QVBoxLayout(this);

  QGridLayout* appearance_layout = new QGridLayout();
  layout->addLayout(appearance_layout);

  int row = 0;

  // Appearance -> Theme
  appearance_layout->addWidget(new QLabel(tr("Theme")), row, 0);

  style_combobox_ = new QComboBox();

  {
    // Populate built-in styles and any external/community discovered themes
    QStringList all_theme_ids = ThemeManager::instance()->GetAvailableThemeIds();
    for (const QString &tid : all_theme_ids) {
      ThemeInfo tinfo = ThemeManager::instance()->GetThemeInfo(tid);
      QString display_text = tinfo.name;
      if (tinfo.is_pure_qss) {
        display_text += tr(" (Community QSS)");
      } else if (!tinfo.is_builtin) {
        display_text += tr(" (Custom Theme)");
      }
      style_combobox_->addItem(display_text, tid);

      if (ThemeManager::instance()->CurrentThemeId() == tid || StyleManager::GetStyle() == tid) {
        style_combobox_->setCurrentIndex(style_combobox_->count() - 1);
      }
    }
  }

  appearance_layout->addWidget(style_combobox_, row, 1);

  // Add "Import Theme / QSS..." button directly on appearance tab
  QPushButton* import_theme_btn = new QPushButton(tr("Import Theme / QSS..."));
  connect(import_theme_btn, &QPushButton::clicked, this, [this]() {
    QString file = QFileDialog::getOpenFileName(this,
                                                tr("Import Theme File"),
                                                QString(),
                                                tr("Olive Themes & QSS (*.qss *.olivetheme);;All Files (*.*)"));
    if (!file.isEmpty()) {
      if (ThemeManager::instance()->ImportThemePackage(file)) {
        // Refresh combobox
        style_combobox_->clear();
        QStringList all_theme_ids = ThemeManager::instance()->GetAvailableThemeIds();
        for (const QString &tid : all_theme_ids) {
          ThemeInfo tinfo = ThemeManager::instance()->GetThemeInfo(tid);
          QString display_text = tinfo.name;
          if (tinfo.is_pure_qss) display_text += tr(" (Community QSS)");
          else if (!tinfo.is_builtin) display_text += tr(" (Custom Theme)");
          style_combobox_->addItem(display_text, tid);
          if (ThemeManager::instance()->CurrentThemeId() == tid) {
            style_combobox_->setCurrentIndex(style_combobox_->count() - 1);
          }
        }
      }
    }
  });
  appearance_layout->addWidget(import_theme_btn, row, 2);

  row++;

  {
    QGroupBox* color_group = new QGroupBox();
    color_group->setTitle(tr("Default Node Colors"));

    QGridLayout* color_layout = new QGridLayout(color_group);

    for (int i=0; i<Node::kCategoryCount; i++) {
      QString cat_name = Node::GetCategoryName(static_cast<Node::CategoryID>(i));
      color_layout->addWidget(new QLabel(cat_name), i, 0);

      ColorCodingComboBox* ccc = new ColorCodingComboBox();
      ccc->SetColor(OLIVE_CONFIG_STR(QStringLiteral("CatColor%1").arg(i)).toInt());
      color_layout->addWidget(ccc, i, 1);
      color_btns_.append(ccc);
    }

    appearance_layout->addWidget(color_group, row, 0, 1, 2);
  }

  row++;
  {
    QGroupBox* marker_group = new QGroupBox();
    marker_group->setTitle(tr("Miscellaneous"));

    QGridLayout* marker_layout = new QGridLayout(marker_group);

    marker_layout->addWidget(new QLabel("Default Marker Color"), 0, 0);

    marker_btn_ = new ColorCodingComboBox();
    marker_btn_->SetColor(OLIVE_CONFIG("MarkerColor").toInt());
    marker_layout->addWidget(marker_btn_, 0, 1);

    appearance_layout->addWidget(marker_group, row, 0, 1, 2);
  }

  layout->addStretch();
}

void PreferencesAppearanceTab::Accept(MultiUndoCommand *command)
{
  Q_UNUSED(command)

  QString style_path = style_combobox_->currentData().toString();

  if (style_path != ThemeManager::instance()->CurrentThemeId() && style_path != StyleManager::GetStyle()) {
    ThemeManager::instance()->ApplyTheme(style_path);
    OLIVE_CONFIG("Style") = style_path;
  }

  for (int i=0; i<color_btns_.size(); i++) {
    OLIVE_CONFIG_STR(QStringLiteral("CatColor%1").arg(i)) = color_btns_.at(i)->GetSelectedColor();
  }

  OLIVE_CONFIG("MarkerColor") = marker_btn_->GetSelectedColor();
}

}
