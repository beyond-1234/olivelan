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

#ifndef COLORSERVICE_H
#define COLORSERVICE_H

#include <memory>
#include <QMutex>

#include "codec/frame.h"
#include "node/node.h"
#include "render/colorprocessor.h"

namespace olive {

class ColorManager : public QObject
{
  Q_OBJECT
public:
  ColorManager(Project *project);
  ~ColorManager();

  void Init();

  QString GetConfigFilename() const;

  static void SetUpDefaultConfig();

  void SetConfigFilename(const QString& filename);

  QStringList ListAvailableDisplays();

  QString GetDefaultDisplay();

  QStringList ListAvailableViews(QString display);

  QString GetDefaultView(const QString& display);

  QStringList ListAvailableLooks();

  QStringList ListAvailableColorspaces() const;

  QString GetDefaultInputColorSpace() const;

  void SetDefaultInputColorSpace(const QString& s);

  QString GetReferenceColorSpace() const;

  QString GetCompliantColorSpace(const QString& s);

  ColorTransform GetCompliantColorSpace(const ColorTransform& transform, bool force_display = false);

  /**
   * @brief Resolve a transform for restoring state after a config/HDR change.
   *
   * Unlike GetCompliantColorSpace(), this method never returns an unknown
   * colour-space name. Missing/removed colour spaces fall back to a valid
   * project default, and invalid display/view/look names are repaired.
   */
  ColorTransform GetRestorableColorTransform(const ColorTransform& transform);

  /**
   * @brief True when any Footage stream in this project carries PQ/HLG metadata.
   *
   * Used by the display to choose the HDR view once per project/sequence rather
   * than switching at every HDR/SDR edit point.
   */
  bool ProjectHasHdrFootage() const;

  void GetDefaultLumaCoefs(double *rgb) const;

  Project *project() const;

  void UpdateConfigFromFilename();

signals:
  void ConfigChanged(const QString &s);

  void ReferenceSpaceChanged(const QString &s);

  void DefaultInputChanged(const QString &s);

private:
  struct Impl;

  std::unique_ptr<Impl> impl_;

  // Patch 0037: the OCIO-typed accessors (GetConfig/CreateConfigFromFile/
  // GetDefaultConfig/ListAvailableColorspaces) live in common/ociobridge.h so
  // this header stays free of <OpenColorIO/OpenColorIO.h>.
  friend struct ColorManagerOCIO;

};

}

#endif // COLORSERVICE_H
