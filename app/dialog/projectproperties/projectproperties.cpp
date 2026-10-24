/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2020 Olive Team

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

#include "projectproperties.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

#include "common/filefunctions.h"
#include "common/ociobridge.h"
#include "common/ocioutils.h"
#include "node/color/colormanager/colormanager.h"
#include "render/diskmanager.h"

namespace olive {

#define super QDialog

ProjectPropertiesDialog::ProjectPropertiesDialog(Project* p, QWidget *parent) :
  super(parent),
  working_project_(p),
  ocio_config_is_valid_(true)
{
  QVBoxLayout* layout = new QVBoxLayout(this);

  setWindowTitle(tr("Project Properties for '%1'").arg(working_project_->name()));

  QTabWidget* tabs = new QTabWidget;
  layout->addWidget(tabs);

  {
    // Color management group
    QWidget* color_group = new QWidget();

    QVBoxLayout* color_outer_layout = new QVBoxLayout(color_group);

    QGridLayout* color_layout = new QGridLayout();
    color_outer_layout->addLayout(color_layout);

    int row = 0;

    color_layout->addWidget(new QLabel(tr("OpenColorIO Configuration:")), row, 0);

    ocio_filename_ = new QLineEdit();
    ocio_filename_->setPlaceholderText(tr("(default)"));
    color_layout->addWidget(ocio_filename_, row, 1);

    row++;

    color_layout->addWidget(new QLabel(tr("Default Input Color Space:")), row, 0);

    default_input_colorspace_ = new QComboBox();
    color_layout->addWidget(default_input_colorspace_, row, 1, 1, 2);

    row++;

    color_layout->addWidget(new QLabel(tr("Reference Space:")), row, 0);

    reference_space_ = new QComboBox(this);
    reference_space_->addItem(tr("Scene Linear"), OCIOUtils::kReferenceSpaceSceneLinear);
    reference_space_->addItem(tr("Compositing Log"), OCIOUtils::kReferenceSpaceCompositingLog);
    QtUtils::SetComboBoxData(reference_space_, p->GetColorReferenceSpace());
    color_layout->addWidget(reference_space_, row, 1, 1, 2);

    row++;

    QPushButton* browse_btn = new QPushButton(tr("Browse"));
    color_layout->addWidget(browse_btn, 0, 2);
    connect(browse_btn, &QPushButton::clicked, this, &ProjectPropertiesDialog::BrowseForOCIOConfig);

    ocio_filename_->setText(working_project_->color_manager()->GetConfigFilename());

    connect(ocio_filename_, &QLineEdit::editingFinished, this, &ProjectPropertiesDialog::OCIOFilenameUpdated);
    OCIOFilenameUpdated();

    tabs->addTab(color_group, tr("Color Management"));

    color_outer_layout->addStretch();
  }

  {
    // Cache group
    QWidget* cache_group = new QWidget();

    QVBoxLayout* cache_layout = new QVBoxLayout(cache_group);

    QButtonGroup* disk_cache_btn_group = new QButtonGroup();

    // Create radio buttons and add to widget and button group
    disk_cache_radios_[Project::kCacheUseDefaultLocation] = new QRadioButton(tr("Use Default Location"));
    disk_cache_radios_[Project::kCacheStoreAlongsideProject] = new QRadioButton(tr("Store Alongside Project"));
    disk_cache_radios_[Project::kCacheCustomPath] = new QRadioButton(tr("Use Custom Location:"));
    for (int i=0; i<kDiskCacheRadioCount; i++) {
      disk_cache_btn_group->addButton(disk_cache_radios_[i]);
      cache_layout->addWidget(disk_cache_radios_[i]);
    }

    // Create custom cache path widget
    custom_cache_path_ = new PathWidget(working_project_->GetCustomCachePath(), this);
    custom_cache_path_->setEnabled(false);
    cache_layout->addWidget(custom_cache_path_);

    // Ensure custom cache path "enabled" is tied to the radio button being checked
    connect(disk_cache_radios_[Project::kCacheCustomPath], &QRadioButton::toggled, custom_cache_path_, &PathWidget::setEnabled);

    // Check the radio button that should currently be active
    disk_cache_radios_[working_project_->GetCacheLocationSetting()]->setChecked(true);

    // Add disk cache settings button
    QPushButton* disk_cache_settings_btn = new QPushButton(tr("Disk Cache Settings"));
    connect(disk_cache_settings_btn, &QPushButton::clicked, this, &ProjectPropertiesDialog::OpenDiskCacheSettings);
    cache_layout->addWidget(disk_cache_settings_btn);

    tabs->addTab(cache_group, tr("Disk Cache"));
  }

  QDialogButtonBox* dialog_btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                       Qt::Horizontal);
  layout->addWidget(dialog_btns);
  connect(dialog_btns, &QDialogButtonBox::accepted, this, &ProjectPropertiesDialog::accept);
  connect(dialog_btns, &QDialogButtonBox::rejected, this, &ProjectPropertiesDialog::reject);
}

