#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
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

    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested, this,
            &MainWindow::onTableContextMenu);

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

    retryAction_ = new QAction(style()->standardIcon(QStyle::SP_BrowserReload),
                               "Re&try", this);
    connect(retryAction_, &QAction::triggered, this, &MainWindow::onRetryClicked);

    redownloadAction_ = new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton),
                                    "Re&download…", this);
    connect(redownloadAction_, &QAction::triggered, this, &MainWindow::onRedownloadClicked);

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
    dlMenu->addAction(retryAction_);
    dlMenu->addAction(redownloadAction_);
    dlMenu->addSeparator();
    dlMenu->addAction(detailsAction_);
    dlMenu->addAction(openFolderAction_);
    dlMenu->addSeparator();
    dlMenu->addAction(removeAction_);

    auto* toolsMenu = menuBar()->addMenu("&Tools");
    auto* updateBackend = toolsMenu->addAction("&Update video downloader (yt-dlp)");
    connect(updateBackend, &QAction::triggered, this, &MainWindow::onUpdateYtDlp);

    auto* toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(newAction_);
    toolbar->addSeparator();
    toolbar->addAction(pauseAction_);
    toolbar->addAction(resumeAction_);
    toolbar->addAction(cancelAction_);
    toolbar->addAction(retryAction_);
    toolbar->addAction(redownloadAction_);
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
        retryAction_->setEnabled(false);
        redownloadAction_->setEnabled(false);
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
    const bool completed = row->rec.status == DownloadStatus::Completed;
    pauseAction_->setEnabled(active);
    resumeAction_->setEnabled(paused);
    cancelAction_->setEnabled(active || paused);
    retryAction_->setEnabled(failed);
    redownloadAction_->setEnabled(failed || completed);
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
    NewDownloadDialog dlg(manager_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const qint64 id =
        manager_->startNew(url, path, dlg.userHash(), dlg.userHashAlgorithm());
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

void MainWindow::onRetryClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) manager_->retry(id);
}

void MainWindow::onRedownloadClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) redownloadFromRow(id);
}

void MainWindow::redownloadFromRow(qint64 id) {
    const auto row = manager_->row(id);
    if (!row) return;
    const QFileInfo fi(row->rec.outputPath);

    NewDownloadDialog dlg(manager_, this);
    // Carry the previously known hash forward so a re-download starts with
    // the same verification expectations.
    dlg.prefill(row->rec.url, fi.absolutePath(), fi.fileName(), row->rec.expectedHash);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const qint64 newId =
        manager_->startNew(url, path, dlg.userHash(), dlg.userHashAlgorithm());
    statusBar()->showMessage(QString("Started %1").arg(QFileInfo(path).fileName()), 4000);
    const int newRow = model_->rowCount() - 1;
    if (newRow >= 0 && model_->idForRow(newRow) == newId) {
        table_->selectRow(newRow);
    }
}

void MainWindow::openExternalDownload(const ExternalDownloadRequest& req) {
    // Bring the app to the foreground regardless of whether we end up starting
    // a download -- the user just clicked something in their browser.
    showNormal();
    raise();
    activateWindow();

    if (req.url.isEmpty()) return;
    const QUrl u(req.url);
    if (u.scheme() != "http" && u.scheme() != "https") return;

    NewDownloadDialog dlg(manager_, this);
    dlg.prefill(req.url, req.dir, req.filename, req.hash);
    // The probe the dialog runs on accept needs the same auth context, or an
    // authenticated URL would 401 at filename-detection time.
    dlg.setRequestHeaders(req.headers);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const qint64 id = manager_->startNew(url, path, dlg.userHash(),
                                         dlg.userHashAlgorithm(), req.headers);
    statusBar()->showMessage(QString("Started %1").arg(QFileInfo(path).fileName()),
                             4000);
    const int row = model_->rowCount() - 1;
    if (row >= 0 && model_->idForRow(row) == id) {
        table_->selectRow(row);
    }
}

