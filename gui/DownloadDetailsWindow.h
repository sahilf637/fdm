#pragma once

#include <QHash>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include "fdm/store/Database.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace fdm::store {
class DownloadManager;
}

namespace fdm_gui {

// One details window per download. Opened from MainWindow's "Details" action.
// Subscribes to DownloadManager signals so the chunks table + summary stay
// in sync without polling.
class DownloadDetailsWindow : public QWidget {
    Q_OBJECT
public:
    DownloadDetailsWindow(fdm::store::DownloadManager* manager, qint64 id,
                          QWidget* parent = nullptr);

signals:
    // Fired when this window's download transitions Active -> Completed.
    // The window closes itself immediately after emitting; MainWindow is
    // expected to surface the follow-up "Open file / Show in folder" modal
    // so it can pop fresh against the main window rather than over the
    // about-to-die details window.
    void downloadCompleted(qint64 id);

private slots:
    void onPauseClicked();
    void onResumeClicked();
    void onCancelClicked();
    void onRetryClicked();
    void onRedownloadClicked();
    void onRowChanged(qint64 id);
    void onRowRemoved(qint64 id);

private:
    void buildUi();
    void refresh();
    void rebuildChunkRows(int chunkCount);

    fdm::store::DownloadManager* manager_;
    qint64 id_;
    // Tracks the previous status so we can detect "just transitioned to
    // Completed" and show the completion modal exactly once.
    fdm::store::DownloadStatus lastStatus_ = fdm::store::DownloadStatus::Queued;
    bool lastStatusKnown_ = false;

    QLabel* nameLabel_ = nullptr;
    QLabel* urlLabel_ = nullptr;
    QLabel* pathLabel_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QProgressBar* overallBar_ = nullptr;
    QTableWidget* chunkTable_ = nullptr;
    QPushButton* pauseBtn_ = nullptr;
    QPushButton* resumeBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* retryBtn_ = nullptr;
    QPushButton* redownloadBtn_ = nullptr;
};

}  // namespace fdm_gui
