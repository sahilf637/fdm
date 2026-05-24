#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QTableView>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "DownloadDetailsWindow.h"
#include "NewDownloadDialog.h"
#include "fdm/store/DownloadListModel.h"
#include "fdm/store/DownloadManager.h"

namespace fdm_gui {

using fdm::store::DownloadListModel;
using fdm::store::DownloadManager;
using fdm::store::DownloadStatus;

MainWindow::MainWindow(DownloadManager* manager, QWidget* parent)
    : QMainWindow(parent), manager_(manager) {
    setWindowTitle("Fresh Download Manager");
    resize(1000, 520);

    buildUi();
    buildToolbarAndMenu();
    updateActionStates();
    statusBar()->showMessage("Ready");
}

void MainWindow::buildUi() {
    model_ = new DownloadListModel(manager_, this);

    table_ = new QTableView;
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(DownloadListModel::ColName,
                                                     QHeaderView::Interactive);
    table_->setColumnWidth(DownloadListModel::ColName, 220);
    table_->setColumnWidth(DownloadListModel::ColSize, 90);
    table_->setColumnWidth(DownloadListModel::ColProgress, 80);
    table_->setColumnWidth(DownloadListModel::ColSpeed, 90);
    table_->setColumnWidth(DownloadListModel::ColStatus, 110);

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(table_, &QTableView::doubleClicked, this, &MainWindow::onRowDoubleClicked);

    setCentralWidget(table_);
}

void MainWindow::buildToolbarAndMenu() {
    newAction_ = new QAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder),
                             "&New Download", this);
    newAction_->setShortcut(QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, &MainWindow::onNewDownloadClicked);

    pauseAction_ = new QAction(style()->standardIcon(QStyle::SP_MediaPause), "&Pause", this);
    connect(pauseAction_, &QAction::triggered, this, &MainWindow::onPauseClicked);

    resumeAction_ = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), "&Resume", this);
    connect(resumeAction_, &QAction::triggered, this, &MainWindow::onResumeClicked);

    cancelAction_ = new QAction(style()->standardIcon(QStyle::SP_MediaStop), "&Cancel", this);
    connect(cancelAction_, &QAction::triggered, this, &MainWindow::onCancelClicked);

    removeAction_ = new QAction(style()->standardIcon(QStyle::SP_TrashIcon),
                                "Re&move from list", this);
    connect(removeAction_, &QAction::triggered, this, &MainWindow::onRemoveClicked);

    openFolderAction_ = new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                                    "Open &folder", this);
    connect(openFolderAction_, &QAction::triggered, this, &MainWindow::onOpenFolderClicked);

    detailsAction_ = new QAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                                 "Show &details", this);
    detailsAction_->setShortcut(Qt::CTRL | Qt::Key_D);
    connect(detailsAction_, &QAction::triggered, this, &MainWindow::onDetailsClicked);

    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction(newAction_);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    auto* dlMenu = menuBar()->addMenu("&Download");
    dlMenu->addAction(pauseAction_);
    dlMenu->addAction(resumeAction_);
    dlMenu->addAction(cancelAction_);
    dlMenu->addSeparator();
    dlMenu->addAction(detailsAction_);
    dlMenu->addAction(openFolderAction_);
    dlMenu->addSeparator();
    dlMenu->addAction(removeAction_);

    auto* toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(newAction_);
    toolbar->addSeparator();
    toolbar->addAction(pauseAction_);
    toolbar->addAction(resumeAction_);
    toolbar->addAction(cancelAction_);
    toolbar->addSeparator();
    toolbar->addAction(detailsAction_);
    toolbar->addAction(openFolderAction_);
    toolbar->addAction(removeAction_);
}

qint64 MainWindow::currentDownloadId() const {
    const QModelIndexList selection = table_->selectionModel()->selectedRows();
    if (selection.isEmpty()) return 0;
    return model_->idForRow(selection.first().row());
}

void MainWindow::updateActionStates() {
    const qint64 id = currentDownloadId();
    if (id == 0) {
        pauseAction_->setEnabled(false);
        resumeAction_->setEnabled(false);
        cancelAction_->setEnabled(false);
        removeAction_->setEnabled(false);
        openFolderAction_->setEnabled(false);
        detailsAction_->setEnabled(false);
        return;
    }
    const auto row = manager_->row(id);
    if (!row) return;
    const bool active = row->rec.status == DownloadStatus::Active;
    const bool paused = row->rec.status == DownloadStatus::Paused;
    const bool failed = row->rec.status == DownloadStatus::Failed;
    pauseAction_->setEnabled(active);
    resumeAction_->setEnabled(paused || failed);
    cancelAction_->setEnabled(active || paused);
    removeAction_->setEnabled(true);
    openFolderAction_->setEnabled(true);
    detailsAction_->setEnabled(true);
}

void MainWindow::onSelectionChanged() {
    updateActionStates();
}

void MainWindow::onRowDoubleClicked(const QModelIndex& index) {
    const qint64 id = model_->idForRow(index.row());
    if (id != 0) openDetailsFor(id);
}

void MainWindow::onNewDownloadClicked() {
    NewDownloadDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const qint64 id = manager_->startNew(url, path);
    statusBar()->showMessage(QString("Started %1").arg(QFileInfo(path).fileName()),
                             4000);
    // Select the new row so action states update without an extra click.
    const int row = model_->rowCount() - 1;
    if (row >= 0 && model_->idForRow(row) == id) {
        table_->selectRow(row);
    }
}

void MainWindow::onPauseClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) manager_->pause(id);
}

void MainWindow::onResumeClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) manager_->resume(id);
}

void MainWindow::onCancelClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) manager_->cancel(id);
}

void MainWindow::onRemoveClicked() {
    const qint64 id = currentDownloadId();
    if (id == 0) return;
    const auto row = manager_->row(id);
    if (!row) return;

    QMessageBox box(this);
    box.setWindowTitle("Remove download");
    box.setText(QString("Remove '%1' from the list?")
                    .arg(QFileInfo(row->rec.outputPath).fileName()));
    QAbstractButton* listOnly =
        box.addButton("Remove from list", QMessageBox::AcceptRole);
    QAbstractButton* alsoFile = box.addButton("Remove from list and delete file",
                                              QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == listOnly) {
        manager_->remove(id, /*alsoRemoveFile=*/false);
    } else if (box.clickedButton() == alsoFile) {
        manager_->remove(id, /*alsoRemoveFile=*/true);
    }
}

void MainWindow::onOpenFolderClicked() {
    const qint64 id = currentDownloadId();
    if (id == 0) return;
    const auto row = manager_->row(id);
    if (!row) return;
    const QString folder = QFileInfo(row->rec.outputPath).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void MainWindow::onDetailsClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) openDetailsFor(id);
}

void MainWindow::openDetailsFor(qint64 id) {
    auto it = detailsWindows_.find(id);
    if (it != detailsWindows_.end() && !it->isNull()) {
        (*it)->raise();
        (*it)->activateWindow();
        return;
    }
    auto* w = new DownloadDetailsWindow(manager_, id);
    w->setAttribute(Qt::WA_DeleteOnClose);
    detailsWindows_.insert(id, w);
    // Self-clean the hash when the window is destroyed so a fresh open
    // creates a new window instead of focusing a dangling pointer.
    connect(w, &QObject::destroyed, this, [this, id]() {
        detailsWindows_.remove(id);
    });
    w->show();
}

}  // namespace fdm_gui
