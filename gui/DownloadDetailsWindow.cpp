#include "DownloadDetailsWindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "MainWindow.h"
#include "fdm/store/DownloadManager.h"

namespace fdm_gui {

using fdm::store::ChunkPersistStatus;
using fdm::store::ChunkRecord;
using fdm::store::DownloadLiveRow;
using fdm::store::DownloadManager;
using fdm::store::DownloadStatus;

namespace {

constexpr int kColIndex = 0;
constexpr int kColRange = 1;
constexpr int kColReceived = 2;
constexpr int kColPercent = 3;
constexpr int kColAttempts = 4;
constexpr int kColStatus = 5;
constexpr int kColCount = 6;

const char* const kSmoothProgressStyle = R"(
QProgressBar {
    border: 1px solid palette(mid);
    border-radius: 4px;
    background-color: palette(base);
    text-align: center;
    min-height: 14px;
}
QProgressBar::chunk {
    background-color: #3a82e6;
    border-radius: 3px;
    margin: 0px;
}
)";

QString humanBytes(qint64 bytes) {
    if (bytes < 0) return "—";
    constexpr qint64 KiB = 1024;
    constexpr qint64 MiB = 1024 * KiB;
    constexpr qint64 GiB = 1024 * MiB;
    if (bytes >= GiB)
        return QString::number(static_cast<double>(bytes) / GiB, 'f', 2) + " GiB";
    if (bytes >= MiB)
        return QString::number(static_cast<double>(bytes) / MiB, 'f', 2) + " MiB";
    if (bytes >= KiB)
        return QString::number(static_cast<double>(bytes) / KiB, 'f', 1) + " KiB";
    return QString::number(bytes) + " B";
}

QString humanRate(double bytesPerSec) {
    if (bytesPerSec <= 0) return "—";
    return humanBytes(static_cast<qint64>(bytesPerSec)) + "/s";
}

QString statusText(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Queued:    return "Queued";
        case DownloadStatus::Active:    return "Downloading";
        case DownloadStatus::Paused:    return "Paused";
        case DownloadStatus::Completed: return "Completed";
        case DownloadStatus::Failed:    return "Failed";
    }
    return "?";
}

QString chunkStatusText(ChunkPersistStatus s) {
    switch (s) {
        case ChunkPersistStatus::Active: return "Active";
        case ChunkPersistStatus::Done:   return "Done";
        case ChunkPersistStatus::Failed: return "Failed";
    }
    return "?";
}

}  // namespace

DownloadDetailsWindow::DownloadDetailsWindow(DownloadManager* manager, qint64 id, QWidget* parent)
    : QWidget(parent, Qt::Window), manager_(manager), id_(id) {
    buildUi();
    connect(manager_, &DownloadManager::rowChanged, this, &DownloadDetailsWindow::onRowChanged);
    connect(manager_, &DownloadManager::rowRemoved, this, &DownloadDetailsWindow::onRowRemoved);
    refresh();
}

