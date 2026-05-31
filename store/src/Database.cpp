#include "fdm/store/Database.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <stdexcept>

namespace fdm::store {

namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS downloads (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    url             TEXT NOT NULL,
    output_path     TEXT NOT NULL,
    total_bytes     INTEGER NOT NULL DEFAULT -1,
    supports_ranges INTEGER NOT NULL DEFAULT 0,
    status          TEXT NOT NULL,
    error           TEXT,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    expected_hash   TEXT,
    hash_algorithm  TEXT,
    hash_source     TEXT,
    actual_hash     TEXT,
    verification    TEXT,
    request_headers TEXT
);

CREATE TABLE IF NOT EXISTS chunks (
    download_id    INTEGER NOT NULL,
    chunk_index    INTEGER NOT NULL,
    start_byte     INTEGER NOT NULL,
    end_byte       INTEGER NOT NULL DEFAULT -1,
    bytes_received INTEGER NOT NULL DEFAULT 0,
    attempts       INTEGER NOT NULL DEFAULT 1,
    status         TEXT NOT NULL,
    PRIMARY KEY (download_id, chunk_index),
    FOREIGN KEY (download_id) REFERENCES downloads(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_downloads_status ON downloads(status);
)SQL";

qint64 nowSeconds() {
    return QDateTime::currentSecsSinceEpoch();
}

// Convenience: throw with the last QSqlError on a query so callers don't
// have to bool-check every step.
void exec(QSqlQuery& q, const char* what) {
    if (!q.exec()) {
        throw std::runtime_error(
            QString("SQL %1 failed: %2").arg(what, q.lastError().text()).toStdString());
    }
}

void execText(QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        throw std::runtime_error(
            QString("SQL exec failed: %1\n  query: %2")
                .arg(q.lastError().text(), sql)
                .toStdString());
    }
}

}  // namespace

QString downloadStatusToString(DownloadStatus s) {
    switch (s) {
        case DownloadStatus::Queued:     return "queued";
        case DownloadStatus::Active:     return "active";
        case DownloadStatus::Paused:     return "paused";
        case DownloadStatus::Finalizing: return "finalizing";
        case DownloadStatus::Completed:  return "completed";
        case DownloadStatus::Failed:     return "failed";
    }
    return "queued";
}

DownloadStatus downloadStatusFromString(const QString& s) {
    if (s == "active")     return DownloadStatus::Active;
    if (s == "paused")     return DownloadStatus::Paused;
    if (s == "finalizing") return DownloadStatus::Finalizing;
    if (s == "completed")  return DownloadStatus::Completed;
    if (s == "failed")     return DownloadStatus::Failed;
    return DownloadStatus::Queued;
}

QString verificationStatusToString(VerificationStatus s) {
    switch (s) {
        case VerificationStatus::None:       return "";
        case VerificationStatus::Pending:    return "pending";
        case VerificationStatus::Verifying:  return "verifying";
        case VerificationStatus::Verified:   return "verified";
        case VerificationStatus::Mismatch:   return "mismatch";
        case VerificationStatus::Unverified: return "unverified";
    }
    return "";
}

VerificationStatus verificationStatusFromString(const QString& s) {
    if (s == "pending")    return VerificationStatus::Pending;
    if (s == "verifying")  return VerificationStatus::Verifying;
    if (s == "verified")   return VerificationStatus::Verified;
    if (s == "mismatch")   return VerificationStatus::Mismatch;
    if (s == "unverified") return VerificationStatus::Unverified;
    return VerificationStatus::None;
}

QString chunkStatusToString(ChunkPersistStatus s) {
    switch (s) {
        case ChunkPersistStatus::Active:  return "active";
        case ChunkPersistStatus::Pending: return "pending";
        case ChunkPersistStatus::Done:    return "done";
        case ChunkPersistStatus::Failed:  return "failed";
    }
    return "active";
}

ChunkPersistStatus chunkStatusFromString(const QString& s) {
    if (s == "pending") return ChunkPersistStatus::Pending;
    if (s == "done")    return ChunkPersistStatus::Done;
    if (s == "failed")  return ChunkPersistStatus::Failed;
    return ChunkPersistStatus::Active;
}

