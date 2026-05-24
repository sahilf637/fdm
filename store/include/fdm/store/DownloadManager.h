#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <memory>
#include <optional>

#include "fdm/DownloadEngine.h"
#include "fdm/EngineEvent.h"
#include "fdm/store/Database.h"

namespace fdm::store {

// In-memory live row: a persisted DownloadRecord plus the volatile, runtime-
// only fields the engine pushes (current sum-of-bytes, smoothed speed, last
// chunk snapshot). Decoupled from QSqlTableModel deliberately: we want to
// own the model semantics (status transitions, throttled persistence) rather
// than let Qt's auto-CRUD drive them.
struct DownloadLiveRow {
    DownloadRecord rec;
    qint64 bytesReceived = 0;
    double bytesPerSec = 0.0;
    fdm::DownloadId engineId = 0;  // 0 when not currently in the engine
    QList<ChunkRecord> chunks;
};

// Coordinates the persistent store, the libcurl engine, and the live in-
// memory mirror that the UI observes. Lives on (and must be touched only
// from) the UI thread; engine callbacks are queued via QMetaObject::invoke.
class DownloadManager : public QObject {
    Q_OBJECT
public:
    // `db` is owned by the caller and must outlive the manager.
    explicit DownloadManager(Database* db, QObject* parent = nullptr);
    ~DownloadManager() override;

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    // Snapshot of the in-memory list, ordered by insertion id (stable).
    QList<DownloadLiveRow> rows() const;
    std::optional<DownloadLiveRow> row(qint64 id) const;
    int rowCount() const { return rowOrder_.size(); }
    qint64 idAt(int row) const;

    // Insert a row in the DB and start it in the engine. Returns the new id.
    qint64 startNew(const QString& url, const QString& outputPath);

    // Engine-control operations. All are id-addressed and idempotent: e.g.
    // pause() of an already-paused download is a no-op.
    void pause(qint64 id);
    void resume(qint64 id);   // works for Paused; also restarts Failed.
    void cancel(qint64 id);
    // Remove from DB. `alsoRemoveFile` deletes the (possibly partial) file
    // on disk too. If the download is currently active, it is cancelled
    // first.
    void remove(qint64 id, bool alsoRemoveFile);

signals:
    void rowAdded(qint64 id);
    void rowChanged(qint64 id);
    void rowRemoved(qint64 id);

private:
    void onEngineEvent(qint64 id, fdm::EngineEvent ev);
    void persistChunks(qint64 id);
    void persistProgressThrottled(qint64 id);
    DownloadLiveRow* find(qint64 id);
    fdm::ResumeSpec buildResumeSpec(const DownloadLiveRow& row) const;

    Database* db_;
    std::unique_ptr<fdm::DownloadEngine> engine_;

    // Storage is keyed by id for O(1) lookup; rowOrder_ preserves listing
    // order for the model.
    QHash<qint64, DownloadLiveRow> rows_;
    QList<qint64> rowOrder_;

    // Last wall-clock time (ms since epoch) we wrote chunk progress for a
    // download. Throttles DB writes to avoid hammering SQLite at 10 Hz.
    QHash<qint64, qint64> lastPersistMs_;
};

}  // namespace fdm::store
