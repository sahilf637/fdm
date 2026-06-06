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
class QTableView;

namespace fdm::store {
class DownloadListModel;
class DownloadManager;
}  // namespace fdm::store

namespace fdm_gui {

class DownloadDetailsWindow;

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

// Top-level download-history window. Lists every persisted download (live
// and historical) and lets the user start new ones, pause/resume/cancel
// existing ones, or open a details window per row.
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

public slots:
    // Entry point for IPC payloads (from SingleInstance). An empty/!object
    // payload just raises the window; otherwise it is parsed into an
    // ExternalDownloadRequest and opened.
    void handleIpcMessage(const QByteArray& payload);

private slots:
    void onNewDownloadClicked();
    void onPauseClicked();
    void onResumeClicked();
    void onCancelClicked();
    void onRetryClicked();
    void onRedownloadClicked();
    void onRemoveClicked();
    void onOpenFolderClicked();
    void onDetailsClicked();
    void onSelectionChanged();
    void onRowDoubleClicked(const QModelIndex& index);
    void onTableContextMenu(const QPoint& pos);
    void onDownloadCompleted(qint64 id);
    void onUpdateYtDlp();

private:
    void buildUi();
    void buildToolbarAndMenu();
    void updateActionStates();
    qint64 currentDownloadId() const;
    void openDetailsFor(qint64 id);

    fdm::store::DownloadManager* manager_;
    fdm::store::DownloadListModel* model_ = nullptr;
    QTableView* table_ = nullptr;

    QAction* newAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* resumeAction_ = nullptr;
    QAction* cancelAction_ = nullptr;
    QAction* retryAction_ = nullptr;
    QAction* redownloadAction_ = nullptr;
    QAction* removeAction_ = nullptr;
    QAction* openFolderAction_ = nullptr;
    QAction* detailsAction_ = nullptr;

    QHash<qint64, QPointer<DownloadDetailsWindow>> detailsWindows_;
};

}  // namespace fdm_gui
