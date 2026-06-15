#include "DownloadDetailsWindow.h"

#include <QApplication>
#include <QColor>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include "ElidedLabel.h"
#include "MainWindow.h"
#include "fdm/store/DownloadManager.h"

namespace fdm_gui {

using fdm::store::ChunkPersistStatus;
using fdm::store::ChunkRecord;
using fdm::store::DownloadLiveRow;
using fdm::store::DownloadManager;
using fdm::store::DownloadStatus;
using fdm::store::VerificationStatus;

namespace {

constexpr int kColIndex = 0;
constexpr int kColRange = 1;
constexpr int kColReceived = 2;
constexpr int kColPercent = 3;
constexpr int kColAttempts = 4;
constexpr int kColStatus = 5;
constexpr int kColCount = 6;

// Matches theme::accent() / theme::statusColor() in Theme.cpp so this window
// reads as part of the same app as the redesigned main list.
constexpr const char* kAccent = "#3b82f6";

// A muted-but-always-readable text colour derived from the active palette
// (60% real text + 40% background). Theme-safe: stays visible in both light and
// dark themes, unlike palette(mid) which can collapse onto the window colour.
QString mutedColor(const QWidget* w) {
    const QColor fg = w->palette().color(QPalette::WindowText);
    const QColor bg = w->palette().color(QPalette::Window);
    const QColor m((fg.red() * 60 + bg.red() * 40) / 100,
                   (fg.green() * 60 + bg.green() * 40) / 100,
                   (fg.blue() * 60 + bg.blue() * 40) / 100);
    return m.name();
}

// Progress-bar style with a per-status fill colour. The %-text colour is left to
// the style so it stays legible over both the track and the fill.
QString barStyle(const char* fill) {
    return QString(R"(
QProgressBar {
    border: none;
    border-radius: 5px;
    background-color: palette(midlight);
    text-align: center;
    min-height: 16px;
}
QProgressBar::chunk {
    background-color: %1;
    border-radius: 5px;
})")
        .arg(fill);
}

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

QString humanEta(qint64 secs) {
    if (secs < 0) return "—";
    const qint64 h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString statusText(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Queued:     return "Queued";
        case DownloadStatus::Active:     return "Downloading";
        case DownloadStatus::Paused:     return "Paused";
        case DownloadStatus::Finalizing: return "Finalizing…";
        case DownloadStatus::Completed:  return "Completed";
        case DownloadStatus::Failed:     return "Failed";
    }
    return "?";
}

// Status pill colour.
const char* statusColor(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Active:     return kAccent;
        case DownloadStatus::Paused:     return "#bf8700";
        case DownloadStatus::Completed:  return "#2da44e";
        case DownloadStatus::Failed:     return "#d1242f";
        case DownloadStatus::Finalizing: return "#8a63d2";
        case DownloadStatus::Queued:     return "#6b7280";
    }
    return "#6b7280";
}

const char* chunkColor(ChunkPersistStatus s) {
    switch (s) {
        case ChunkPersistStatus::Done:    return "#2da44e";
        case ChunkPersistStatus::Active:  return kAccent;
        case ChunkPersistStatus::Pending: return "#bf8700";
        case ChunkPersistStatus::Failed:  return "#d1242f";
    }
    return kAccent;
}

QString chunkStatusText(const ChunkRecord& c) {
    switch (c.status) {
        case ChunkPersistStatus::Active:  return "Downloading";
        case ChunkPersistStatus::Pending:
            return c.attempts > 1 ? QString("Retry #%1").arg(c.attempts - 1) : QString("Waiting");
        case ChunkPersistStatus::Done:    return "Done";
        case ChunkPersistStatus::Failed:  return "Failed";
    }
    return "?";
}

// A caption above a value, for the stats strip.
QLabel* addStat(QGridLayout* grid, int col, const QString& caption,
                const QString& mutedCol, QLabel** valueOut) {
    auto* cap = new QLabel(caption);
    cap->setStyleSheet(QString("color:%1; font-size:11px;").arg(mutedCol));
    auto* val = new QLabel("—");
    val->setStyleSheet("color: palette(window-text); font-size: 14px; font-weight: 600;");
    grid->addWidget(cap, 0, col);
    grid->addWidget(val, 1, col);
    *valueOut = val;
    return val;
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
    resize(760, 520);

    const QString muted = mutedColor(this);

    // --- header: name + status pill -----------------------------------------
    nameLabel_ = new ElidedLabel;
    nameLabel_->setStyleSheet("color: palette(window-text); font-size: 15px; font-weight: 700;");
    statusPill_ = new QLabel;
    statusPill_->setAlignment(Qt::AlignCenter);

    auto* headerRow = new QHBoxLayout;
    headerRow->addWidget(nameLabel_, 1);
    headerRow->addWidget(statusPill_, 0, Qt::AlignRight | Qt::AlignVCenter);

    urlLabel_ = new ElidedLabel;
    urlLabel_->setStyleSheet(QString("color:%1;").arg(muted));
    pathLabel_ = new ElidedLabel;
    pathLabel_->setStyleSheet(QString("color:%1;").arg(muted));

    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet("color: palette(midlight);");

    // --- stats strip: speed | ETA | size | connections ----------------------
    auto* stats = new QGridLayout;
    stats->setHorizontalSpacing(28);
    addStat(stats, 0, "Speed", muted, &speedLabel_);
    addStat(stats, 1, "ETA", muted, &etaLabel_);
    addStat(stats, 2, "Downloaded", muted, &sizeLabel_);
    addStat(stats, 3, "Connections", muted, &connLabel_);
    stats->setColumnStretch(4, 1);

    overallBar_ = new QProgressBar;
    overallBar_->setRange(0, 1000);
    overallBar_->setFormat("%p%");
    overallBar_->setStyleSheet(barStyle(kAccent));

    // Verification badge / error detail line. Wrap long errors instead of
    // stretching the window (error text contains spaces, so wrapping works).
    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet(QString("color:%1; font-size: 12px;").arg(muted));

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
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);
    root->addLayout(headerRow);
    root->addWidget(urlLabel_);
    root->addWidget(pathLabel_);
    root->addWidget(divider);
    root->addLayout(stats);
    root->addWidget(overallBar_);
    root->addWidget(statusLabel_);
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
            chunkTable_->setCellWidget(r, kColPercent, bar);
        }
    }
}