void ProjectPropertiesDialog::accept()
{
  const QString new_ocio_file = ocio_filename_->text().trimmed();
  const bool ocio_changed = (working_project_->color_manager()->GetConfigFilename() != new_ocio_file);

  if (ocio_changed && !ocio_config_is_valid_) {
    QMessageBox mb(this);
    mb.setWindowModality(Qt::WindowModal);
    mb.setIcon(QMessageBox::Critical);
    mb.setWindowTitle(tr("OpenColorIO Config Error"));
    mb.setText(tr("Failed to set OpenColorIO configuration: %1").arg(ocio_config_error_));
    mb.addButton(QMessageBox::Ok);
    mb.exec();
    return;
  }

  if (disk_cache_radios_[Project::kCacheUseDefaultLocation]->isChecked()) {
    // Keep new cache path empty, which means default
  } else if (disk_cache_radios_[Project::kCacheStoreAlongsideProject]->isChecked()) {
    // Ensure alongside project path is valid
    if (!VerifyPathAndWarnIfBad(working_project_->get_cache_alongside_project_path())) {
      return;
    }
  } else {
    // Ensure custom path is valid
    if (!VerifyPathAndWarnIfBad(custom_cache_path_->text())) {
      return;
    }
  }

  if (custom_cache_path_->text() != working_project_->GetCustomCachePath()) {
    // Check if the user is okay with invalidating the current cache
    if (!DiskManager::ShowDiskCacheChangeConfirmationDialog(this)) {
      return;
    }

    working_project_->SetCustomCachePath(custom_cache_path_->text());

    emit DiskManager::instance()->InvalidateProject(working_project_);
  }

  // This should ripple changes throughout the graph/cache that the color config has changed, and
  // therefore should be done after the cache path is changed
  if (working_project_->color_manager()->GetConfigFilename() != new_ocio_file) {
    working_project_->color_manager()->SetConfigFilename(new_ocio_file);
  }
  // An unreadable configuration leaves the combo box empty. Writing that empty string back would
  // erase the project's default input color space, so only apply a real selection.
  if (default_input_colorspace_->count() > 0
      && working_project_->color_manager()->GetDefaultInputColorSpace() != default_input_colorspace_->currentText()) {
    working_project_->color_manager()->SetDefaultInputColorSpace(default_input_colorspace_->currentText());
  }
  if (working_project_->GetColorReferenceSpace() != reference_space_->currentData().toString()) {
    working_project_->SetColorReferenceSpace(reference_space_->currentData().toString());
  }

  super::accept();
}

bool ProjectPropertiesDialog::VerifyPathAndWarnIfBad(const QString &path)
{
  if (!FileFunctions::DirectoryIsValid(path)) {
    QMessageBox mb(this);
    mb.setWindowModality(Qt::WindowModal);
    mb.setIcon(QMessageBox::Critical);
    mb.setWindowTitle(tr("Invalid path"));
    mb.setText(tr("The custom cache path is invalid. Please check it and try again."));
    mb.addButton(QMessageBox::Ok);
    mb.exec();
    return false;
  }

  return true;
}

void ProjectPropertiesDialog::BrowseForOCIOConfig()
{
  QString fn = QFileDialog::getOpenFileName(
      this,
      tr("Browse for OpenColorIO configuration"),
      QString(),
      tr("OpenColorIO Configurations (*.ocio);;All Files (*)"));
  if (!fn.isEmpty()) {
    ocio_filename_->setText(fn);
    OCIOFilenameUpdated();
  }
}

void ProjectPropertiesDialog::OCIOFilenameUpdated()
{
  default_input_colorspace_->clear();

  try {
    OCIO::ConstConfigRcPtr c;
    const QString fn = ocio_filename_->text().trimmed();

    if (fn.isEmpty()) {
      c = ColorManagerOCIO::GetDefaultConfig();
    } else {
      const QString resolved = OCIOUtils::ResolveProjectRelativePath(working_project_, fn);
      if (!QFileInfo::exists(resolved)) {
        throw OCIO::Exception(QString("File does not exist: %1").arg(fn).toUtf8().constData());
      }
      c = ColorManagerOCIO::CreateConfigFromFile(resolved);
      if (!c) {
        throw OCIO::Exception("Failed to read configuration");
      }
      c->validate();
    }

    ocio_filename_->setStyleSheet(QString());
    ocio_config_is_valid_ = true;
    ocio_config_error_.clear();

    // List input color spaces
    QStringList input_cs = ColorManagerOCIO::ListAvailableColorspaces(c);

    foreach (QString cs, input_cs) {
      default_input_colorspace_->addItem(cs);

      if (cs == working_project_->color_manager()->GetDefaultInputColorSpace()) {
        default_input_colorspace_->setCurrentIndex(default_input_colorspace_->count()-1);
      }
    }
  } catch (const OCIO::Exception& e) {
    ocio_config_is_valid_ = false;
    ocio_filename_->setStyleSheet(QStringLiteral("QLineEdit {color: red;}"));
    ocio_config_error_ = QString::fromUtf8(e.what());

    // The requested configuration cannot be read, but the project keeps rendering with the
    // configuration that is currently active. Show that one instead of leaving the combo empty,
    // which would neither reflect the effective color management nor be safe to write back.
    foreach (const QString& cs, working_project_->color_manager()->ListAvailableColorspaces()) {
      default_input_colorspace_->addItem(cs);

      if (cs == working_project_->color_manager()->GetDefaultInputColorSpace()) {
        default_input_colorspace_->setCurrentIndex(default_input_colorspace_->count() - 1);
      }
    }
  }
}

void ProjectPropertiesDialog::OpenDiskCacheSettings()
{
  if (disk_cache_radios_[Project::kCacheUseDefaultLocation]->isChecked()) {
    DiskManager::instance()->ShowDiskCacheSettingsDialog(DiskManager::instance()->GetDefaultCacheFolder(), this);
  } else if (disk_cache_radios_[Project::kCacheStoreAlongsideProject]->isChecked()) {
    DiskManager::instance()->ShowDiskCacheSettingsDialog(working_project_->get_cache_alongside_project_path(), this);
  } else {
    DiskManager::instance()->ShowDiskCacheSettingsDialog(custom_cache_path_->text(), this);
  }
}

}