Database::Database(const QString& dbPath) {
    // Make sure the parent directory exists before SQLite tries to create
    // the file inside it.
    const QFileInfo info(dbPath);
    QDir().mkpath(info.absolutePath());

    // Unique connection name so multiple Database instances (e.g. in tests)
    // do not collide on the default connection.
    connectionName_ = QStringLiteral("fdm-db-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    db.setDatabaseName(dbPath);
    if (!db.open()) {
        throw std::runtime_error(
            QString("open sqlite at %1: %2").arg(dbPath, db.lastError().text()).toStdString());
    }

    // Sane defaults for a desktop app: WAL gives us readers-don't-block-writers
    // and survives crashes cleanly; NORMAL synchronous is the standard SQLite
    // recommendation for application use (FULL is overkill, OFF risks corruption).
    execText(db, "PRAGMA journal_mode = WAL");
    execText(db, "PRAGMA synchronous = NORMAL");
    execText(db, "PRAGMA foreign_keys = ON");

    // Apply schema in one shot. SQLite allows multiple statements via
    // QSqlQuery::exec only if separated; split on `;`.
    const QStringList statements = QString(kSchema).split(';', Qt::SkipEmptyParts);
    for (const QString& s : statements) {
        const QString trimmed = s.trimmed();
        if (trimmed.isEmpty()) continue;
        execText(db, trimmed);
    }

    // Additive migration: SQLite has no `ADD COLUMN IF NOT EXISTS`, and the
    // schema above only fires for fresh DBs (CREATE TABLE IF NOT EXISTS).
    // For DBs created before these columns existed, walk the actual
    // table_info and ADD COLUMN for each one we need.
    QStringList existing;
    {
        QSqlQuery info(db);
        if (info.exec("PRAGMA table_info(downloads)")) {
            while (info.next()) existing.append(info.value(1).toString());
        }
    }
    struct Col { const char* name; const char* ddl; };
    const Col required[] = {
        {"expected_hash",  "expected_hash TEXT"},
        {"hash_algorithm", "hash_algorithm TEXT"},
        {"hash_source",    "hash_source TEXT"},
        {"actual_hash",    "actual_hash TEXT"},
        {"verification",   "verification TEXT"},
        {"request_headers", "request_headers TEXT"},
    };
    for (const Col& c : required) {
        if (existing.contains(c.name)) continue;
        execText(db, QString("ALTER TABLE downloads ADD COLUMN %1").arg(c.ddl));
    }
}

Database::~Database() {
    {
        // Connection must go out of scope before removeDatabase.
        QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

qint64 Database::insertDownload(const DownloadRecord& rec) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO downloads"
        " (url, output_path, total_bytes, supports_ranges, status, error, created_at, updated_at,"
        "  expected_hash, hash_algorithm, hash_source, actual_hash, verification, request_headers)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    const qint64 now = nowSeconds();
    q.addBindValue(rec.url);
    q.addBindValue(rec.outputPath);
    q.addBindValue(static_cast<qlonglong>(rec.totalBytes));
    q.addBindValue(rec.supportsRanges ? 1 : 0);
    q.addBindValue(downloadStatusToString(rec.status));
    q.addBindValue(rec.error.isEmpty() ? QVariant() : QVariant(rec.error));
    q.addBindValue(static_cast<qlonglong>(rec.createdAt ? rec.createdAt : now));
    q.addBindValue(static_cast<qlonglong>(now));
    q.addBindValue(rec.expectedHash.isEmpty() ? QVariant() : QVariant(rec.expectedHash));
    q.addBindValue(rec.hashAlgorithm.isEmpty() ? QVariant() : QVariant(rec.hashAlgorithm));
    q.addBindValue(rec.hashSource.isEmpty() ? QVariant() : QVariant(rec.hashSource));
    q.addBindValue(rec.actualHash.isEmpty() ? QVariant() : QVariant(rec.actualHash));
    const QString verStr = verificationStatusToString(rec.verification);
    q.addBindValue(verStr.isEmpty() ? QVariant() : QVariant(verStr));
    q.addBindValue(rec.requestHeaders.isEmpty() ? QVariant() : QVariant(rec.requestHeaders));
    exec(q, "insertDownload");
    return q.lastInsertId().toLongLong();
}

void Database::updateExpectedHash(qint64 id, const QString& hash,
                                  const QString& algorithm, const QString& source) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "UPDATE downloads SET expected_hash = ?, hash_algorithm = ?, hash_source = ?,"
        " verification = COALESCE(verification, 'pending'), updated_at = ? WHERE id = ?");
    q.addBindValue(hash.isEmpty() ? QVariant() : QVariant(hash));
    q.addBindValue(algorithm.isEmpty() ? QVariant() : QVariant(algorithm));
    q.addBindValue(source.isEmpty() ? QVariant() : QVariant(source));
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "updateExpectedHash");
}

