#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

#include "DownloadDetailsWindow.h"
#include "DownloadItemDelegate.h"
#include "Icons.h"
#include "NewDownloadDialog.h"
#include "fdm/store/DownloadFilterProxyModel.h"
#include "fdm/store/DownloadListModel.h"
#include "fdm/store/DownloadManager.h"

namespace fdm_gui {

using fdm::store::DownloadFilterProxyModel;
using fdm::store::DownloadListModel;
using fdm::store::DownloadManager;
using fdm::store::DownloadStatus;

using Category = DownloadFilterProxyModel::Category;

namespace {

QString humanRate(double bytesPerSec) {
    constexpr double KiB = 1024.0;
    constexpr double MiB = 1024.0 * KiB;
    constexpr double GiB = 1024.0 * MiB;
    if (bytesPerSec >= GiB) return QString::number(bytesPerSec / GiB, 'f', 2) + " GiB/s";
    if (bytesPerSec >= MiB) return QString::number(bytesPerSec / MiB, 'f', 2) + " MiB/s";
    if (bytesPerSec >= KiB) return QString::number(bytesPerSec / KiB, 'f', 1) + " KiB/s";
    return QString::number(static_cast<qint64>(bytesPerSec)) + " B/s";
}

QString categoryLabel(Category c) {
    switch (c) {
        case Category::All:         return "All";
        case Category::Downloading: return "Downloading";
        case Category::Paused:      return "Paused";
        case Category::Completed:   return "Completed";
        case Category::Failed:      return "Failed";
        case Category::Videos:      return "Videos";
    }
    return {};
}

bool isHttpUrl(const QString& text) {
    const QUrl u(text);
    return u.isValid() && (u.scheme() == "http" || u.scheme() == "https");
}

}  // namespace

MainWindow::MainWindow(DownloadManager* manager, QWidget* parent)
    : QMainWindow(parent), manager_(manager) {
    setWindowTitle("Fresh Download Manager");
    resize(1000, 560);
    setAcceptDrops(true);

    buildUi();
    buildTopBarAndMenu();
    updateActionStates();
    updateAggregates();

    connect(manager_, &DownloadManager::rowAdded, this, &MainWindow::updateAggregates);
    connect(manager_, &DownloadManager::rowChanged, this, &MainWindow::updateAggregates);
    connect(manager_, &DownloadManager::rowRemoved, this, &MainWindow::updateAggregates);

    // Completion notifications: seed with current statuses so rows restored
    // from the DB don't fire, only genuine transitions do.
    for (const auto& row : manager_->rows()) {
        lastStatus_.insert(row.rec.id, static_cast<int>(row.rec.status));
    }
    connect(manager_, &DownloadManager::rowChanged, this,
            &MainWindow::onRowChangedForNotify);
    connect(manager_, &DownloadManager::rowRemoved, this,
            [this](qint64 id) { lastStatus_.remove(id); });

    // Land on whatever the user most likely cares about: live downloads if
    // any survived the restart, the full list otherwise.
    bool anyLive = false;
    for (const auto& row : manager_->rows()) {
        if (row.rec.status != DownloadStatus::Completed &&
            row.rec.status != DownloadStatus::Failed) {
            anyLive = true;
            break;
        }
    }
    sidebar_->setCurrentRow(anyLive ? 1 : 0);  // row order matches buildSidebar()
}

void MainWindow::buildUi() {
    model_ = new DownloadListModel(manager_, this);
    proxy_ = new DownloadFilterProxyModel(this);
    proxy_->setSourceModel(model_);

    list_ = new QListView;
    list_->setObjectName("downloadList");
    list_->setModel(proxy_);
    list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Hover feedback (row highlight + inline-button highlight in the
    // delegate) needs move events even with no button down. Mouse events
    // are delivered to the viewport, so tracking goes there.
    list_->viewport()->setMouseTracking(true);

    auto* delegate = new DownloadItemDelegate(list_);
    list_->setItemDelegate(delegate);
    // Clears the delegate's stale button-hover highlight when the cursor
    // leaves the rows (editorEvent only fires while over an item).
    list_->viewport()->installEventFilter(delegate);
    connect(delegate, &DownloadItemDelegate::actionTriggered, this,
            [this](qint64 id, DownloadItemDelegate::RowAction action) {
                using RowAction = DownloadItemDelegate::RowAction;
                switch (action) {
                    case RowAction::Pause:      manager_->pause(id); break;
                    case RowAction::Resume:     manager_->resume(id); break;
                    case RowAction::Cancel:     manager_->cancel(id); break;
                    case RowAction::Retry:      manager_->retry(id); break;
                    case RowAction::OpenFolder: openFolderFor(id); break;
                }
            });

    connect(list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(list_, &QListView::doubleClicked, this, &MainWindow::onRowDoubleClicked);

    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListView::customContextMenuRequested, this,
            &MainWindow::onTableContextMenu);

    emptyLabel_ = new QLabel;
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet("color: palette(mid); font-size: 14px;");

    stack_ = new QStackedWidget;
    stack_->addWidget(list_);        // page 0
    stack_->addWidget(emptyLabel_);  // page 1

    buildSidebar();

    auto* central = new QWidget;
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sidebar_);
    layout->addWidget(stack_, 1);
    setCentralWidget(central);

    statsLabel_ = new QLabel;
    statusBar()->addPermanentWidget(statsLabel_);
}

void MainWindow::buildSidebar() {
    sidebar_ = new QListWidget;
    sidebar_->setObjectName("sidebar");
    sidebar_->setFixedWidth(170);
    sidebar_->setSelectionMode(QAbstractItemView::SingleSelection);

    // Row order matters: the constructor selects row 1 ("Downloading") when
    // live downloads exist.
    const Category cats[] = {Category::All,       Category::Downloading,
                             Category::Paused,    Category::Completed,
                             Category::Failed,    Category::Videos};
    for (Category c : cats) {
        auto* item = new QListWidgetItem(categoryLabel(c), sidebar_);
        item->setData(Qt::UserRole, static_cast<int>(c));
    }

    connect(sidebar_, &QListWidget::currentRowChanged, this,
            &MainWindow::onCategoryChanged);
}

void MainWindow::buildTopBarAndMenu() {
    using icons::Glyph;
    newAction_ = new QAction(icons::icon(Glyph::Add), "&Add download…", this);
    newAction_->setShortcut(QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, &MainWindow::onNewDownloadClicked);

    addClipboardAction_ = new QAction("Add from &clipboard", this);
    addClipboardAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(addClipboardAction_, &QAction::triggered, this,
            &MainWindow::onAddFromClipboard);

    addVideoAction_ = new QAction(icons::icon(Glyph::Video), "Add &video…", this);
    connect(addVideoAction_, &QAction::triggered, this, &MainWindow::onAddVideoClicked);

    pauseAction_ = new QAction(icons::icon(Glyph::Pause), "&Pause", this);
    connect(pauseAction_, &QAction::triggered, this, &MainWindow::onPauseClicked);

    resumeAction_ = new QAction(icons::icon(Glyph::Play), "&Resume", this);
    connect(resumeAction_, &QAction::triggered, this, &MainWindow::onResumeClicked);

    cancelAction_ = new QAction(icons::icon(Glyph::Cancel), "&Cancel", this);
    connect(cancelAction_, &QAction::triggered, this, &MainWindow::onCancelClicked);

    retryAction_ = new QAction(icons::icon(Glyph::Retry), "Re&try", this);
    connect(retryAction_, &QAction::triggered, this, &MainWindow::onRetryClicked);

    redownloadAction_ = new QAction(icons::icon(Glyph::Redownload), "Re&download…", this);
    connect(redownloadAction_, &QAction::triggered, this, &MainWindow::onRedownloadClicked);

    removeAction_ = new QAction(icons::icon(Glyph::Trash), "Re&move from list", this);
    removeAction_->setShortcut(QKeySequence::Delete);
    // Widget-scoped so the Delete key still edits text in the search box;
    // it only removes rows while the list itself has focus.
    removeAction_->setShortcutContext(Qt::WidgetShortcut);
    list_->addAction(removeAction_);
    connect(removeAction_, &QAction::triggered, this, &MainWindow::onRemoveClicked);

    openFolderAction_ = new QAction(icons::icon(Glyph::Folder), "Open &folder", this);
    connect(openFolderAction_, &QAction::triggered, this, &MainWindow::onOpenFolderClicked);

    detailsAction_ = new QAction(icons::icon(Glyph::Details), "Show &details", this);
    detailsAction_->setShortcut(Qt::CTRL | Qt::Key_D);
    connect(detailsAction_, &QAction::triggered, this, &MainWindow::onDetailsClicked);

    pauseAllAction_ = new QAction(icons::icon(Glyph::Pause), "Pause a&ll", this);
    connect(pauseAllAction_, &QAction::triggered, this, &MainWindow::onPauseAllClicked);

    resumeAllAction_ = new QAction(icons::icon(Glyph::Play), "Resume al&l", this);
    connect(resumeAllAction_, &QAction::triggered, this, &MainWindow::onResumeAllClicked);

    removeCompletedAction_ = new QAction("Remove completed from list", this);
    connect(removeCompletedAction_, &QAction::triggered, this,
            &MainWindow::onRemoveCompletedClicked);

    auto* updateBackendAction = new QAction("&Update video downloader (yt-dlp)", this);
    connect(updateBackendAction, &QAction::triggered, this, &MainWindow::onUpdateYtDlp);

    auto* quitAction = new QAction("&Quit", this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // --- menu bar (keyboard/discoverability mirror of everything) ---------
    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction(newAction_);
    fileMenu->addAction(addClipboardAction_);
    fileMenu->addAction(addVideoAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

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
    dlMenu->addSeparator();
    dlMenu->addAction(pauseAllAction_);
    dlMenu->addAction(resumeAllAction_);
    dlMenu->addAction(removeCompletedAction_);

    auto* toolsMenu = menuBar()->addMenu("&Tools");
    toolsMenu->addAction(updateBackendAction);

    // --- top bar: Add ▾ | pause/resume all | …spacer… | search | overflow --
    auto* toolbar = addToolBar("Main");
    toolbar->setObjectName("topBar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* addButton = new QToolButton;
    addButton->setDefaultAction(newAction_);
    addButton->setPopupMode(QToolButton::MenuButtonPopup);
    addButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* addMenu = new QMenu(addButton);
    addMenu->addAction(addClipboardAction_);
    addMenu->addAction(addVideoAction_);
    addButton->setMenu(addMenu);
    toolbar->addWidget(addButton);

    toolbar->addSeparator();
    toolbar->addAction(pauseAllAction_);
    toolbar->addAction(resumeAllAction_);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    searchEdit_ = new QLineEdit;
    searchEdit_->setObjectName("searchBox");
    searchEdit_->setPlaceholderText("Search name or URL  (Ctrl+F)");
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setFixedWidth(240);
    connect(searchEdit_, &QLineEdit::textChanged, this,
            &MainWindow::onSearchTextChanged);
    toolbar->addWidget(searchEdit_);

    // Ctrl+V while the list has focus = "add from clipboard". Widget-scoped
    // so pasting into the search box keeps its normal meaning.
    auto* pasteAdd = new QAction(this);
    pasteAdd->setShortcut(QKeySequence::Paste);
    pasteAdd->setShortcutContext(Qt::WidgetShortcut);
    connect(pasteAdd, &QAction::triggered, this, &MainWindow::onAddFromClipboard);
    list_->addAction(pasteAdd);

    auto* focusSearch = new QAction(this);
    focusSearch->setShortcut(QKeySequence::Find);
    connect(focusSearch, &QAction::triggered, this, [this]() {
        searchEdit_->setFocus();
        searchEdit_->selectAll();
    });
    addAction(focusSearch);

    auto* overflowButton = new QToolButton;
    overflowButton->setIcon(icons::icon(Glyph::Dots));
    overflowButton->setPopupMode(QToolButton::InstantPopup);
    auto* overflowMenu = new QMenu(overflowButton);
    overflowMenu->addAction(removeCompletedAction_);
    overflowMenu->addAction(updateBackendAction);
    overflowMenu->addSeparator();
    overflowMenu->addAction(quitAction);
    overflowButton->setMenu(overflowMenu);
    toolbar->addWidget(overflowButton);
}

void MainWindow::installTray() {
    tray_ = new QSystemTrayIcon(windowIcon(), this);
    tray_->setToolTip("Fresh Download Manager");

    auto* menu = new QMenu(this);
    auto* openAction = menu->addAction("Open FDM");
    connect(openAction, &QAction::triggered, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    auto* quitAction = menu->addAction("Quit");
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    tray_->setContextMenu(menu);

    // Clicking the tray icon reopens the list (closing the window only hides
    // it; with quitOnLastWindowClosed=false the app keeps running here).
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    // Clicking the "Download complete" balloon reveals the file.
    connect(tray_, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (lastCompletedPath_.isEmpty()) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(lastCompletedPath_).absolutePath()));
    });
    tray_->show();
}

qint64 MainWindow::currentDownloadId() const {
    const QList<qint64> ids = selectedDownloadIds();
    return ids.isEmpty() ? 0 : ids.first();
}

QList<qint64> MainWindow::selectedDownloadIds() const {
    QList<qint64> ids;
    for (const QModelIndex& idx : list_->selectionModel()->selectedRows()) {
        const qint64 id = proxy_->idForRow(idx.row());
        if (id != 0) ids.append(id);
    }
    return ids;
}

void MainWindow::selectProxyRow(int proxyRow) {
    const QModelIndex idx = proxy_->index(proxyRow, 0);
    list_->selectionModel()->select(
        idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    list_->setCurrentIndex(idx);
}

void MainWindow::selectRowById(qint64 id) {
    for (int row = 0; row < proxy_->rowCount(); ++row) {
        if (proxy_->idForRow(row) == id) {
            selectProxyRow(row);
            return;
        }
    }
}

void MainWindow::showNewDownload(qint64 id) {
    // A just-started download is Queued/Active, so the Downloading category
    // is guaranteed to show it even if the user was browsing history.
    const Category current = proxy_->category();
    if (current != Category::All && current != Category::Downloading) {
        for (int row = 0; row < sidebar_->count(); ++row) {
            const auto cat = static_cast<Category>(
                sidebar_->item(row)->data(Qt::UserRole).toInt());
            if (cat == Category::Downloading) {
                sidebar_->setCurrentRow(row);
                break;
            }
        }
    }
    selectRowById(id);
}

void MainWindow::updateActionStates() {
    bool anyActive = false;
    bool anyPaused = false;
    bool anyResumable = false;
    bool anyFailed = false;
    const QList<qint64> ids = selectedDownloadIds();
    for (qint64 id : ids) {
        const auto row = manager_->row(id);
        if (!row) continue;
        anyActive |= row->rec.status == DownloadStatus::Active;
        anyPaused |= row->rec.status == DownloadStatus::Paused;
        // Paused rows resume; so do failed rows with resumable chunk state.
        anyResumable |= row->rec.status == DownloadStatus::Paused ||
                        manager_->canResumeFromChunks(id);
        anyFailed |= row->rec.status == DownloadStatus::Failed;
    }
    const bool single = ids.size() == 1;
    bool singleFinished = false;
    if (single) {
        const auto row = manager_->row(ids.first());
        singleFinished = row && (row->rec.status == DownloadStatus::Failed ||
                                 row->rec.status == DownloadStatus::Completed);
    }

    pauseAction_->setEnabled(anyActive);
    resumeAction_->setEnabled(anyResumable);
    cancelAction_->setEnabled(anyActive || anyPaused);
    retryAction_->setEnabled(anyFailed);
    redownloadAction_->setEnabled(singleFinished);
    removeAction_->setEnabled(!ids.isEmpty());
    openFolderAction_->setEnabled(single);
    detailsAction_->setEnabled(single);
}

void MainWindow::updateAggregates() {
    int total = 0, downloading = 0, paused = 0, completed = 0, failed = 0, videos = 0;
    double totalSpeed = 0.0;
    for (const auto& row : manager_->rows()) {
        ++total;
        switch (row.rec.status) {
            case DownloadStatus::Queued:
            case DownloadStatus::Active:
            case DownloadStatus::Finalizing:
                ++downloading;
                break;
            case DownloadStatus::Paused:    ++paused;    break;
            case DownloadStatus::Completed: ++completed; break;
            case DownloadStatus::Failed:    ++failed;    break;
        }
        if (row.rec.status == DownloadStatus::Active) totalSpeed += row.bytesPerSec;
        if (row.rec.kind == "video") ++videos;
    }

    for (int i = 0; i < sidebar_->count(); ++i) {
        QListWidgetItem* item = sidebar_->item(i);
        const auto cat = static_cast<Category>(item->data(Qt::UserRole).toInt());
        int count = 0;
        switch (cat) {
            case Category::All:         count = total;       break;
            case Category::Downloading: count = downloading; break;
            case Category::Paused:      count = paused;      break;
            case Category::Completed:   count = completed;   break;
            case Category::Failed:      count = failed;      break;
            case Category::Videos:      count = videos;      break;
        }
        item->setText(QString("%1  (%2)").arg(categoryLabel(cat)).arg(count));
    }

    if (downloading > 0 && totalSpeed > 0) {
        statsLabel_->setText(QString("↓ %1 · %2 active")
                                 .arg(humanRate(totalSpeed))
                                 .arg(downloading));
    } else if (downloading > 0) {
        statsLabel_->setText(QString("%1 active").arg(downloading));
    } else {
        statsLabel_->setText(total == 0 ? QString("No downloads")
                                        : QString("%1 downloads").arg(total));
    }

    if (proxy_->rowCount() == 0) {
        if (total == 0) {
            emptyLabel_->setText(
                "No downloads yet\nPress Ctrl+N, or copy a link and use Add ▾");
        } else if (!searchEdit_->text().trimmed().isEmpty()) {
            emptyLabel_->setText("No downloads match your search");
        } else {
            emptyLabel_->setText("Nothing in this category");
        }
        stack_->setCurrentIndex(1);
    } else {
        stack_->setCurrentIndex(0);
    }
}

void MainWindow::onSelectionChanged() {
    updateActionStates();
}

void MainWindow::onCategoryChanged() {
    QListWidgetItem* item = sidebar_->currentItem();
    if (!item) return;
    proxy_->setCategory(static_cast<Category>(item->data(Qt::UserRole).toInt()));
    updateAggregates();
    updateActionStates();
}

void MainWindow::onSearchTextChanged(const QString& text) {
    proxy_->setSearchText(text);
    updateAggregates();
    updateActionStates();
}

void MainWindow::onRowDoubleClicked(const QModelIndex& index) {
    const qint64 id = proxy_->idForRow(index.row());
    if (id == 0) return;
    // Double-clicking a finished file opens it; anything else opens the
    // live details window.
    const auto row = manager_->row(id);
    if (row && row->rec.status == DownloadStatus::Completed) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(row->rec.outputPath));
        return;
    }
    openDetailsFor(id);
}

void MainWindow::onRowChangedForNotify(qint64 id) {
    const auto row = manager_->row(id);
    if (!row) return;
    const int status = static_cast<int>(row->rec.status);
    const int previous = lastStatus_.value(id, -1);
    lastStatus_.insert(id, status);
    if (previous == status ||
        row->rec.status != DownloadStatus::Completed ||
        previous == -1) {
        return;
    }

    lastCompletedPath_ = row->rec.outputPath;
    const QString name = QFileInfo(row->rec.outputPath).fileName();
    statusBar()->showMessage(QString("Completed %1").arg(name), 6000);
    if (tray_) {
        tray_->showMessage("Download complete", name,
                           QSystemTrayIcon::Information, 6000);
    }
}

void MainWindow::addDownloadWithDialog(const QString& prefillUrl) {
    NewDownloadDialog dlg(manager_, this);
    if (!prefillUrl.isEmpty()) {
        dlg.prefill(prefillUrl, QString(), QString());
    }
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const qint64 id =
        manager_->startNew(url, path, dlg.userHash(), dlg.userHashAlgorithm());
    statusBar()->showMessage(QString("Started %1").arg(QFileInfo(path).fileName()),
                             4000);
    showNewDownload(id);
}

void MainWindow::onNewDownloadClicked() {
    addDownloadWithDialog(QString());
}

void MainWindow::onAddFromClipboard() {
    const QString text = QApplication::clipboard()->text().trimmed();
    addDownloadWithDialog(isHttpUrl(text) ? text : QString());
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (mime->hasUrls()) {
        for (const QUrl& u : mime->urls()) {
            if (u.scheme() == "http" || u.scheme() == "https") {
                event->acceptProposedAction();
                return;
            }
        }
    }
    if (mime->hasText() && isHttpUrl(mime->text().trimmed())) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData();
    QString url;
    if (mime->hasUrls()) {
        for (const QUrl& u : mime->urls()) {
            if (u.scheme() == "http" || u.scheme() == "https") {
                url = u.toString();
                break;
            }
        }
    }
    if (url.isEmpty() && mime->hasText() && isHttpUrl(mime->text().trimmed())) {
        url = mime->text().trimmed();
    }
    if (url.isEmpty()) return;
    event->acceptProposedAction();
    addDownloadWithDialog(url);
}

void MainWindow::onAddVideoClicked() {
    NewDownloadDialog dlg(manager_, this);
    dlg.setVideoMode(/*urlEditable=*/true);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString url = dlg.url();
    const QString path = dlg.outputPath();
    if (url.isEmpty() || path.isEmpty()) return;

    const QFileInfo fi(path);
    // Empty selector lets yt-dlp pick its default (best) format.
    const qint64 id = manager_->startVideo(url, QString(), fi.completeBaseName(),
                                           fi.absolutePath(), {});
    statusBar()->showMessage(
        QString("Downloading video: %1").arg(fi.completeBaseName()), 4000);
    showNewDownload(id);
}

void MainWindow::onPauseClicked() {
    for (qint64 id : selectedDownloadIds()) {
        const auto row = manager_->row(id);
        if (row && row->rec.status == DownloadStatus::Active) manager_->pause(id);
    }
}

void MainWindow::onResumeClicked() {
    for (qint64 id : selectedDownloadIds()) {
        const auto row = manager_->row(id);
        if (row && (row->rec.status == DownloadStatus::Paused ||
                    manager_->canResumeFromChunks(id)))
            manager_->resume(id);
    }
}

void MainWindow::onCancelClicked() {
    for (qint64 id : selectedDownloadIds()) {
        const auto row = manager_->row(id);
        if (row && (row->rec.status == DownloadStatus::Active ||
                    row->rec.status == DownloadStatus::Paused)) {
            manager_->cancel(id);
        }
    }
}

void MainWindow::onRetryClicked() {
    for (qint64 id : selectedDownloadIds()) {
        const auto row = manager_->row(id);
        if (row && row->rec.status == DownloadStatus::Failed) manager_->retry(id);
    }
}

void MainWindow::onPauseAllClicked() {
    for (const auto& row : manager_->rows()) {
        if (row.rec.status == DownloadStatus::Active) manager_->pause(row.rec.id);
    }
}

void MainWindow::onResumeAllClicked() {
    for (const auto& row : manager_->rows()) {
        if (row.rec.status == DownloadStatus::Paused) manager_->resume(row.rec.id);
    }
}

void MainWindow::onRemoveCompletedClicked() {
    QList<qint64> ids;
    for (const auto& row : manager_->rows()) {
        if (row.rec.status == DownloadStatus::Completed) ids.append(row.rec.id);
    }
    if (ids.isEmpty()) {
        statusBar()->showMessage("No completed downloads to remove", 4000);
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Remove completed");
    box.setText(QString("Remove %1 completed download%2 from the list?\n"
                        "Files on disk are kept.")
                    .arg(ids.size())
                    .arg(ids.size() == 1 ? "" : "s"));
    QAbstractButton* removeBtn = box.addButton("Remove", QMessageBox::AcceptRole);
    QAbstractButton* cancelBtn = box.addButton("Cancel", QMessageBox::RejectRole);
    cancelBtn->setIcon(QIcon());
    removeBtn->setIcon(QIcon());
    box.exec();
    if (box.clickedButton() != removeBtn) return;

    for (qint64 id : ids) manager_->remove(id, /*alsoRemoveFile=*/false);
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
    showNewDownload(newId);
    // Pop a details window for the restart -- this path also fires from a
    // details window's "Redownload…" button while the main list is hidden.
    openDetailsFor(newId);
}

void MainWindow::openExternalDownload(const ExternalDownloadRequest& req) {
    if (req.url.isEmpty()) return;
    if (!isHttpUrl(req.url)) return;

    // Stand-alone dialog (parent nullptr): for a browser-triggered download the
    // main list stays hidden in the tray, so the dialog -- and the details
    // window it opens on Start -- carry the whole flow on their own.
    NewDownloadDialog dlg(manager_, nullptr);
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
    // Show live progress in its own details window rather than the main list.
    // (This also wires up the completion modal.)
    openDetailsFor(id);
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
        const QString url = o.value("url").toString();
        if (url.isEmpty()) return;
        QList<QPair<QString, QString>> headers;
        const QJsonObject h = o.value("headers").toObject();
        for (auto it = h.begin(); it != h.end(); ++it) {
            headers.append({it.key(), it.value().toString()});
        }
        // Same New Download dialog as a direct download, in "video" mode (no
        // HTTP probe / hash -- yt-dlp owns extraction). We take the chosen
        // folder + base name; the real extension (mp4/mkv) is decided once
        // yt-dlp resolves the container, so the typed one is just a hint.
        QString suggested = o.value("title").toString();
        if (suggested.isEmpty()) suggested = QStringLiteral("video");
        suggested.replace('/', '_').replace('\\', '_');
        const QString defDir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

        NewDownloadDialog dlg(manager_, nullptr);
        dlg.setVideoMode();
        dlg.prefill(url, defDir, suggested + ".mp4");
        if (dlg.exec() != QDialog::Accepted) return;
        const QString out = dlg.outputPath();
        if (out.isEmpty()) return;
        const QFileInfo fi(out);
        const qint64 id =
            manager_->startVideo(url, o.value("selector").toString(),
                                 fi.completeBaseName(), fi.absolutePath(), headers);
        statusBar()->showMessage(
            QString("Downloading video: %1").arg(fi.completeBaseName()), 4000);
        openDetailsFor(id);
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
    const QList<qint64> ids = selectedDownloadIds();
    if (ids.isEmpty()) return;

    QString text;
    if (ids.size() == 1) {
        const auto row = manager_->row(ids.first());
        if (!row) return;
        text = QString("Remove '%1' from the list?")
                   .arg(QFileInfo(row->rec.outputPath).fileName());
    } else {
        text = QString("Remove %1 downloads from the list?").arg(ids.size());
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("Remove download");
    box.setText(text);

    auto* alsoDeleteFile = new QCheckBox(
        ids.size() == 1 ? "Also delete the downloaded file"
                        : "Also delete the downloaded files");
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
        for (qint64 id : ids) manager_->remove(id, alsoDeleteFile->isChecked());
    }
}

void MainWindow::onOpenFolderClicked() {
    const qint64 id = currentDownloadId();
    if (id != 0) openFolderFor(id);
}

void MainWindow::openFolderFor(qint64 id) {
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
    // With the main list on screen the row itself (plus the tray balloon)
    // already announces completion; the modal is only for the browser flow
    // where a lone details window was the whole UI.
    if (isVisible()) return;

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
    // If the click landed on a row outside the current selection, move the
    // selection there so the menu acts on what's under the cursor. A click
    // inside the selection keeps it, so multi-row actions work.
    // customContextMenuRequested delivers pos in viewport coordinates.
    const QModelIndex idx = list_->indexAt(pos);
    if (idx.isValid()) {
        if (!list_->selectionModel()->isRowSelected(idx.row(), QModelIndex())) {
            selectProxyRow(idx.row());
        }
    } else {
        list_->clearSelection();
    }

    QMenu menu(this);
    if (selectedDownloadIds().isEmpty()) {
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
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

}  // namespace fdm_gui