void DownloadDetailsWindow::refresh() {
    const auto opt = manager_->row(id_);
    if (!opt) return;
    const DownloadLiveRow& row = *opt;
    const DownloadStatus st = row.rec.status;
    const bool active = st == DownloadStatus::Active;

    nameLabel_->setFullText(QFileInfo(row.rec.outputPath).fileName());
    statusPill_->setText(statusText(st));
    // Soft pill (translucent fill + coloured text), same treatment as the
    // main list's status pills.
    const QColor pillColor(statusColor(st));
    statusPill_->setStyleSheet(
        QString("background: rgba(%1,%2,%3,22%); color:%4; border-radius:9px;"
                " padding:2px 12px; font-size:12px; font-weight:600;")
            .arg(pillColor.red())
            .arg(pillColor.green())
            .arg(pillColor.blue())
            .arg(pillColor.name()));
    urlLabel_->setFullText("URL: " + row.rec.url);
    pathLabel_->setFullText("Path: " + row.rec.outputPath);

    speedLabel_->setText(active ? humanRate(row.bytesPerSec) : "—");

    // ETA from the smoothed rate.
    QString eta = "—";
    if (active && row.bytesPerSec > 0 && row.rec.totalBytes > 0 &&
        row.rec.totalBytes > row.bytesReceived) {
        eta = humanEta(static_cast<qint64>(
            (row.rec.totalBytes - row.bytesReceived) / row.bytesPerSec));
    }
    etaLabel_->setText(eta);

    sizeLabel_->setText(humanBytes(row.bytesReceived) + " / " +
                        humanBytes(row.rec.totalBytes));

    connLabel_->setText(row.connectionLimit > 0
                            ? QString("%1 / %2").arg(row.activeConnections)
                                  .arg(row.connectionLimit)
                            : QString("—"));

    // Verification badge / error detail.
    QString detail;
    if (!row.rec.error.isEmpty()) detail = row.rec.error;
    if (row.rec.verification == VerificationStatus::Verified &&
        !row.rec.hashAlgorithm.isEmpty()) {
        detail = QString("✓ %1 verified (%2)")
                     .arg(row.rec.hashAlgorithm.toUpper(), row.rec.hashSource);
    }
    statusLabel_->setText(detail);
    statusLabel_->setVisible(!detail.isEmpty());

    if (st == DownloadStatus::Finalizing) {
        overallBar_->setRange(0, 1000);
        overallBar_->setValue(1000);
    } else if (row.rec.totalBytes > 0) {
        overallBar_->setRange(0, 1000);
        overallBar_->setValue(static_cast<int>((row.bytesReceived * 1000) / row.rec.totalBytes));
    } else {
        overallBar_->setRange(0, 0);
    }

    // Segment table -- render by row position (segment indices are sparse and
    // grow at runtime, so index != row).
    rebuildChunkRows(row.chunks.size());
    for (int r = 0; r < row.chunks.size(); ++r) {
        const ChunkRecord& c = row.chunks[r];
        chunkTable_->item(r, kColIndex)->setText(QString::number(c.index));
        const QString range =
            c.endByte >= 0
                ? QString("%1 – %2").arg(humanBytes(c.startByte), humanBytes(c.endByte))
                : QString("from %1").arg(humanBytes(c.startByte));
        chunkTable_->item(r, kColRange)->setText(range);
        chunkTable_->item(r, kColReceived)->setText(humanBytes(c.bytesReceived));
        const qint64 chunkSize = (c.endByte >= 0) ? (c.endByte - c.startByte + 1) : -1;
        if (auto* bar = qobject_cast<QProgressBar*>(chunkTable_->cellWidget(r, kColPercent))) {
            bar->setStyleSheet(barStyle(chunkColor(c.status)));
            if (c.status == ChunkPersistStatus::Done) {
                bar->setRange(0, 1000);
                bar->setValue(1000);
            } else if (chunkSize > 0) {
                bar->setRange(0, 1000);
                bar->setValue(static_cast<int>((c.bytesReceived * 1000) / chunkSize));
            } else {
                bar->setRange(0, 0);
            }
        }
        chunkTable_->item(r, kColAttempts)->setText(QString::number(c.attempts));
        chunkTable_->item(r, kColStatus)->setText(chunkStatusText(c));
    }

    const bool paused = st == DownloadStatus::Paused;
    const bool failed = st == DownloadStatus::Failed;
    const bool completed = st == DownloadStatus::Completed;
    const bool finalizing = st == DownloadStatus::Finalizing;
    pauseBtn_->setVisible(active && !finalizing);
    // A failed download with partial chunk state resumes from where it
    // stopped (Retry restarts from byte zero).
    resumeBtn_->setVisible(paused || manager_->canResumeFromChunks(id_));
    cancelBtn_->setVisible((active || paused) && !finalizing);
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
    // been hidden, not stacked over it.
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
    if (!main) {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if ((main = qobject_cast<MainWindow*>(w))) break;
        }
    }
    if (main) main->redownloadFromRow(id_);
}

}  // namespace fdm_gui
