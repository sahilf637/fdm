#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

#include "fdm/DownloadEngine.h"
#include "fdm/EngineEvent.h"
#include "fdm/store/Database.h"

class QProcess;

namespace fdm::store {

// Qt-flavoured view of fdm::ProbeResult. Strings are QStrings so UI code
// can use them without round-tripping through std::string.
struct ProbeResult {
    bool ok = false;
    QString error;
    QString finalUrl;            // post-redirect URL
    QString suggestedFilename;   // server's Content-Disposition, sanitized
    qint64 contentLength = -1;
    bool supportsRanges = false;
};

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
    // Live connection telemetry from the engine's latest Progress (not
    // persisted): how many segments are downloading right now and the current
    // adaptive cap.
    int activeConnections = 0;
    int connectionLimit = 0;
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
    // Variant carrying an optional user-supplied expected hash (lowercased
    // hex) and algorithm name. When non-empty, the hash is persisted to the
    // row immediately and takes precedence over any server-side discovery.
    qint64 startNew(const QString& url, const QString& outputPath,
                    const QString& userHash, const QString& userHashAlgorithm);
    // Variant carrying extra request headers (Cookie / Referer / User-Agent,
    // e.g. forwarded from a browser extension). The headers are persisted and
    // replayed on resume, and sent on the probe + every chunk request.
    qint64 startNew(const QString& url, const QString& outputPath,
                    const QString& userHash, const QString& userHashAlgorithm,
                    const QList<QPair<QString, QString>>& headers);

    // Start a streaming-media download via yt-dlp (handles HLS/DASH/YouTube and
    // muxes audio+video with ffmpeg). `selector` is a yt-dlp -f expression;
    // `title` seeds the output filename under `outputDir`. Headers (cookies/UA)
    // are forwarded to yt-dlp. Returns the new row id (0 only if a row couldn't
    // be created). The row is marked kind="video" so pause/resume/cancel route
    // to the child-process path.
    qint64 startVideo(const QString& url, const QString& selector,
                      const QString& title, const QString& outputDir,
                      const QList<QPair<QString, QString>>& headers);

    // Probe a URL without registering a download. Callback fires on the UI
    // thread (the same thread `this` lives on). Useful for filling in a
    // server-suggested filename before the user commits to a save path.
    void probe(const QString& url, std::function<void(ProbeResult)> onResult);
    // Variant that sends extra request headers, so an authenticated URL still
    // resolves its Content-Disposition filename / size at probe time.
    void probe(const QString& url, const QList<QPair<QString, QString>>& headers,
               std::function<void(ProbeResult)> onResult);

    // Update the video backend in place (runs `yt-dlp -U`). `done` fires with
    // success + the tool's combined output. Keeps YouTube extraction working as
    // the site changes. No-op-safe if yt-dlp isn't found.
    void updateVideoBackend(std::function<void(bool, QString)> done);

    // Engine-control operations. All are id-addressed and idempotent: e.g.
    // pause() of an already-paused download is a no-op.
    void pause(qint64 id);
    void resume(qint64 id);   // continues a Paused download from saved chunks.
    void cancel(qint64 id);
    // Restart from byte zero -- drops chunk progress, clears errors, calls
    // engine.start() fresh. Use when the prior attempt failed in a way
    // resume can't recover from.
    void retry(qint64 id);
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
    // Write the row's current chunk set to the DB now: a full replaceChunks
    // when the layout grew/changed (so new rows are INSERTed), else a cheaper
    // in-place updateChunkProgress. Clears the dirty flag. No-op if empty.
    void flushChunks(qint64 id);
    // Build the queued engine-event callback for `id`: hops the event back onto
    // this object's thread before dispatching to onEngineEvent.
    std::function<void(fdm::EngineEvent)> makeEngineCallback(qint64 id);
    DownloadLiveRow* find(qint64 id);
    fdm::ResumeSpec buildResumeSpec(const DownloadLiveRow& row) const;
    // Drives a row from "bytes finished" to "verification verdict written".
    // Discovers the hash if not already known (no-op if it is), then kicks
    // off the QtConcurrent hashing job. Idempotent: safe to call on
    // already-Finalizing rows during startup recovery.
    void beginFinalize(qint64 id);
    void startHashJob(qint64 id);
    void onHashJobDone(qint64 id, const QString& actualHash);