void Database::updateVerificationResult(qint64 id, const QString& actualHash,
                                        VerificationStatus result) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "UPDATE downloads SET actual_hash = ?, verification = ?, updated_at = ? WHERE id = ?");
    q.addBindValue(actualHash.isEmpty() ? QVariant() : QVariant(actualHash));
    const QString verStr = verificationStatusToString(result);
    q.addBindValue(verStr.isEmpty() ? QVariant() : QVariant(verStr));
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "updateVerificationResult");
}

void Database::updateVerificationStatus(qint64 id, VerificationStatus s) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare("UPDATE downloads SET verification = ?, updated_at = ? WHERE id = ?");
    const QString verStr = verificationStatusToString(s);
    q.addBindValue(verStr.isEmpty() ? QVariant() : QVariant(verStr));
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "updateVerificationStatus");
}

void Database::updateDownloadStatus(qint64 id, DownloadStatus s, const QString& error) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare("UPDATE downloads SET status = ?, error = ?, updated_at = ? WHERE id = ?");
    q.addBindValue(downloadStatusToString(s));
    q.addBindValue(error.isEmpty() ? QVariant() : QVariant(error));
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "updateDownloadStatus");
}

void Database::updateDownloadTotals(qint64 id, qint64 totalBytes, bool supportsRanges) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "UPDATE downloads SET total_bytes = ?, supports_ranges = ?, updated_at = ? WHERE id = ?");
    q.addBindValue(static_cast<qlonglong>(totalBytes));
    q.addBindValue(supportsRanges ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "updateDownloadTotals");
}

void Database::deleteDownload(qint64 id) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare("DELETE FROM downloads WHERE id = ?");
    q.addBindValue(static_cast<qlonglong>(id));
    exec(q, "deleteDownload");
}

void Database::replaceChunks(qint64 downloadId, const QList<ChunkRecord>& chunks) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    if (!db.transaction()) {
        throw std::runtime_error("replaceChunks: begin transaction failed");
    }
    try {
        {
            QSqlQuery del(db);
            del.prepare("DELETE FROM chunks WHERE download_id = ?");
            del.addBindValue(static_cast<qlonglong>(downloadId));
            exec(del, "deleteChunks");
        }
        QSqlQuery ins(db);
        ins.prepare(
            "INSERT INTO chunks"
            " (download_id, chunk_index, start_byte, end_byte, bytes_received, attempts, status)"
            " VALUES (?, ?, ?, ?, ?, ?, ?)");
        for (const ChunkRecord& c : chunks) {
            ins.bindValue(0, static_cast<qlonglong>(downloadId));
            ins.bindValue(1, c.index);
            ins.bindValue(2, static_cast<qlonglong>(c.startByte));
            ins.bindValue(3, static_cast<qlonglong>(c.endByte));
            ins.bindValue(4, static_cast<qlonglong>(c.bytesReceived));
            ins.bindValue(5, c.attempts);
            ins.bindValue(6, chunkStatusToString(c.status));
            exec(ins, "insertChunk");
        }
        if (!db.commit()) throw std::runtime_error("replaceChunks: commit failed");
    } catch (...) {
        db.rollback();
        throw;
    }
}