void MainWindow::handleIpcMessage(const QByteArray& payload) {
    if (payload.trimmed().isEmpty()) {
        // Plain "raise" ping from a re-launched instance.
        showNormal();
        raise();
        activateWindow();
        return;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        showNormal();
        raise();
        activateWindow();
        return;
    }

    const QJsonObject o = doc.object();

    // Video download from the extension's in-page panel: no dialog -- start it
    // straight away via yt-dlp under the manager's "video" path.
    if (o.value("type").toString() == "download-video") {
        showNormal();
        raise();
        activateWindow();
        const QString url = o.value("url").toString();
        if (url.isEmpty()) return;
        QList<QPair<QString, QString>> headers;
        const QJsonObject h = o.value("headers").toObject();
        for (auto it = h.begin(); it != h.end(); ++it) {
            headers.append({it.key(), it.value().toString()});
        }
        const QString outDir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString title = o.value("title").toString();
        const qint64 id = manager_->startVideo(url, o.value("selector").toString(),
                                               title, outDir, headers);
        statusBar()->showMessage(
            QString("Downloading video: %1").arg(title.isEmpty() ? url : title), 4000);
        const int row = model_->rowCount() - 1;
        if (row >= 0 && model_->idForRow(row) == id) table_->selectRow(row);
        return;
    }

    ExternalDownloadRequest req;
    req.url = o.value("url").toString();
    req.filename = o.value("filename").toString();
    req.dir = o.value("dir").toString();
    req.hash = o.value("hash").toString();
    const QJsonObject headers = o.value("headers").toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        req.headers.append({it.key(), it.value().toString()});
    }
    openExternalDownload(req);
}

void MainWindow::onRemoveClicked() {
    const qint64 id = currentDownloadId();
    if (id == 0) return;
    const auto row = manager_->row(id);
    if (!row) return;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Remove download");
    box.setText(QString("Remove '%1' from the list?")
                    .arg(QFileInfo(row->rec.outputPath).fileName()));

    auto* alsoDeleteFile = new QCheckBox("Also delete the downloaded file");
    box.setCheckBox(alsoDeleteFile);

    QAbstractButton* removeBtn = box.addButton("Remove", QMessageBox::AcceptRole);
    QAbstractButton* cancelBtn = box.addButton("Cancel", QMessageBox::RejectRole);
    // QMessageBox auto-assigns themed icons to buttons based on their role,
    // which gives the Cancel button a red-X icon on KDE/Breeze. Strip it so
    // the dialog reads as text-only.
    cancelBtn->setIcon(QIcon());
    removeBtn->setIcon(QIcon());

    box.exec();
    if (box.clickedButton() == removeBtn) {
        manager_->remove(id, alsoDeleteFile->isChecked());
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
    // Queued connection: lets the details window finish closing before we
    // pop the completion modal, so the modal lands against MainWindow
    // instead of stacking over a window that's about to disappear.
    connect(w, &DownloadDetailsWindow::downloadCompleted, this,
            &MainWindow::onDownloadCompleted, Qt::QueuedConnection);
    w->show();
}

void MainWindow::onDownloadCompleted(qint64 id) {
    const auto row = manager_->row(id);
    if (!row) return;
    const QString path = row->rec.outputPath;
    const QString filename = QFileInfo(path).fileName();

    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle("Download complete");
    box.setText(QString("'%1' finished downloading.").arg(filename));
    QAbstractButton* openFile =
        box.addButton("Open file", QMessageBox::ActionRole);
    QAbstractButton* showFolder =
        box.addButton("Show in folder", QMessageBox::ActionRole);
    QAbstractButton* closeBtn = box.addButton("Close", QMessageBox::RejectRole);
    openFile->setIcon(QIcon());
    showFolder->setIcon(QIcon());
    closeBtn->setIcon(QIcon());

    // Pop in front + grab focus before entering the modal event loop.
    box.show();
    box.raise();
    box.activateWindow();
    box.exec();

    if (box.clickedButton() == openFile) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else if (box.clickedButton() == showFolder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }
}

void MainWindow::onUpdateYtDlp() {
    statusBar()->showMessage("Updating video downloader…");
    manager_->updateVideoBackend([this](bool ok, QString msg) {
        statusBar()->showMessage(ok ? "Video downloader updated" : "Update failed", 5000);
        QMessageBox::information(this, ok ? "Video downloader" : "Update failed",
                                 msg.isEmpty() ? (ok ? "Up to date." : "Update failed.") : msg);
    });
}

void MainWindow::onTableContextMenu(const QPoint& pos) {
    // Select the row under the cursor first so action enabled-states match
    // what the menu is about to act on. customContextMenuRequested delivers
    // pos in viewport coordinates.
    const QModelIndex idx = table_->indexAt(pos);
    if (idx.isValid()) {
        table_->selectRow(idx.row());
    } else {
        table_->clearSelection();
    }

    QMenu menu(this);
    if (currentDownloadId() == 0) {
        // No row clicked -- offer the only action that makes sense in empty
        // space.
        menu.addAction(newAction_);
    } else {
        menu.addAction(pauseAction_);
        menu.addAction(resumeAction_);
        menu.addAction(cancelAction_);
        menu.addSeparator();
        menu.addAction(retryAction_);
        menu.addAction(redownloadAction_);
        menu.addSeparator();
        menu.addAction(detailsAction_);
        menu.addAction(openFolderAction_);
        menu.addSeparator();
        menu.addAction(removeAction_);
    }
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

}  // namespace fdm_gui
