#pragma once

#include <QHash>
#include <QString>
#include <QWidget>
#include <QtGlobal>

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

private slots:
    void onPauseClicked();
    void onResumeClicked();
    void onCancelClicked();
    void onRowChanged(qint64 id);
    void onRowRemoved(qint64 id);

private:
    void buildUi();
    void refresh();
    void rebuildChunkRows(int chunkCount);

    fdm::store::DownloadManager* manager_;
    qint64 id_;

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
};

}  // namespace fdm_gui
