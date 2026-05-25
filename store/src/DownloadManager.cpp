#include "fdm/store/DownloadManager.h"

#include <QDateTime>
#include <QFile>
#include <QMetaObject>

#include <utility>
#include <variant>

namespace fdm::store {

namespace {

constexpr qint64 kPersistThrottleMs = 2000;  // flush chunk progress at most every 2s

ChunkPersistStatus toPersistStatus(fdm::ChunkProgress::Status s) {
    switch (s) {
        case fdm::ChunkProgress::Status::Done:   return ChunkPersistStatus::Done;
        case fdm::ChunkProgress::Status::Failed: return ChunkPersistStatus::Failed;
        case fdm::ChunkProgress::Status::Active: return ChunkPersistStatus::Active;
    }
    return ChunkPersistStatus::Active;
}

fdm::ChunkProgress::Status fromPersistStatus(ChunkPersistStatus s) {
    switch (s) {
        case ChunkPersistStatus::Done:   return fdm::ChunkProgress::Status::Done;
        case ChunkPersistStatus::Failed: return fdm::ChunkProgress::Status::Failed;
        case ChunkPersistStatus::Active: return fdm::ChunkProgress::Status::Active;
    }
    return fdm::ChunkProgress::Status::Active;
}

QList<ChunkRecord> chunksFromSpecs(const std::vector<fdm::ChunkSpec>& specs) {
    QList<ChunkRecord> out;
    out.reserve(static_cast<int>(specs.size()));
    for (const fdm::ChunkSpec& s : specs) {
        ChunkRecord c;
        c.index = s.index;
        c.startByte = s.startByte;
        c.endByte = s.endByte;
        c.bytesReceived = 0;
        c.attempts = 1;
        c.status = ChunkPersistStatus::Active;
        out.append(c);
    }
    return out;
}

QList<ChunkRecord> chunksFromProgress(const std::vector<fdm::ChunkProgress>& progress) {
    QList<ChunkRecord> out;
    out.reserve(static_cast<int>(progress.size()));
    for (const fdm::ChunkProgress& p : progress) {
        ChunkRecord c;
        c.index = p.index;
        c.startByte = p.startByte;
        c.endByte = p.endByte;
        c.bytesReceived = p.bytesReceived;
        c.attempts = p.attempts;
        c.status = toPersistStatus(p.status);
        out.append(c);
    }
    return out;
}

qint64 sumChunkBytes(const QList<ChunkRecord>& chunks) {
    qint64 sum = 0;
    for (const ChunkRecord& c : chunks) sum += c.bytesReceived;
    return sum;
}

}  // namespace

DownloadManager::DownloadManager(Database* db, QObject* parent)
    : QObject(parent), db_(db) {
    // First: any download that was Active in the DB was interrupted by the
    // previous process exit (we are by definition not running yet). Demote
    // before loading the in-memory mirror so the UI sees them as Paused.
    db_->markInterruptedAsPaused();

    for (const DownloadRecord& rec : db_->listDownloads()) {
        DownloadLiveRow row;
        row.rec = rec;
        row.chunks = db_->chunksFor(rec.id);
        row.bytesReceived = sumChunkBytes(row.chunks);
        rows_.insert(rec.id, row);
        rowOrder_.append(rec.id);
    }

    engine_ = std::make_unique<fdm::DownloadEngine>();
}

DownloadManager::~DownloadManager() = default;

QList<DownloadLiveRow> DownloadManager::rows() const {
    QList<DownloadLiveRow> out;
    out.reserve(rowOrder_.size());
    for (qint64 id : rowOrder_) {
        auto it = rows_.find(id);
        if (it != rows_.end()) out.append(*it);
    }
    return out;
}

std::optional<DownloadLiveRow> DownloadManager::row(qint64 id) const {
    auto it = rows_.find(id);
    if (it == rows_.end()) return std::nullopt;
    return *it;
}

qint64 DownloadManager::idAt(int row) const {
    if (row < 0 || row >= rowOrder_.size()) return 0;
    return rowOrder_[row];
}

DownloadLiveRow* DownloadManager::find(qint64 id) {
    auto it = rows_.find(id);
    return it == rows_.end() ? nullptr : &*it;
}

qint64 DownloadManager::startNew(const QString& url, const QString& outputPath) {
    DownloadRecord rec;
    rec.url = url;
    rec.outputPath = outputPath;
    rec.totalBytes = -1;
    rec.supportsRanges = false;
    rec.status = DownloadStatus::Active;
    rec.createdAt = QDateTime::currentSecsSinceEpoch();
    const qint64 id = db_->insertDownload(rec);

    DownloadLiveRow row;
    row.rec = rec;
    row.rec.id = id;
    rows_.insert(id, row);
    rowOrder_.append(id);
    emit rowAdded(id);

    DownloadLiveRow* live = find(id);
    live->engineId = engine_->start(
        url.toStdString(), outputPath.toStdString(),
        [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
    return id;
}

void DownloadManager::probe(const QString& url,
                            std::function<void(ProbeResult)> onResult) {
    engine_->probe(
        url.toStdString(),
        [this, cb = std::move(onResult)](fdm::ProbeResult pr) {
            ProbeResult ui;
            ui.ok = pr.ok;
            ui.error = QString::fromStdString(pr.error);
            ui.finalUrl = QString::fromStdString(pr.info.finalUrl);
            ui.suggestedFilename = QString::fromStdString(pr.info.suggestedFilename);
            ui.contentLength = pr.info.contentLength;
            ui.supportsRanges = pr.info.supportsRanges;
            // Hop back to the UI thread (= the thread this QObject lives on).
            QMetaObject::invokeMethod(
                this, [cb, ui]() { cb(ui); }, Qt::QueuedConnection);
        });
}

fdm::ResumeSpec DownloadManager::buildResumeSpec(const DownloadLiveRow& row) const {
    fdm::ResumeSpec spec;
    spec.url = row.rec.url.toStdString();
    spec.outputPath = row.rec.outputPath.toStdString();
    spec.totalBytes = row.rec.totalBytes;
    spec.supportsRanges = row.rec.supportsRanges;
    spec.chunks.reserve(row.chunks.size());
    for (const ChunkRecord& c : row.chunks) {
        fdm::ChunkRestore r;
        r.index = c.index;
        r.startByte = c.startByte;
        r.endByte = c.endByte;
        r.bytesReceived = c.bytesReceived;
        r.attempts = c.attempts;
        r.status = fromPersistStatus(c.status);
        spec.chunks.push_back(r);
    }
    return spec;
}

void DownloadManager::pause(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId == 0) {
        // Not currently in the engine -- nothing to ask the engine to do.
        // If we somehow got here for an Active row, sync the DB to Paused.
        if (row->rec.status == DownloadStatus::Active) {
            row->rec.status = DownloadStatus::Paused;
            db_->updateDownloadStatus(id, DownloadStatus::Paused);
            emit rowChanged(id);
        }
        return;
    }
    engine_->pause(row->engineId);
}

void DownloadManager::resume(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId != 0) {
        // It's still in the engine and paused: just resume in place.
        engine_->resume(row->engineId);
        row->rec.status = DownloadStatus::Active;
        db_->updateDownloadStatus(id, DownloadStatus::Active);
        emit rowChanged(id);
        return;
    }

    // Cross-restart resume: hand the engine the persisted chunk state.
    // If we have no chunks persisted (download never got past probe), fall
    // back to a fresh start which will probe and split again.
    if (row->chunks.isEmpty() || row->rec.totalBytes < 0) {
        row->rec.status = DownloadStatus::Active;
        row->rec.error.clear();
        db_->updateDownloadStatus(id, DownloadStatus::Active);
        emit rowChanged(id);
        row->engineId = engine_->start(
            row->rec.url.toStdString(), row->rec.outputPath.toStdString(),
            [this, id](fdm::EngineEvent ev) {
                QMetaObject::invokeMethod(
                    this, [this, id, ev = std::move(ev)]() mutable {
                        onEngineEvent(id, std::move(ev));
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    fdm::ResumeSpec spec = buildResumeSpec(*row);
    row->rec.status = DownloadStatus::Active;
    row->rec.error.clear();
    db_->updateDownloadStatus(id, DownloadStatus::Active);
    emit rowChanged(id);
    row->engineId = engine_->resumeKnown(
        std::move(spec), [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
}

void DownloadManager::retry(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    // Reset live state and DB metadata so the upcoming engine.start() begins
    // from a clean slate. The engine's preallocateFile uses O_TRUNC, so the
    // existing partial file is overwritten -- no manual file removal needed.
    row->chunks.clear();
    row->bytesReceived = 0;
    row->bytesPerSec = 0;
    row->rec.totalBytes = -1;
    row->rec.supportsRanges = false;
    row->rec.status = DownloadStatus::Active;
    row->rec.error.clear();

    db_->updateDownloadStatus(id, DownloadStatus::Active);
    db_->updateDownloadTotals(id, -1, false);
    db_->replaceChunks(id, {});
    lastPersistMs_.remove(id);
    emit rowChanged(id);

    row->engineId = engine_->start(
        row->rec.url.toStdString(), row->rec.outputPath.toStdString(),
        [this, id](fdm::EngineEvent ev) {
            QMetaObject::invokeMethod(
                this, [this, id, ev = std::move(ev)]() mutable {
                    onEngineEvent(id, std::move(ev));
                },
                Qt::QueuedConnection);
        });
}

void DownloadManager::cancel(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    row->rec.status = DownloadStatus::Failed;
    row->rec.error = "Cancelled";
    db_->updateDownloadStatus(id, DownloadStatus::Failed, "Cancelled");
    emit rowChanged(id);
}

void DownloadManager::remove(qint64 id, bool alsoRemoveFile) {
    DownloadLiveRow* row = find(id);
    if (!row) return;
    if (row->engineId != 0) {
        engine_->cancel(row->engineId);
        row->engineId = 0;
    }
    const QString filePath = row->rec.outputPath;
    db_->deleteDownload(id);
    rows_.remove(id);
    rowOrder_.removeAll(id);
    lastPersistMs_.remove(id);
    if (alsoRemoveFile && !filePath.isEmpty()) {
        QFile::remove(filePath);
    }
    emit rowRemoved(id);
}

void DownloadManager::onEngineEvent(qint64 id, fdm::EngineEvent ev) {
    DownloadLiveRow* row = find(id);
    if (!row) return;  // row was removed before this event landed

    std::visit(
        [&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, fdm::Started>) {
                row->rec.totalBytes = e.contentLength;
                row->rec.supportsRanges = e.supportsRanges;
                row->chunks = chunksFromSpecs(e.chunks);
                row->bytesReceived = 0;
                // Persist totals + initial chunk layout in one swoop. The
                // Active status was set when the user started/resumed; we
                // don't churn it here.
                db_->updateDownloadTotals(id, e.contentLength, e.supportsRanges);
                db_->replaceChunks(id, row->chunks);
                lastPersistMs_[id] = QDateTime::currentMSecsSinceEpoch();
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Progress>) {
                row->chunks = chunksFromProgress(e.chunks);
                row->bytesReceived = e.received;
                // EMA on the raw 100ms-window rate the engine reports.
                constexpr double kAlpha = 0.2;
                row->bytesPerSec = (row->bytesPerSec <= 0)
                                       ? e.bytesPerSec
                                       : kAlpha * e.bytesPerSec +
                                             (1.0 - kAlpha) * row->bytesPerSec;
                persistProgressThrottled(id);
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Paused>) {
                row->rec.status = DownloadStatus::Paused;
                row->bytesPerSec = 0;
                db_->updateDownloadStatus(id, DownloadStatus::Paused);
                // Always flush on a state transition; throttling is only
                // for the high-frequency Progress path.
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Finished>) {
                row->engineId = 0;
                row->rec.status = DownloadStatus::Completed;
                row->bytesPerSec = 0;
                if (row->rec.totalBytes > 0) {
                    row->bytesReceived = row->rec.totalBytes;
                }
                db_->updateDownloadStatus(id, DownloadStatus::Completed);
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                emit rowChanged(id);
            } else if constexpr (std::is_same_v<T, fdm::Failed>) {
                row->engineId = 0;
                row->rec.status = DownloadStatus::Failed;
                row->rec.error = QString::fromStdString(e.reason);
                row->bytesPerSec = 0;
                db_->updateDownloadStatus(id, DownloadStatus::Failed, row->rec.error);
                if (!row->chunks.isEmpty()) {
                    db_->updateChunkProgress(id, row->chunks);
                }
                emit rowChanged(id);
            }
        },
        ev);
}

void DownloadManager::persistProgressThrottled(qint64 id) {
    DownloadLiveRow* row = find(id);
    if (!row || row->chunks.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = lastPersistMs_.value(id, 0);
    if (now - last < kPersistThrottleMs) return;
    lastPersistMs_[id] = now;
    db_->updateChunkProgress(id, row->chunks);
}

}  // namespace fdm::store
