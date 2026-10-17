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

#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QScreen>
#include <QTimer>

#include <kddockwidgets/LayoutSaver.h>

#ifdef Q_OS_LINUX
#include <QOffscreenSurface>
#endif

#include "dialog/about/about.h"
#include "dialog/gpusession/gpusessiondialog.h"
#include "codec/hal/gpu_environment_probe.h"
#include "codec/hal/gpu_session_advisor.h"
#include "mainmenu.h"
#include "mainstatusbar.h"
#include "timeline/timelineundoworkarea.h"

namespace olive {

#define super KDDockWidgets::QtWidgets::MainWindow

MainWindow::MainWindow(QWidget *parent) :
  super(QStringLiteral("OliveMain"), KDDockWidgets::MainWindowOption_None, parent),
  initial_layout_applied_(false),
  project_(nullptr)
{
  // Resizes main window to desktop geometry on startup. Fixes the following issues:
  // * Qt on Windows has a bug that "de-maximizes" the window when widgets are added, resizing the
  //   window beforehand works around that issue and we just set it to whatever size is available.
  // * On Linux, it seems the window starts off at a vastly different size and then maximizes
  //   which throws off the proportions and makes the resulting layout wonky.
  if (!qApp->screens().empty()) {
    resize(qApp->screens().at(0)->availableSize());
  }

#ifdef Q_OS_WINDOWS
  // Set up taskbar button progress bar (used for some modal tasks like exporting)
  taskbar_btn_id_ = RegisterWindowMessage(TEXT("TaskbarButtonCreated"));
  taskbar_interface_ = nullptr;
#endif

  first_show_ = true;

  // Create and set main menu
  MainMenu* main_menu = new MainMenu(this);
  setMenuBar(main_menu);

  LoadCustomShortcuts();

  // Create and set status bar
  MainStatusBar* status_bar = new MainStatusBar(this);
  status_bar->ConnectTaskManager(TaskManager::instance());
  connect(status_bar, &MainStatusBar::DoubleClicked, this, &MainWindow::StatusBarDoubleClicked);
  setStatusBar(status_bar);

  // Create standard panels
  node_panel_ = new NodePanel();
  footage_viewer_panel_ = new FootageViewerPanel();
  param_panel_ = new ParamPanel();
  curve_panel_ = new CurvePanel();
  sequence_viewer_panel_ = new SequenceViewerPanel();
  multicam_panel_ = new MulticamPanel();
  pixel_sampler_panel_ = new PixelSamplerPanel();
  project_panel_ = new ProjectPanel(QStringLiteral("ProjectPanel"));
  tool_panel_ = new ToolPanel();
  task_man_panel_ = new TaskManagerPanel();
  AppendTimelinePanel();
  audio_monitor_panel_ = new AudioMonitorPanel();
  scope_panel_ = new ScopePanel();
  history_panel_ = new HistoryPanel();

  // HACK: The pixel sampler is closed by default, which signals to Core that
  //       it's no longer visible. However KDDockWidgets doesn't appear to
  //       emit the "shown" signal before emitting the "hidden" signals, resulting
  //       in Core thinking there are -1 pixel samplers open. To mitigate that,
  //       we force "shown" to emit ourselves here.
  emit pixel_sampler_panel_->shown();

  // Make node-related connections
  connect(node_panel_, &NodePanel::NodeSelectionChangedWithContexts, param_panel_, &ParamPanel::SetSelectedNodes);
  connect(node_panel_, &NodePanel::NodeGroupOpened, this, &MainWindow::NodePanelGroupOpenedOrClosed);
  connect(node_panel_, &NodePanel::NodeGroupClosed, this, &MainWindow::NodePanelGroupOpenedOrClosed);
  connect(param_panel_, &ParamPanel::FocusedNodeChanged, sequence_viewer_panel_, &ViewerPanel::SetGizmos);
  connect(param_panel_, &ParamPanel::RequestViewerToStartEditingText, sequence_viewer_panel_, &ViewerPanel::RequestStartEditingText);
  connect(param_panel_, &ParamPanel::FocusedNodeChanged, curve_panel_, &CurvePanel::SetNode);
  connect(param_panel_, &ParamPanel::SelectedNodesChanged, node_panel_, &NodePanel::Select);
  connect(project_panel_, &ProjectPanel::ProjectNameChanged, this, &MainWindow::UpdateTitle);

  connect(node_panel_, &NodePanel::NodeSelectionChanged, sequence_viewer_panel_, &ViewerPanel::SetNodeViewSelections);

  // Route play/pause/shuttle commands from these panels to the sequence viewer
  sequence_viewer_panel_->ConnectTimeBasedPanel(param_panel_);
  sequence_viewer_panel_->ConnectTimeBasedPanel(curve_panel_);
  sequence_viewer_panel_->ConnectTimeBasedPanel(multicam_panel_);

  connect(PanelManager::instance(), &PanelManager::FocusedPanelChanged, this, &MainWindow::FocusedPanelChanged);

  sequence_viewer_panel_->AddPlaybackDevice(multicam_panel_->GetMulticamWidget()->GetDisplayWidget());
  sequence_viewer_panel_->ConnectMulticamWidget(multicam_panel_->GetMulticamWidget());

  scope_panel_->SetViewerPanel(sequence_viewer_panel_);

  UpdateTitle();

  // NOTE: the default dock layout is deliberately *not* built here. On Wayland the window has no
  // size yet at this point (and even the first showEvent/resize can still be 0x0), which would make
  // KDDockWidgets lay every panel out at zero size and leave an empty window. ApplyInitialLayoutIfNeeded()
  // builds it as soon as the window reports a real size - see showEvent()/resizeEvent().
}

MainWindow::~MainWindow()
{
#ifdef Q_OS_WINDOWS
  if (taskbar_interface_) {
    taskbar_interface_->Release();
  }
#endif
}

void MainWindow::LoadLayout(const MainWindowLayoutInfo &info)
{
  foreach (Folder* folder, info.open_folders()) {
    OpenFolder(folder, true);
  }

  foreach (Sequence *sequence, info.open_sequences()) {
    OpenSequence(sequence, info.open_sequences().size() == 1);
  }

  foreach (ViewerOutput *viewer, info.open_viewers()) {
    OpenNodeInViewer(viewer);
  }

  for (auto it=info.panel_data().cbegin(); it!=info.panel_data().cend(); it++) {
    // Find panel with this ID
    if (PanelWidget *panel = PanelManager::instance()->GetPanelWithName(it->first)) {
      panel->LoadData(it->second);
    }
  }

  // KDDockWidgets 2.x serializes layouts as JSON, which is incompatible with the opaque binary
  // format used by 1.x. Older Olive projects therefore carry a state blob that can't be restored.
  // LayoutSaver::restoreLayout() parses with allow_exceptions=false and rejects such data safely
  // (returns false, leaving the current layout untouched), so we simply fall back to the default
  // layout rather than leaving panels in an undefined state.
  if (!KDDockWidgets::LayoutSaver().restoreLayout(qUncompress(info.state()))) {
    qWarning() << "Failed to restore saved dock layout (incompatible with KDDockWidgets 2.x or corrupt);"
               << "rebuilding default layout.";

    // Rebuild unconditionally. restoreLayout() validates the blob before mutating, but its RAII
    // cleanup still prunes empty groups from the *current* layout, so the initial default layout
    // can be left partially dismantled. ApplyInitialLayoutIfNeeded() would be a no-op here (it
    // already ran), so call SetDefaultLayout() directly.
    initial_layout_applied_ = true;
    SetDefaultLayout();
  }
}

QString TransformNameForSerialization(const QString &unique, int i)
{
  return QStringLiteral("%1:%2").arg(unique.split(':').at(0), QString::number(i));
}

void CorrectPanelDataIfNecessary(const QString &unique_name, int index, MainWindowLayoutInfo &info, QByteArray &layout)
{
  QString corrected = TransformNameForSerialization(unique_name, index);
  if (corrected != unique_name) {
    info.move_panel_data(unique_name, corrected);
    layout.replace(unique_name.toUtf8(), corrected.toUtf8());
  }
}

MainWindowLayoutInfo MainWindow::SaveLayout() const
{
  MainWindowLayoutInfo info;

  QByteArray layout = premaximized_state_.isEmpty() ? KDDockWidgets::LayoutSaver().serializeLayout() : premaximized_state_;

  foreach (PanelWidget *panel, PanelManager::instance()->panels()) {
    info.set_panel_data(panel->uniqueName(), panel->SaveData());
  }

  for (int i = 0; i < folder_panels_.size(); i++) {
    auto panel = folder_panels_.at(i);
    info.add_folder(panel->get_root());
    CorrectPanelDataIfNecessary(panel->uniqueName(), i, info, layout);
  }

  for (int i = 0; i < timeline_panels_.size(); i++) {
    auto panel = timeline_panels_.at(i);
    info.add_sequence(panel->GetSequence());
    CorrectPanelDataIfNecessary(panel->uniqueName(), i, info, layout);
  }

  for (int i = 0; i < viewer_panels_.size(); i++) {
    auto panel = viewer_panels_.at(i);
    info.add_viewer(panel->GetConnectedViewer());
    CorrectPanelDataIfNecessary(panel->uniqueName(), i, info, layout);
  }

  info.set_state(qCompress(layout));

  return info;
}

TimelinePanel* MainWindow::OpenSequence(Sequence *sequence, bool enable_focus)
{
  // See if this sequence is already open, and switch to it if so
  foreach (TimelinePanel* tl, timeline_panels_) {
    if (tl->GetConnectedViewer() == sequence) {
      tl->raise();
      return tl;
    }
  }

  // See if we have any sequences open or not
  TimelinePanel* panel;

  if (!timeline_panels_.first()->GetConnectedViewer()) {
    panel = timeline_panels_.first();
  } else {
    panel = AppendTimelinePanel();
    //enable_focus = false;
  }

  panel->ConnectViewerNode(sequence);

  if (enable_focus) {
    TimelineFocused(sequence);
    UpdateAudioMonitorParams(sequence);
  }

  return panel;
}

void MainWindow::CloseSequence(Sequence *sequence)
{
  // We defer to RemoveTimelinePanel() to close the panels, which may delete and remove indices from timeline_panels_.
  // We make a copy so that our array here doesn't get ruined by what RemoveTimelinePanel() does
  QList<TimelinePanel*> copy = timeline_panels_;

  foreach (TimelinePanel* tp, copy) {
    if (tp->GetConnectedViewer() == sequence) {
      RemoveTimelinePanel(tp);
    }
  }
}

bool MainWindow::IsSequenceOpen(Sequence *sequence) const
{
  foreach (TimelinePanel* tp, timeline_panels_) {
    if (tp->GetConnectedViewer() == sequence) {
      return true;
    }
  }

  return false;
}

void MainWindow::OpenFolder(Folder *i, bool floating)
{
  ProjectPanel* panel = AppendPanelInternal(QStringLiteral("FolderPanel"), folder_panels_);

  panel->set_project(i->project());
  panel->set_root(i);

  if (floating) {
    // Floating panels are intentionally top-level windows.
    panel->show();
    panel->raise();
    panel->setFloating(floating);
  } else {
    // Dock first, then show, so KDDockWidgets doesn't have to spin up a transient FloatingWindow.
    project_panel_->addDockWidgetAsTab(panel);
    panel->show();
    panel->raise();
  }

  // If the panel is closed, just destroy it
  connect(panel, &ProjectPanel::CloseRequested, this, &MainWindow::FolderPanelCloseRequested);
}

void MainWindow::OpenNodeInViewer(ViewerOutput *node)
{
  ViewerPanel *existing = nullptr;

  for (auto it = viewer_panels_.cbegin(); it != viewer_panels_.cend(); it++) {
    ViewerPanel *it2 = (*it);
    if (it2->GetConnectedViewer() == node) {
      existing = it2;
      break;
    }
  }

  if (existing) {
    // This node already has a viewer, raise it
    existing->raise();
  } else {
    // Create a viewer for this node
    ViewerPanel* viewer = AppendPanelInternal(QStringLiteral("ViewerPanel"), viewer_panels_);

    // Viewers open as floating windows (they aren't docked anywhere), so show them explicitly.
    // "For some reason raise() on its own doesn't do anything, we need both."
    viewer->show();
    viewer->raise();

    viewer->ConnectViewerNode(node);

    connect(viewer, &ViewerPanel::CloseRequested, this, &MainWindow::ViewerCloseRequested);
    connect(node, &ViewerOutput::RemovedFromGraph, this, &MainWindow::ViewerWithPanelRemovedFromGraph);
  }
}

void MainWindow::SetFullscreen(bool fullscreen)
{
  if (fullscreen) {
    setWindowState(windowState() | Qt::WindowFullScreen);
  } else {
    setWindowState(windowState() & ~Qt::WindowFullScreen);
  }
}

void MainWindow::ToggleMaximizedPanel()
{
  KDDockWidgets::LayoutSaver saver;

  if (premaximized_state_.isEmpty()) {
    // Assume nothing is maximized at the moment

    // Find the currently focused panel
    PanelWidget* currently_hovered = PanelManager::instance()->CurrentlyHovered();

    // If no panel is hovered, fallback to the currently active panel
    if (!currently_hovered) {
      currently_hovered = PanelManager::instance()->CurrentlyFocused();

      // If no panel is hovered or focused, do nothing
      if (!currently_hovered) {
        return;
      }
    }

    // If this panel is not actually on the main window, this is a no-op
    if (currently_hovered->isFloating()) {
      return;
    }

    // Save the current state so it can be restored later
    premaximized_state_ = saver.serializeLayout();

    // For every other panel that is on the main window, hide it
    foreach (PanelWidget* panel, PanelManager::instance()->panels()) {
      if (!panel->isFloating() && panel != currently_hovered) {
        panel->close();
      }
    }
  } else {
    // Preserve currently focused panel
    auto currently_focused_panel = PanelManager::instance()->CurrentlyFocused(false);

    // Assume we are currently maximized, restore the state
    PanelManager::instance()->SetSuppressChangedSignal(true);
    saver.restoreLayout(premaximized_state_);
    premaximized_state_.clear();

    currently_focused_panel->raise();
    currently_focused_panel->setFocus();

    PanelManager::instance()->SetSuppressChangedSignal(false);
  }
}

void MainWindow::SetProject(Project *p)
{
  if (project_ == p) {
    return;
  }

  if (project_) {
    // Clear all data
    param_panel_->SetContexts(QVector<Node*>());
    node_panel_->SetContexts(QVector<Node*>());

    // Close any nodes open in TimeBasedWidgets
    foreach (PanelWidget* panel, PanelManager::instance()->panels()) {
      TimeBasedPanel* tbp = dynamic_cast<TimeBasedPanel*>(panel);

      if (tbp && tbp->GetConnectedViewer() && tbp->GetConnectedViewer()->project() == project_) {
        if (dynamic_cast<TimelinePanel*>(tbp)) {
          // Prefer our CloseSequence function which will delete any unnecessary timeline panels
          CloseSequence(static_cast<Sequence*>(tbp->GetConnectedViewer()));
        } else {
          tbp->DisconnectViewerNode();
        }
      }
    }

    // Close any extra folder panels
    foreach (ProjectPanel* panel, folder_panels_) {
      panel->close();
    }

    // Close any extra viewer panels
    foreach (ViewerPanel *viewer, viewer_panels_) {
      viewer->close();
    }
  }

  project_ = p;
  project_panel_->set_project(p);

  if (project_) {
    project_panel_->setFocus();
  }
}

void MainWindow::SetApplicationProgressStatus(ProgressStatus status)
{
#if defined(Q_OS_WINDOWS)
  if (taskbar_interface_) {
    switch (status) {
    case kProgressShow:
      taskbar_interface_->SetProgressState(reinterpret_cast<HWND>(this->winId()), TBPF_NORMAL);
      break;
    case kProgressNone:
      taskbar_interface_->SetProgressState(reinterpret_cast<HWND>(this->winId()), TBPF_NOPROGRESS);
      break;
    case kProgressError:
      taskbar_interface_->SetProgressState(reinterpret_cast<HWND>(this->winId()), TBPF_ERROR);
      break;
    }
  }

#elif defined(Q_OS_MAC)
#endif
}

void MainWindow::SetApplicationProgressValue(int value)
{
#if defined(Q_OS_WINDOWS)
  if (taskbar_interface_) {
    taskbar_interface_->SetProgressValue(reinterpret_cast<HWND>(this->winId()), value, 100);
  }
#elif defined(Q_OS_MAC)
#endif
}

void MainWindow::SelectFootage(const QVector<Footage *> &e)
{
  SelectFootageForProjectPanel(e, project_panel_);
  for (ProjectPanel *p : folder_panels_) {
    SelectFootageForProjectPanel(e, p);
  }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
  // Try to close all projects (this will return false if the user chooses not to close)
  if (!Core::instance()->CloseProject(false)) {
    e->ignore();
    return;
  }

  scope_panel_->SetViewerPanel(nullptr);

  PanelManager::instance()->DeleteAllPanels();

  SaveCustomShortcuts();

  QMainWindow::closeEvent(e);
}

#ifdef Q_OS_WINDOWS
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
{
  if (static_cast<MSG*>(message)->message == taskbar_btn_id_) {
    // Attempt to create taskbar button progress handle
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITaskbarList3,
                                  reinterpret_cast<void**>(&taskbar_interface_));

    if (SUCCEEDED(hr)) {
      hr = taskbar_interface_->HrInit();

      if (FAILED(hr)) {
        taskbar_interface_->Release();
        taskbar_interface_ = nullptr;
      }
    }
  }

