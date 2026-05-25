#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>
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