    // --- video (yt-dlp child process) path, used for segmented HLS/DASH ------
    void spawnVideo(qint64 id);                       // (re)launch the child
    // Pull a plain (non-DRM) HLS/DASH manifest with ffmpeg alone (`-c copy`),
    // used as a fallback when yt-dlp isn't installed. Reuses the videoProcs_
    // registry so pause/cancel/finish route exactly like the yt-dlp child.
    void spawnFfmpegHls(qint64 id);
    void onVideoLine(qint64 id, const QString& line); // parse one output line
    void finishVideo(qint64 id, bool ok, const QString& error);
    // Apply a pending pause/cancel intent to a finished video row. Returns true
    // if an intent was handled (caller stops); false to proceed to ok/fail.
    // Callers do their own temp-file cleanup before calling.
    bool applyVideoIntent(qint64 id, const QString& intent);

    // --- video via the multi-connection engine + ffmpeg mux ------------------
    // resolveVideo runs `yt-dlp -J` to get the direct stream URL(s); single-file
    // (https) streams are downloaded by the engine in parallel and muxed with
    // ffmpeg, while segmented streams fall back to the yt-dlp child above.
    struct VideoEngineJob;
    void resolveVideo(qint64 id);
    // Escalation overload. `forceGeneric` adds --force-generic-extractor (retry a
    // page URL the site extractors can't read); `impersonate` adds curl_cffi
    // browser impersonation (last resort for Cloudflare 403s). Neither is used on
    // the first try, so YouTube etc. keep their normal extractor path.
    void resolveVideo(qint64 id, bool forceGeneric, bool impersonate);
    void handleResolved(qint64 id, const QByteArray& jsonOut);
    void startEngineStreams(qint64 id);
    void onVideoStreamEvent(qint64 id, int streamIdx, fdm::EngineEvent ev);
    void updateVideoJobProgress(qint64 id);
    void beginVideoMux(qint64 id);
    void finishEngineVideo(qint64 id, bool ok, const QString& error);

    Database* db_;
    std::unique_ptr<fdm::DownloadEngine> engine_;

    // Storage is keyed by id for O(1) lookup; rowOrder_ preserves listing
    // order for the model.
    QHash<qint64, DownloadLiveRow> rows_;
    QList<qint64> rowOrder_;

    // Last wall-clock time (ms since epoch) we wrote chunk progress for a
    // download. Throttles DB writes to avoid hammering SQLite at 10 Hz.
    QHash<qint64, qint64> lastPersistMs_;

    // Downloads whose chunk layout grew/changed since the last DB flush. The
    // next flush rewrites the whole chunk set (replaceChunks) for these instead
    // of an in-place update, so newly split segments get persisted.
    QSet<qint64> chunkLayoutDirty_;

    // Running yt-dlp child processes, keyed by download id. videoIntent_ records
    // why we're about to stop one ("cancel"/"pause") so the finished handler
    // writes the right terminal status; videoFinalPath_ holds the muxed file's
    // real path once yt-dlp reports it.
    QHash<qint64, QProcess*> videoProcs_;
    QHash<qint64, QString> videoIntent_;
    QHash<qint64, QString> videoFinalPath_;
    QHash<qint64, VideoEngineJob*> videoJobs_;  // engine-driven video downloads
    // Extra yt-dlp args (e.g. --force-generic-extractor, impersonation) that
    // succeeded at resolve time, replayed by the download child so it extracts
    // the same way. Empty/absent for the common first-try-succeeds case.
    QHash<qint64, QStringList> videoExtraYtdlpArgs_;
};

}  // namespace fdm::store
