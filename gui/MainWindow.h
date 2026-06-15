#pragma once

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QtGlobal>

class QAction;
class QItemSelection;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QStackedWidget;
class QSystemTrayIcon;

namespace fdm::store {
class DownloadFilterProxyModel;
class DownloadListModel;
class DownloadManager;
}  // namespace fdm::store

namespace fdm_gui {

class DownloadDetailsWindow;
class DownloadItemDelegate;

// A download handed to us from outside the app (browser extension via the
// native host, or the --add-download CLI arg). Headers carry auth context
// (Cookie / Referer / User-Agent) so protected downloads work.
struct ExternalDownloadRequest {
    QString url;
    QString filename;   // suggested leaf name (server-detected if empty)
    QString dir;        // save directory (dialog default if empty)
    QString hash;       // optional expected hash hex
    QList<QPair<QString, QString>> headers;
};

// Top-level download window: a category sidebar (Downloading / Completed /
// Failed / …) over the persisted download list, a top bar with add / batch
// pause-resume / live search, and per-row actions via context menu. Lets the
// user start new downloads, control existing ones, or open a details window
// per row.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `manager` is owned by the caller (typically main()) and must outlive
    // this window.
    explicit MainWindow(fdm::store::DownloadManager* manager, QWidget* parent = nullptr);

public:
    // Open the New Download dialog with the given row's URL + path pre-filled.
    // If accepted, creates a brand-new download (separate from `id`). Public
    // so DownloadDetailsWindow can request a redownload without duplicating
    // the dialog/select-row glue.
    void redownloadFromRow(qint64 id);

    // Raise the window and, if `req.url` is a valid http(s) URL, open the New
    // Download dialog pre-filled (forwarding auth headers on start).
    void openExternalDownload(const ExternalDownloadRequest& req);

    // Install a system-tray icon so the app can live in the background with the
    // main list hidden (downloads keep running). Tray menu: open the list /
    // quit. Called once at startup, only when a tray is available.
    void installTray();

public slots:
    // Entry point for IPC payloads (from SingleInstance). An empty/!object
    // payload just raises the window; otherwise it is parsed into an
    // ExternalDownloadRequest and opened.
    void handleIpcMessage(const QByteArray& payload);

protected:
    // Dropping an http(s) link anywhere on the window opens the New Download
    // dialog pre-filled with it.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onNewDownloadClicked();
    void onAddFromClipboard();
    void onAddVideoClicked();
    void onPauseClicked();
    void onResumeClicked();
    void onCancelClicked();
    void onRetryClicked();
    void onRedownloadClicked();
    void onRemoveClicked();
    void onOpenFolderClicked();
    void onDetailsClicked();
    void onPauseAllClicked();
    void onResumeAllClicked();
    void onRemoveCompletedClicked();
    void onSelectionChanged();
    void onCategoryChanged();
    void onSearchTextChanged(const QString& text);
    void onRowDoubleClicked(const QModelIndex& index);
    void onTableContextMenu(const QPoint& pos);
    void onDownloadCompleted(qint64 id);
    void onUpdateYtDlp();
    // Watches status transitions to Completed and raises a tray notification
    // (the non-modal replacement for the old completion popup).
    void onRowChangedForNotify(qint64 id);
    // Refresh sidebar counts, the status-bar aggregate line, and the
    // empty-state page. Connected to every manager row* signal.
    void updateAggregates();

private:
    void buildUi();
    void buildTopBarAndMenu();
    void buildSidebar();
    void updateActionStates();
    qint64 currentDownloadId() const;
    // Ids of all selected rows (proxy order). Empty when nothing is selected.
    QList<qint64> selectedDownloadIds() const;
    void openDetailsFor(qint64 id);
    void openFolderFor(qint64 id);
    // Shared "add a download" flow: open the dialog (optionally pre-filled
    // with a URL), start the accepted download, and reveal its row.
    void addDownloadWithDialog(const QString& prefillUrl);
    // Make `proxyRow` the only selected row (mirrors QTableView::selectRow).
    void selectProxyRow(int proxyRow);
    // Select the (proxy) row showing download `id`, if currently visible.
    void selectRowById(qint64 id);
    // Switch the sidebar/proxy to the category that shows fresh downloads,
    // then select `id`. Used after starting a download so the new row is
    // visible regardless of the current filter.
    void showNewDownload(qint64 id);

    fdm::store::DownloadManager* manager_;
    fdm::store::DownloadListModel* model_ = nullptr;
    fdm::store::DownloadFilterProxyModel* proxy_ = nullptr;
    QListWidget* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;  // page 0: list_, page 1: empty state
    QLabel* emptyLabel_ = nullptr;
    QListView* list_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;

    QAction* newAction_ = nullptr;
    QAction* addClipboardAction_ = nullptr;
    QAction* addVideoAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* resumeAction_ = nullptr;
    QAction* cancelAction_ = nullptr;
    QAction* retryAction_ = nullptr;
    QAction* redownloadAction_ = nullptr;
    QAction* removeAction_ = nullptr;
    QAction* openFolderAction_ = nullptr;
    QAction* detailsAction_ = nullptr;
    QAction* pauseAllAction_ = nullptr;
    QAction* resumeAllAction_ = nullptr;
    QAction* removeCompletedAction_ = nullptr;

    QHash<qint64, QPointer<DownloadDetailsWindow>> detailsWindows_;

    // Last known status per row, to detect transitions to Completed for the
    // tray notification (rowChanged fires for any field, at progress rate).
    QHash<qint64, int> lastStatus_;
    // Output path of the most recently completed download; the tray
    // notification's click target.
    QString lastCompletedPath_;
};

}  // namespace fdm_gui