void DownloadDetailsWindow::buildUi() {
    setWindowTitle("Download Details");
    resize(720, 480);

    nameLabel_ = new QLabel;
    nameLabel_->setStyleSheet("font-weight: bold; font-size: 14px;");
    urlLabel_ = new QLabel;
    urlLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    urlLabel_->setWordWrap(true);
    pathLabel_ = new QLabel;
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel_->setWordWrap(true);
    sizeLabel_ = new QLabel;
    statusLabel_ = new QLabel;
    speedLabel_ = new QLabel;

    overallBar_ = new QProgressBar;
    overallBar_->setRange(0, 1000);
    overallBar_->setFormat("%p%");
    overallBar_->setStyleSheet(kSmoothProgressStyle);

    chunkTable_ = new QTableWidget(0, kColCount);
    chunkTable_->setHorizontalHeaderLabels(
        {"#", "Range", "Received", "Progress", "Attempts", "Status"});
    chunkTable_->verticalHeader()->setVisible(false);
    chunkTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    chunkTable_->setSelectionMode(QAbstractItemView::NoSelection);
    chunkTable_->setFocusPolicy(Qt::NoFocus);
    chunkTable_->setShowGrid(false);
    chunkTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    chunkTable_->horizontalHeader()->setSectionResizeMode(kColPercent, QHeaderView::Stretch);
    chunkTable_->setAlternatingRowColors(true);

    pauseBtn_ = new QPushButton("Pause");
    resumeBtn_ = new QPushButton("Resume");
    cancelBtn_ = new QPushButton("Cancel");
    retryBtn_ = new QPushButton("Retry");
    redownloadBtn_ = new QPushButton("Redownload…");
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(retryBtn_);
    btnRow->addWidget(redownloadBtn_);
    btnRow->addWidget(pauseBtn_);
    btnRow->addWidget(resumeBtn_);
    btnRow->addWidget(cancelBtn_);
    connect(pauseBtn_, &QPushButton::clicked, this, &DownloadDetailsWindow::onPauseClicked);
    connect(resumeBtn_, &QPushButton::clicked, this, &DownloadDetailsWindow::onResumeClicked);
    connect(cancelBtn_, &QPushButton::clicked, this, &DownloadDetailsWindow::onCancelClicked);
    connect(retryBtn_, &QPushButton::clicked, this, &DownloadDetailsWindow::onRetryClicked);
    connect(redownloadBtn_, &QPushButton::clicked, this,
            &DownloadDetailsWindow::onRedownloadClicked);

    auto* root = new QVBoxLayout(this);
    root->addWidget(nameLabel_);
    root->addWidget(urlLabel_);
    root->addWidget(pathLabel_);

    auto* metaRow = new QHBoxLayout;
    metaRow->addWidget(sizeLabel_);
    metaRow->addStretch();
    metaRow->addWidget(statusLabel_);
    metaRow->addSpacing(20);
    metaRow->addWidget(speedLabel_);
    root->addLayout(metaRow);

    root->addWidget(overallBar_);
    root->addWidget(chunkTable_, 1);
    root->addLayout(btnRow);
}

void DownloadDetailsWindow::rebuildChunkRows(int chunkCount) {
    chunkTable_->setRowCount(chunkCount);
    for (int r = 0; r < chunkCount; ++r) {
        if (!chunkTable_->item(r, kColIndex)) {
            chunkTable_->setItem(r, kColIndex, new QTableWidgetItem);
            chunkTable_->setItem(r, kColRange, new QTableWidgetItem);
            chunkTable_->setItem(r, kColReceived, new QTableWidgetItem);
            chunkTable_->setItem(r, kColAttempts, new QTableWidgetItem);
            chunkTable_->setItem(r, kColStatus, new QTableWidgetItem);
            auto* bar = new QProgressBar;
            bar->setRange(0, 1000);
            bar->setFormat("%p%");
            bar->setStyleSheet(kSmoothProgressStyle);
            chunkTable_->setCellWidget(r, kColPercent, bar);
        }
    }
}

