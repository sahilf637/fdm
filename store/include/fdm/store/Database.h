#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace fdm::store {

// Lifecycle of a download as persisted on disk. Stored as a TEXT column for
// readability when poking the SQLite file with the `sqlite3` CLI; the
// runtime overhead is irrelevant for a tool that updates rows at <= 10 Hz.
enum class DownloadStatus {
    Queued,      // user added it but the engine hasn't started running it yet
    Active,      // engine is downloading bytes right now
    Paused,      // user paused, or app shut down while Active
    Finalizing,  // bytes on disk, hash discovery + verification running
    Completed,
    Failed,
};

enum class ChunkPersistStatus { Active, Done, Failed };

// Lifecycle of the optional hash check. `Pending` means we know we WILL try
// to verify (a hash has been recorded); `Unverified` means we tried and
// concluded there's no hash available -- not a failure.
enum class VerificationStatus {
    None,        // no verification attempted yet (default for fresh rows)
    Pending,
    Verifying,
    Verified,
    Mismatch,
    Unverified,  // no hash discoverable from any source
};

QString verificationStatusToString(VerificationStatus s);
VerificationStatus verificationStatusFromString(const QString& s);

QString downloadStatusToString(DownloadStatus s);
DownloadStatus downloadStatusFromString(const QString& s);
QString chunkStatusToString(ChunkPersistStatus s);
ChunkPersistStatus chunkStatusFromString(const QString& s);

struct DownloadRecord {
    qint64 id = 0;             // 0 means "not yet inserted"
    QString url;
    QString outputPath;
    qint64 totalBytes = -1;    // -1 until probed
    bool supportsRanges = false;
    DownloadStatus status = DownloadStatus::Queued;
    QString error;
    qint64 createdAt = 0;      // unix seconds
    qint64 updatedAt = 0;
    // Hash verification fields. expectedHash / hashAlgorithm / hashSource
    // identify the digest we'll compare against; actualHash is filled
    // post-verification. All empty when no hash is known.
    QString expectedHash;
    QString hashAlgorithm;     // "sha256" | "sha1" | "md5"
    QString hashSource;        // "user" | "content-digest" | "digest" | "content-md5" | "sidecar-*"
    QString actualHash;
    VerificationStatus verification = VerificationStatus::None;
    // Extra request headers to replay on resume, stored as a JSON array of
    // raw "Name: value" lines (e.g. cookies/referer forwarded from a browser).
    // Empty for downloads started by typing a URL into the dialog.
    QString requestHeaders;
};

struct ChunkRecord {
    int index = 0;
    qint64 startByte = 0;
    qint64 endByte = -1;
    qint64 bytesReceived = 0;
    int attempts = 1;
    ChunkPersistStatus status = ChunkPersistStatus::Active;
};

// Thin, single-threaded wrapper around a QSqlDatabase connection. Owns its
// own connection name so multiple Database instances in the same process do
// not collide (useful for tests). All methods are intended to be called from
// the UI thread; QSqlDatabase is not thread-safe by default.
class Database {
public:
    // Opens (or creates) the SQLite database at `dbPath` and applies the
    // schema. Throws std::runtime_error on failure.
    explicit Database(const QString& dbPath);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Inserts a new row, returns its assigned id. created_at/updated_at are
    // stamped from the current wall clock.
    qint64 insertDownload(const DownloadRecord& rec);

    // Updates the status and (optionally) error message; bumps updated_at.
    void updateDownloadStatus(qint64 id, DownloadStatus s,
                              const QString& error = QString());

    // Persists post-probe metadata (total size and range support).
    void updateDownloadTotals(qint64 id, qint64 totalBytes, bool supportsRanges);

    // Removes the row and (via ON DELETE CASCADE) all its chunks.
    void deleteDownload(qint64 id);

    // Replaces all chunks for the download with `chunks`. Run inside a
    // transaction. Use when the engine first hands us the chunk split.
    void replaceChunks(qint64 downloadId, const QList<ChunkRecord>& chunks);

    // Updates per-chunk bytes_received / attempts / status. Run inside a
    // transaction.  Rows must already exist (use replaceChunks first).
    void updateChunkProgress(qint64 downloadId, const QList<ChunkRecord>& chunks);

    // Read APIs.
    QList<DownloadRecord> listDownloads() const;
    std::optional<DownloadRecord> getDownload(qint64 id) const;
    QList<ChunkRecord> chunksFor(qint64 downloadId) const;

    // Persist the expected hash + its source after it's been discovered
    // (from user input, headers, or sidecar). Also bumps verification to
    // Pending if it was None.
    void updateExpectedHash(qint64 id, const QString& hash,
                            const QString& algorithm, const QString& source);

    // Persist the actual hash + final verification verdict after the local
    // hash compute finishes.
    void updateVerificationResult(qint64 id, const QString& actualHash,
                                  VerificationStatus result);

    // Set the verification column without other changes.
    void updateVerificationStatus(qint64 id, VerificationStatus s);

    // On app startup: any download persisted as Active was interrupted by
    // the previous process exit. Demote to Paused so the user can resume.
    // Returns the number of rows updated.
    int markInterruptedAsPaused();

private:
    QString connectionName_;
};

}  // namespace fdm::store