void Database::updateChunkProgress(qint64 downloadId, const QList<ChunkRecord>& chunks) {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    if (!db.transaction()) {
        throw std::runtime_error("updateChunkProgress: begin transaction failed");
    }
    try {
        QSqlQuery upd(db);
        upd.prepare(
            "UPDATE chunks SET bytes_received = ?, attempts = ?, status = ?"
            " WHERE download_id = ? AND chunk_index = ?");
        for (const ChunkRecord& c : chunks) {
            upd.bindValue(0, static_cast<qlonglong>(c.bytesReceived));
            upd.bindValue(1, c.attempts);
            upd.bindValue(2, chunkStatusToString(c.status));
            upd.bindValue(3, static_cast<qlonglong>(downloadId));
            upd.bindValue(4, c.index);
            exec(upd, "updateChunk");
        }
        if (!db.commit()) throw std::runtime_error("updateChunkProgress: commit failed");
    } catch (...) {
        db.rollback();
        throw;
    }
}

namespace {

constexpr const char* kDownloadSelectColumns =
    "id, url, output_path, total_bytes, supports_ranges, status, error,"
    " created_at, updated_at, expected_hash, hash_algorithm, hash_source,"
    " actual_hash, verification, request_headers";

DownloadRecord readDownloadRow(const QSqlQuery& q) {
    DownloadRecord r;
    r.id = q.value(0).toLongLong();
    r.url = q.value(1).toString();
    r.outputPath = q.value(2).toString();
    r.totalBytes = q.value(3).toLongLong();
    r.supportsRanges = q.value(4).toInt() != 0;
    r.status = downloadStatusFromString(q.value(5).toString());
    r.error = q.value(6).toString();
    r.createdAt = q.value(7).toLongLong();
    r.updatedAt = q.value(8).toLongLong();
    r.expectedHash = q.value(9).toString();
    r.hashAlgorithm = q.value(10).toString();
    r.hashSource = q.value(11).toString();
    r.actualHash = q.value(12).toString();
    r.verification = verificationStatusFromString(q.value(13).toString());
    r.requestHeaders = q.value(14).toString();
    return r;
}

}  // namespace

QList<DownloadRecord> Database::listDownloads() const {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    if (!q.exec(QString("SELECT ") + kDownloadSelectColumns +
                " FROM downloads ORDER BY id ASC")) {
        throw std::runtime_error(
            QString("listDownloads: %1").arg(q.lastError().text()).toStdString());
    }
    QList<DownloadRecord> out;
    while (q.next()) out.append(readDownloadRow(q));
    return out;
}

std::optional<DownloadRecord> Database::getDownload(qint64 id) const {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(QString("SELECT ") + kDownloadSelectColumns + " FROM downloads WHERE id = ?");
    q.addBindValue(static_cast<qlonglong>(id));
    if (!q.exec()) {
        throw std::runtime_error(
            QString("getDownload: %1").arg(q.lastError().text()).toStdString());
    }
    if (!q.next()) return std::nullopt;
    return readDownloadRow(q);
}

QList<ChunkRecord> Database::chunksFor(qint64 downloadId) const {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "SELECT chunk_index, start_byte, end_byte, bytes_received, attempts, status"
        " FROM chunks WHERE download_id = ? ORDER BY chunk_index ASC");
    q.addBindValue(static_cast<qlonglong>(downloadId));
    if (!q.exec()) {
        throw std::runtime_error(
            QString("chunksFor: %1").arg(q.lastError().text()).toStdString());
    }
    QList<ChunkRecord> out;
    while (q.next()) {
        ChunkRecord c;
        c.index = q.value(0).toInt();
        c.startByte = q.value(1).toLongLong();
        c.endByte = q.value(2).toLongLong();
        c.bytesReceived = q.value(3).toLongLong();
        c.attempts = q.value(4).toInt();
        c.status = chunkStatusFromString(q.value(5).toString());
        out.append(c);
    }
    return out;
}

int Database::markInterruptedAsPaused() {
    QSqlDatabase db = QSqlDatabase::database(connectionName_);
    QSqlQuery q(db);
    q.prepare(
        "UPDATE downloads SET status = 'paused', updated_at = ?"
        " WHERE status = 'active'");
    q.addBindValue(static_cast<qlonglong>(nowSeconds()));
    if (!q.exec()) {
        throw std::runtime_error(
            QString("markInterruptedAsPaused: %1").arg(q.lastError().text()).toStdString());
    }
    return q.numRowsAffected();
}

}  // namespace fdm::store