void DownloadDetailsWindow::refresh() {
    const auto opt = manager_->row(id_);
    if (!opt) return;
    const DownloadLiveRow& row = *opt;

    nameLabel_->setText(QFileInfo(row.rec.outputPath).fileName());
    urlLabel_->setText("URL: " + row.rec.url);
    pathLabel_->setText("Path: " + row.rec.outputPath);
    sizeLabel_->setText("Size: " + humanBytes(row.rec.totalBytes));

    QString status = "Status: " + statusText(row.rec.status);
    if (!row.rec.error.isEmpty()) status += " (" + row.rec.error + ")";
    statusLabel_->setText(status);
    speedLabel_->setText("Speed: " + humanRate(row.bytesPerSec));

    if (row.rec.totalBytes > 0) {
        const int permille = static_cast<int>(
            (row.bytesReceived * 1000) / row.rec.totalBytes);
        overallBar_->setRange(0, 1000);
        overallBar_->setValue(permille);
    } else {
        overallBar_->setRange(0, 0);
    }

    rebuildChunkRows(row.chunks.size());
    for (int i = 0; i < row.chunks.size(); ++i) {
        const ChunkRecord& c = row.chunks[i];
        const int r = c.index;
        if (r < 0 || r >= chunkTable_->rowCount()) continue;
        chunkTable_->item(r, kColIndex)->setText(QString::number(c.index));
        const QString range =
            c.endByte >= 0
                ? QString("%1 – %2").arg(humanBytes(c.startByte), humanBytes(c.endByte))
                : QString("from %1").arg(humanBytes(c.startByte));
        chunkTable_->item(r, kColRange)->setText(range);
        chunkTable_->item(r, kColReceived)->setText(humanBytes(c.bytesReceived));
        const qint64 chunkSize = (c.endByte >= 0) ? (c.endByte - c.startByte + 1) : -1;
        if (auto* bar = qobject_cast<QProgressBar*>(chunkTable_->cellWidget(r, kColPercent))) {
            if (chunkSize > 0) {
                bar->setRange(0, 1000);
                bar->setValue(static_cast<int>((c.bytesReceived * 1000) / chunkSize));
            } else {
                bar->setRange(0, 0);
            }
        }
        chunkTable_->item(r, kColAttempts)->setText(QString::number(c.attempts));
        chunkTable_->item(r, kColStatus)->setText(chunkStatusText(c.status));
    }

    const bool active = row.rec.status == DownloadStatus::Active;
    const bool paused = row.rec.status == DownloadStatus::Paused;
    const bool failed = row.rec.status == DownloadStatus::Failed;
    const bool completed = row.rec.status == DownloadStatus::Completed;
    // Show only the actions relevant for the current state. Hidden (rather
    // than just disabled) so the user isn't faced with a wall of greyed-out
    // buttons.
    pauseBtn_->setVisible(active);
    resumeBtn_->setVisible(paused);
    cancelBtn_->setVisible(active || paused);
    retryBtn_->setVisible(failed);
    redownloadBtn_->setVisible(failed || completed);
}

void DownloadDetailsWindow::onRowChanged(qint64 id) {
    if (id != id_) return;
    const auto opt = manager_->row(id_);
    if (!opt) return;

    const DownloadStatus newStatus = opt->rec.status;
    refresh();

    // Detect Active -> Completed transition. Close this window first, then
    // signal MainWindow to surface the completion modal -- the slot is
    // wired with Qt::QueuedConnection so it runs after this window has
    // been hidden, not stacked over it. Opening details on an already-
    // completed row does not re-fire the modal (lastStatusKnown_ guard).
    const bool justCompleted = lastStatusKnown_ &&
                               lastStatus_ != DownloadStatus::Completed &&
                               newStatus == DownloadStatus::Completed;
    lastStatus_ = newStatus;
    lastStatusKnown_ = true;
    if (justCompleted) {
        const qint64 id = id_;
        close();
        emit downloadCompleted(id);
    }
}

void DownloadDetailsWindow::onRowRemoved(qint64 id) {
    if (id == id_) close();
}

void DownloadDetailsWindow::onPauseClicked() { manager_->pause(id_); }
void DownloadDetailsWindow::onResumeClicked() { manager_->resume(id_); }

void DownloadDetailsWindow::onCancelClicked() {
    // Cancel the running download, then dismiss this window. The "Failed"
    // state will still be visible in the main download list afterwards.
    manager_->cancel(id_);
    close();
}

void DownloadDetailsWindow::onRetryClicked() { manager_->retry(id_); }

void DownloadDetailsWindow::onRedownloadClicked() {
    // Walk up to the owning MainWindow and delegate -- it owns the
    // NewDownloadDialog flow and the row-selection bookkeeping so a new
    // download lands in the same place a user-driven "+ New" would.
    auto* main = qobject_cast<MainWindow*>(window()->parentWidget());
    // qobject_cast against parent fails: this window is top-level. Look up
    // the MainWindow via the topLevelWidgets list instead.
    if (!main) {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if ((main = qobject_cast<MainWindow*>(w))) break;
        }
    }
    if (main) main->redownloadFromRow(id_);
}

}  // namespace fdm_gui
