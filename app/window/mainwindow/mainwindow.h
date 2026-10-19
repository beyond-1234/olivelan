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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <kddockwidgets/Config.h>
#include <kddockwidgets/MainWindow.h>

#include "mainwindowlayoutinfo.h"
#include "node/project.h"
#include "panel/multicam/multicampanel.h"
#include "panel/panelmanager.h"
#include "panel/audiomonitor/audiomonitor.h"
#include "panel/curve/curve.h"
#include "panel/history/historypanel.h"
#include "panel/node/node.h"
#include "panel/param/param.h"
#include "panel/project/project.h"
#include "panel/scope/scope.h"
#include "panel/table/table.h"
#include "panel/taskmanager/taskmanager.h"
#include "panel/timeline/timeline.h"
#include "panel/tool/tool.h"
#include "panel/footageviewer/footageviewer.h"
#include "panel/sequenceviewer/sequenceviewer.h"
#include "panel/pixelsampler/pixelsamplerpanel.h"

#ifdef Q_OS_WINDOWS
#include <shobjidl.h>
#endif

namespace olive {

/**
 * @brief Olive's main window responsible for docking widgets and the main menu bar.
 */
class MainWindow : public KDDockWidgets::QtWidgets::MainWindow
{
  Q_OBJECT
public:
  MainWindow(QWidget *parent = nullptr);

  virtual ~MainWindow() override;

  void LoadLayout(const MainWindowLayoutInfo &info);

  MainWindowLayoutInfo SaveLayout() const;

  TimelinePanel *OpenSequence(Sequence* sequence, bool enable_focus = true);

  void CloseSequence(Sequence* sequence);

  bool IsSequenceOpen(Sequence* sequence) const;

  void OpenFolder(Folder *i, bool floating);

  void OpenNodeInViewer(ViewerOutput* node);

  enum ProgressStatus {
    kProgressNone,
    kProgressShow,
    kProgressError
  };

  /**
   * @brief Where applicable, show progress on an operating system level
   *
   * * For Windows, this is shown as progress in the taskbar.
   * * For macOS, this is shown as progress in the dock.
   */
  void SetApplicationProgressStatus(ProgressStatus status);

  /**
   * @brief If SetApplicationProgressStatus is set to kShowProgress, set the value with this
   *
   * Expects a percentage (0-100 inclusive).
   */
  void SetApplicationProgressValue(int value);

  void SelectFootage(const QVector<Footage*> &e);

  /**
   * @brief Patch 0095: starts playback for a fixed duration, then exits the process.
   *
   * Hidden `--autoplay` benchmark aid. It waits for the startup project's sequence viewer to
   * connect, plays continuously from the current playhead, and logs a start/finish pair with
   * playhead progress plus presented/expected/skipped frame counters. An external harness can
   * therefore distinguish actual presentation from playhead advance while also measuring CPU
   * time and readback counts without injecting input.
   */
  void StartAutoplayBenchmark(int seconds);

public slots:
  void SetProject(Project *p);

  void SetFullscreen(bool fullscreen);

  void ToggleMaximizedPanel();

  void SetDefaultLayout();

protected:
  virtual void showEvent(QShowEvent* e) override;

  virtual void resizeEvent(QResizeEvent* e) override;

  virtual void closeEvent(QCloseEvent* e) override;

#ifdef Q_OS_WINDOWS
  virtual bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private:
  /**
   * @brief Applies the default dock layout once the window actually has a size.
   *
   * On Wayland the first showEvent/resize can be delivered while the window is still 0x0. Building
   * the layout at that point makes KDDockWidgets lay every panel out at zero size, leaving an empty
   * window. We therefore defer the initial layout until the window reports a non-zero size.
   */
  void ApplyInitialLayoutIfNeeded();

  void ApplyCapcutDockSizesWhenReady(int attempts);

  void TryStartAutoplayBenchmark();

  TimelinePanel* AppendTimelinePanel();

  template <typename T>
  T* AppendPanelInternal(const QString &panel_name, QList<T*>& list);

  template <typename T>
  void RemovePanelInternal(QList<T*>& list, T *panel);

  void RemoveTimelinePanel(TimelinePanel *panel);

  void TimelineFocused(ViewerOutput *viewer);

  static QString GetCustomShortcutsFile();

  void LoadCustomShortcuts();

  void SaveCustomShortcuts();

  void UpdateAudioMonitorParams(ViewerOutput* viewer);

  void UpdateNodePanelContextFromTimelinePanel(TimelinePanel *panel);

  void SelectFootageForProjectPanel(const QVector<Footage*> &e, ProjectPanel *p);

  QByteArray premaximized_state_;

  // Standard panels
  ProjectPanel *project_panel_;
  NodePanel* node_panel_;
  ParamPanel* param_panel_;
  CurvePanel* curve_panel_;
  SequenceViewerPanel* sequence_viewer_panel_;
  FootageViewerPanel* footage_viewer_panel_;
  QList<ProjectPanel*> folder_panels_;
  ToolPanel* tool_panel_;
  QList<TimelinePanel*> timeline_panels_;
  AudioMonitorPanel* audio_monitor_panel_;
  TaskManagerPanel* task_man_panel_;
  PixelSamplerPanel* pixel_sampler_panel_;
  ScopePanel* scope_panel_;
  QList<ViewerPanel*> viewer_panels_;
  MulticamPanel *multicam_panel_;
  HistoryPanel *history_panel_;

#ifdef Q_OS_WINDOWS
  unsigned int taskbar_btn_id_;

  ITaskbarList3* taskbar_interface_;
#endif

  bool first_show_;

  bool constructor_finished_{false};

  bool initial_layout_applied_;

  bool capcut_dock_sizes_applied_;

  int autoplay_seconds_;
  int autoplay_wait_attempts_;

  Project *project_;

private slots:
  void FocusedPanelChanged(PanelWidget* panel);

  void UpdateTitle();

  void TimelineCloseRequested();

  void ViewerCloseRequested();

  void ViewerWithPanelRemovedFromGraph();

  void FolderPanelCloseRequested();

  void StatusBarDoubleClicked();

  void NodePanelGroupOpenedOrClosed();

#ifdef Q_OS_LINUX
  void ShowNouveauWarning();
#endif

  void TimelinePanelSelectionChanged(const QVector<Block*> &blocks);

  void ShowWelcomeDialog();

  /**
   * @brief Patch 0078 (Phase 9): offers the display-session choice when it is actually meaningful.
   *
   * Logs the session classification on every start regardless of whether the prompt is shown.
   */
  void ShowGpuSessionDialogIfNeeded();

  void RevealViewerInProject(ViewerOutput *r);
  void RevealViewerInFootageViewer(ViewerOutput *r, const TimeRange &range);

};

}

#endif