  return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

void MainWindow::StatusBarDoubleClicked()
{
  task_man_panel_->show();
  task_man_panel_->raise();
}

void MainWindow::NodePanelGroupOpenedOrClosed()
{
  NodePanel *p = static_cast<NodePanel*>(sender());
  param_panel_->SetContexts(p->GetContexts());
}

void MainWindow::TimelinePanelSelectionChanged(const QVector<Block *> &blocks)
{
  TimelinePanel *panel = static_cast<TimelinePanel *>(sender());

  if (PanelManager::instance()->CurrentlyFocused(false) == panel) {
    UpdateNodePanelContextFromTimelinePanel(panel);
    sequence_viewer_panel_->SetTimelineSelectedBlocks(blocks);
  }
}

void MainWindow::ShowWelcomeDialog()
{
  if (OLIVE_CONFIG("ShowWelcomeDialog").toBool()) {
    // Shown non-modally on purpose. An application-modal dialog launched around startup competes
    // for activation with the panels that are being opened while the project/layout settles; on
    // Wayland (KWin) that makes the main window and the dialog bounce activation back and forth,
    // with the main window looking frozen/blank. A non-modal welcome dialog can't do that.
    AboutDialog *ad = new AboutDialog(true, this);
    ad->setAttribute(Qt::WA_DeleteOnClose);
    ad->show();
    ad->raise();
  }
}

void MainWindow::ShowGpuSessionDialogIfNeeded()
{
  // Always record the classification, prompt or not - it is the only trace of why a given tier was
  // chosen on someone else's machine.
  qInfo() << "[GPU Session]" << GpuSessionAdvisor::DescribeEnvironment();

  if (OLIVE_CONFIG("GpuSessionPromptDisabled").toBool()) {
    return;
  }

  // An explicit OLIVE_GPU_SESSION means the session is already decided (scripted runs, the test
  // harness); don't interrupt.
  if (GpuSessionAdvisor::IsSessionForcedByEnvironment()) {
    return;
  }

  // Only a hybrid Wayland session has a real choice; everywhere else both modes put GL on the same
  // GPU and the prompt would be pure noise.
  if (!GpuSessionAdvisor::HasChoice()) {
    return;
  }

  // Non-modal for the same reason as the welcome dialog: an application-modal dialog shown around
  // startup fights the panels being opened nearby for activation, which on Wayland (KWin) makes the
  // main window look frozen.
  GpuSessionDialog *dialog = new GpuSessionDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->show();
  dialog->raise();
}

void MainWindow::RevealViewerInProject(ViewerOutput *r)
{
  // Rather than just using the resident ProjectPanel, find the most recently focused one since
  // that's probably the one people will want
  auto panels = PanelManager::instance()->GetPanelsOfType<ProjectPanel>();
  foreach (ProjectPanel *p, panels) {
    if (p->SelectItem(r)) {
      break;
    }
  }
}

void MainWindow::RevealViewerInFootageViewer(ViewerOutput *r, const TimeRange &range)
{
  footage_viewer_panel_->ConnectViewerNode(r);

  auto command = new MultiUndoCommand();
  if (!r->GetWorkArea()->enabled()) {
    command->add_child(new WorkareaSetEnabledCommand(r->project(), r->GetWorkArea(), true));
  }
  command->add_child(new WorkareaSetRangeCommand(r->GetWorkArea(), range));
  Core::instance()->undo_stack()->push(command, tr("Set Footage Workarea"));

  r->SetPlayhead(range.in());
}

#ifdef Q_OS_LINUX
void MainWindow::ShowNouveauWarning()
{
  QMessageBox::warning(this,
                       tr("Driver Warning"),
                       tr("Olive has detected your system is using the Nouveau graphics driver.\n\nThis driver is "
                          "known to have stability and performance issues with Olive. It is highly recommended "
                          "you install the proprietary NVIDIA driver before continuing to use Olive."),
                       QMessageBox::Ok);
}
#endif

void MainWindow::UpdateTitle()
{
  if (Core::instance()->GetActiveProject()) {
    setWindowTitle(QStringLiteral("%1 %2 - [*]%3").arg(QApplication::applicationName(),
                                                       QApplication::applicationVersion(),
                                                       Core::instance()->GetActiveProject()->pretty_filename()));
  } else {
    setWindowTitle(QStringLiteral("%1 %2").arg(QApplication::applicationName(),
                                               QApplication::applicationVersion()));
  }
}

void MainWindow::TimelineCloseRequested()
{
  TimelinePanel *t = static_cast<TimelinePanel*>(sender());
  RemoveTimelinePanel(t);
}

void MainWindow::ViewerCloseRequested()
{
  ViewerPanel* panel = static_cast<ViewerPanel*>(sender());

  if (panel == scope_panel_->GetConnectedViewerPanel()) {
    scope_panel_->SetViewerPanel(sequence_viewer_panel_);
  }

  RemovePanelInternal(viewer_panels_, panel);

  panel->deleteLater();
}

void MainWindow::ViewerWithPanelRemovedFromGraph()
{
  ViewerOutput* vo = static_cast<ViewerOutput*>(sender());
  ViewerPanel *panel = nullptr;

  foreach (ViewerPanel *p, viewer_panels_) {
    if (p->GetConnectedViewer() == vo) {
      panel = p;
      break;
    }
  }

  if (panel) {
    RemovePanelInternal(viewer_panels_, panel);
    panel->deleteLater();
    disconnect(vo, &ViewerOutput::RemovedFromGraph, this, &MainWindow::ViewerWithPanelRemovedFromGraph);
  }
}

void MainWindow::FolderPanelCloseRequested()
{
  ProjectPanel* panel = static_cast<ProjectPanel*>(sender());
  RemovePanelInternal(folder_panels_, panel);
  panel->deleteLater();
}

TimelinePanel* MainWindow::AppendTimelinePanel()
{
  TimelinePanel *previous = nullptr;
  if (!timeline_panels_.empty()) {
    previous = timeline_panels_.last();
  }

  TimelinePanel* panel = AppendPanelInternal(QStringLiteral("TimelinePanel"), timeline_panels_);

  if (previous) {
    // Dock first, then show, so KDDockWidgets doesn't have to spin up a transient FloatingWindow.
    previous->addDockWidgetAsTab(panel);
    panel->show();
    panel->raise();
  } else {
    panel->show();
    panel->raise();
    panel->SetSignalInsteadOfClose(false);
  }

  connect(panel, &PanelWidget::CloseRequested, this, &MainWindow::TimelineCloseRequested);
  connect(panel, &TimelinePanel::RequestCaptureStart, sequence_viewer_panel_, &SequenceViewerPanel::StartCapture);
  connect(panel, &TimelinePanel::BlockSelectionChanged, this, &MainWindow::TimelinePanelSelectionChanged);
  connect(panel, &TimelinePanel::RevealViewerInProject, this, &MainWindow::RevealViewerInProject);
  connect(panel, &TimelinePanel::RevealViewerInFootageViewer, this, &MainWindow::RevealViewerInFootageViewer);

  sequence_viewer_panel_->ConnectTimeBasedPanel(panel);

  return panel;
}

void MainWindow::RemoveTimelinePanel(TimelinePanel *panel)
{
  // Stop showing this timeline in the viewer
  TimelineFocused(nullptr);
  panel->ConnectViewerNode(nullptr);

  if (timeline_panels_.size() != 1) {
    RemovePanelInternal(timeline_panels_, panel);
    panel->deleteLater();
  }
}

void MainWindow::TimelineFocused(ViewerOutput* viewer)
{
  sequence_viewer_panel_->ConnectViewerNode(viewer);
  multicam_panel_->ConnectViewerNode(viewer);
  param_panel_->ConnectViewerNode(viewer);
  curve_panel_->ConnectViewerNode(viewer);
}

QString MainWindow::GetCustomShortcutsFile()
{
  return QDir(FileFunctions::GetConfigurationLocation()).filePath(QStringLiteral("shortcuts"));
}

void LoadCustomShortcutsInternal(QMenu* menu, const QMap<QString, QString>& shortcuts)
{
  QList<QAction*> actions = menu->actions();

  foreach (QAction* a, actions) {
    if (a->menu()) {
      LoadCustomShortcutsInternal(a->menu(), shortcuts);
    } else if (!a->isSeparator()) {
      QString action_id = a->property("id").toString();

      if (shortcuts.contains(action_id)) {
        a->setShortcut(shortcuts.value(action_id));
      }
    }
  }
}

void MainWindow::LoadCustomShortcuts()
{
  QFile shortcut_file(GetCustomShortcutsFile());
  if (shortcut_file.exists() && shortcut_file.open(QFile::ReadOnly)) {
    QMap<QString, QString> shortcuts;

    QString shortcut_str = QString::fromUtf8(shortcut_file.readAll());

    QStringList shortcut_list = shortcut_str.split(QStringLiteral("\n"));

    foreach (const QString& s, shortcut_list) {
      QStringList shortcut_line = s.split(QStringLiteral("\t"));
      if (shortcut_line.size() >= 2) {
        shortcuts.insert(shortcut_line.at(0), shortcut_line.at(1));
      }
    }

    shortcut_file.close();

    if (!shortcuts.isEmpty()) {
      QList<QAction*> menus = menuBar()->actions();

      foreach (QAction* menu, menus) {
        LoadCustomShortcutsInternal(menu->menu(), shortcuts);
      }
    }
  }
}

void SaveCustomShortcutsInternal(QMenu* menu, QMap<QString, QString>* shortcuts)
{
  QList<QAction*> actions = menu->actions();

  foreach (QAction* a, actions) {
    if (a->menu()) {
      SaveCustomShortcutsInternal(a->menu(), shortcuts);
    } else if (!a->isSeparator()) {
      QString default_shortcut = a->property("keydefault").value<QKeySequence>().toString();
      QString current_shortcut = a->shortcut().toString();
      if (current_shortcut != default_shortcut) {
        QString action_id = a->property("id").toString();
        shortcuts->insert(action_id, current_shortcut);
      }
    }
  }
}

void MainWindow::SaveCustomShortcuts()
{
  QMap<QString, QString> shortcuts;
  QList<QAction*> menus = menuBar()->actions();

  foreach (QAction* menu, menus) {
    SaveCustomShortcutsInternal(menu->menu(), &shortcuts);
  }

  QFile shortcut_file(GetCustomShortcutsFile());
  if (shortcuts.isEmpty()) {
    if (shortcut_file.exists()) {
      // No custom shortcuts, remove any existing file
      shortcut_file.remove();
    }
  } else if (shortcut_file.open(QFile::WriteOnly)) {
    for (auto it=shortcuts.cbegin(); it!=shortcuts.cend(); it++) {
      if (it != shortcuts.cbegin()) {
        shortcut_file.write(QStringLiteral("\n").toUtf8());
      }

      shortcut_file.write(it.key().toUtf8());
      shortcut_file.write(QStringLiteral("\t").toUtf8());
      shortcut_file.write(it.value().toUtf8());
    }
    shortcut_file.close();
  } else {
    qCritical() << "Failed to save custom keyboard shortcuts";
  }
}

void MainWindow::UpdateAudioMonitorParams(ViewerOutput *viewer)
{
  if (!audio_monitor_panel_->IsPlaying()) {
    audio_monitor_panel_->SetParams(viewer ? viewer->GetAudioParams() : AudioParams());
  }
}

void MainWindow::UpdateNodePanelContextFromTimelinePanel(TimelinePanel *panel)
{
  // Add selected blocks (if any)
  const QVector<Block*> &blocks = panel->GetSelectedBlocks();
  QVector<Node *> context(blocks.size());
  for (int i=0; i<blocks.size(); i++) {
    context[i] = blocks.at(i);
  }

  // If no selected blocks, set the context to the sequence
  ViewerOutput *viewer = panel->GetConnectedViewer();
  if (viewer && context.isEmpty()) {
    context.append(viewer);
  }

  node_panel_->SetContexts(context);
  param_panel_->SetContexts(context);
}

void MainWindow::SelectFootageForProjectPanel(const QVector<Footage *> &e, ProjectPanel *p)
{
  p->DeselectAll();
  for (Footage *f : e) {
    if (p->get_root()->HasChildRecursive(f)) {
      p->SelectItem(f, false);
    }
  }
}

void MainWindow::FocusedPanelChanged(PanelWidget *panel)
{
  // Update audio monitor panel
  if (TimeBasedPanel* tbp = dynamic_cast<TimeBasedPanel*>(panel)) {
    UpdateAudioMonitorParams(tbp->GetConnectedViewer());
  }

  if (NodePanel *node_panel = dynamic_cast<NodePanel*>(panel)) {
    // Set param view contexts to these
    const QVector<Node*> &new_ctxs = node_panel->GetContexts();

    if (new_ctxs != param_panel_->GetContexts()) {
      param_panel_->SetContexts(new_ctxs);
    }
  } else if (TimelinePanel* timeline = dynamic_cast<TimelinePanel*>(panel)) {
    // Signal timeline focus
    TimelineFocused(timeline->GetConnectedViewer());

    UpdateNodePanelContextFromTimelinePanel(timeline);
  } else if (ProjectPanel* project = dynamic_cast<ProjectPanel*>(panel)) {
    // Signal project panel focus
    Q_UNUSED(project)
    UpdateTitle();
  } else if (ViewerPanelBase *viewer = dynamic_cast<ViewerPanelBase*>(panel)) {
    // Update scopes for viewer
    scope_panel_->SetViewerPanel(viewer);
  }
}

void MainWindow::SetDefaultLayout()
{
  KDDockWidgets::InitialOption o;
  o.preferredSize = QSize(0, centralAreaGeometry().height());

  // Top left - Tabify footage viewer, param panel, and node panel
  addDockWidget(footage_viewer_panel_, KDDockWidgets::Location_OnTop, nullptr, o);
  footage_viewer_panel_->addDockWidgetAsTab(param_panel_);
  footage_viewer_panel_->addDockWidgetAsTab(node_panel_);
  param_panel_->raise();

  // Top right - sequence viewer
  addDockWidget(sequence_viewer_panel_, KDDockWidgets::Location_OnRight, footage_viewer_panel_, o);

  // Bottom center - timelines
  addDockWidget(timeline_panels_.first(), KDDockWidgets::Location_OnBottom);

  // Left of timeline - tool panel
  o.preferredSize = QSize(1, 0);
  addDockWidget(tool_panel_, KDDockWidgets::Location_OnLeft, timeline_panels_.first(), o);

  // Right of timeline - audio monitor
  o.preferredSize = QSize(320, 0);
  addDockWidget(audio_monitor_panel_, KDDockWidgets::Location_OnRight, timeline_panels_.first(), o);

  // Bottom left - project panel
  addDockWidget(project_panel_, KDDockWidgets::Location_OnLeft, tool_panel_);
  project_panel_->addDockWidgetAsTab(history_panel_);
  project_panel_->raise();

  // Hidden panels
  pixel_sampler_panel_->close();
  task_man_panel_->close();
  curve_panel_->close();
  scope_panel_->close();
  multicam_panel_->close();
  for (auto it = folder_panels_.cbegin(); it != folder_panels_.cend(); it++) {
    (*it)->close();
  }
  for (auto it = viewer_panels_.cbegin(); it != viewer_panels_.cend(); it++) {
    (*it)->close();
  }
  for (auto it = timeline_panels_.cbegin(); it != timeline_panels_.cend(); it++) {
    auto p = *it;
    if (p != timeline_panels_.first()) {
      p->addDockWidgetAsTab(p);
    }
  }

  // Set to unmaximized panels
  premaximized_state_.clear();
}

void MainWindow::ApplyInitialLayoutIfNeeded()
{
  if (initial_layout_applied_) {
    return;
  }

  // On Wayland the window is still 0x0 when the first showEvent/resize arrives. Building the dock
  // layout at that size leaves every panel at zero size (an empty window), so wait for a real size.
  if (width() <= 0 || height() <= 0) {
    return;
  }

  initial_layout_applied_ = true;
  SetDefaultLayout();

  // Only now that the dock layout exists is it safe to show the welcome dialog; showing it earlier
  // (while panels are still being opened) makes it compete for activation with the startup churn.
  QMetaObject::invokeMethod(this, "ShowWelcomeDialog", Qt::QueuedConnection);

  // Patch 0078 (Phase 9): same reasoning, and queued after the welcome dialog so the two never
  // overlap on screen.
  QMetaObject::invokeMethod(this, "ShowGpuSessionDialogIfNeeded", Qt::QueuedConnection);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
  QMainWindow::resizeEvent(e);

  ApplyInitialLayoutIfNeeded();
}

void MainWindow::showEvent(QShowEvent *e)
{
  QMainWindow::showEvent(e);

  // The window may already have a real size here (X11), or still be 0x0 (Wayland), in which case
  // resizeEvent() will build the layout once the compositor has configured the window.
  ApplyInitialLayoutIfNeeded();
  if (!initial_layout_applied_) {
    // Retry once the event loop has processed the platform's configure/resize.
    QTimer::singleShot(0, this, &MainWindow::ApplyInitialLayoutIfNeeded);
  }

  if (first_show_) {
    QMetaObject::invokeMethod(Core::instance(), "CheckForAutoRecoveries", Qt::QueuedConnection);

#ifdef Q_OS_LINUX
    // Check for nouveau since that driver really doesn't work with Olive
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext context;
    context.create();
    context.makeCurrent(&surface);
    const char* vendor = reinterpret_cast<const char*>(context.functions()->glGetString(GL_VENDOR));
    qDebug() << "Using graphics driver:" << vendor;

    // Patch 0078 (Phase 9): hand this to the HAL probe. Pipeline routing happens during footage
    // import, which is after showEvent, and without a vendor the probe cannot tell that Tier 1's
    // CUDA-GL interop is impossible on a hybrid Wayland session (the log used to report
    // "gl=unknown" and then select a Tier 1 whose frame pool could never register a texture).
    // This is a platform-level context rather than the render context, so a later report from the
    // render path supersedes it.
    GpuEnvironmentProbe::Instance().RecordOpenGlVendor(vendor);

    if (!strcmp(vendor, "nouveau")) {
      QMetaObject::invokeMethod(this, "ShowNouveauWarning", Qt::QueuedConnection);
    }
#endif

    first_show_ = false;
  }
}

template<typename T>
T *MainWindow::AppendPanelInternal(const QString &panel_name, QList<T*>& list)
{
  T* panel = new T(TransformNameForSerialization(panel_name, list.size()));

  // NOTE: deliberately not shown here. Calling show() on a dock widget that isn't docked yet makes
  // KDDockWidgets morph it into a FloatingWindow (a real top-level window), which flashes a taskbar
  // entry before docking removes it again. Callers dock the panel first and then show it.

  list.append(panel);

  // Let us handle the panel closing rather than the panel itself
  panel->SetSignalInsteadOfClose(true);

  return panel;
}

template<typename T>
void MainWindow::RemovePanelInternal(QList<T *> &list, T *panel)
{
  list.removeOne(panel);
}

}
