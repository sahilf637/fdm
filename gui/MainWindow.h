#pragma once

#include <QList>
#include <QMainWindow>

#include "EngineBridge.h"

class QLineEdit;
class QPushButton;
class QProgressBar;
class QLabel;
class QTableWidget;

namespace fdm_gui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onBrowseClicked();
    void onUrlChanged(const QString& url);
    void onStartClicked();
    void onNewDownloadWindow();

    void onStarted(qint64 totalBytes, bool supportsRanges, int chunkCount,
                   const QList<ChunkRow>& chunks);
    void onProgressUpdated(qint64 received, qint64 total, double bytesPerSec,
                           const QList<ChunkRow>& chunks);
    void onFinished();
    void onFailed(const QString& reason);

private:
    void buildUi();
    void buildMenuAndToolbar();
    void applyProgressBarStyle(class QProgressBar* bar);
    void setFormEnabled(bool enabled);
    void refreshFilenameSuggestion(const QString& url);
    QString resolveSaveName(const QString& userName, const QString& url) const;

    // Form
    QLineEdit* urlEdit_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPushButton* startBtn_ = nullptr;

    // Status
    QLabel* statusLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QProgressBar* overallBar_ = nullptr;
    QTableWidget* chunkTable_ = nullptr;

    EngineBridge* bridge_ = nullptr;
};

}  // namespace fdm_gui
